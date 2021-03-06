/*
 * Copyright (C) 2015 Niek Linnenbank
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <FreeNOS/System.h>
#include <Log.h>
#include <ListIterator.h>
#include <SplitAllocator.h>
#include <BubbleAllocator.h>
#include <PoolAllocator.h>
#include <IntController.h>
#include <BootImage.h>
#include <CoreInfo.h>
#include "Kernel.h"
#include "Memory.h"
#include "Process.h"
#include "ProcessManager.h"

Kernel::Kernel(CoreInfo *info)
    : Singleton<Kernel>(this), m_interrupts(256)
{
    // Output log banners
    if (Log::instance)
    {
        Log::instance->append(BANNER);
        Log::instance->append(COPYRIGHT "\r\n");
    }

    // Initialize members
    Arch::MemoryMap map;
    Memory::Range kernelData = map.range(MemoryMap::KernelData);

    const Allocator::Range physRange = { info->memory.phys, info->memory.size, 0 };
    const Allocator::Range virtRange = { kernelData.virt, kernelData.size, 0 };
    m_alloc  = new SplitAllocator(physRange, virtRange, PAGESIZE);
    m_procs  = new ProcessManager();
    m_api    = new API();
    m_coreInfo   = info;
    m_intControl = ZERO;
    m_timer      = ZERO;

    // Mark first 4MB in phys memory used
    for (Size i = 0; i < (1024*1024*4); i += PAGESIZE)
        m_alloc->allocate(info->memory.phys + i);

    // Mark all kernel memory used
    for (Size i = 0; i < info->kernel.size; i += PAGESIZE)
        m_alloc->allocate(info->kernel.phys + i);

    // Mark BootImage memory used
    for (Size i = 0; i < m_coreInfo->bootImageSize; i += PAGESIZE)
        m_alloc->allocate(m_coreInfo->bootImageAddress + i);

    // Mark heap memory used
    for (Size i = 0; i < m_coreInfo->heapSize; i += PAGESIZE)
        m_alloc->allocate(m_coreInfo->heapAddress + i);

    // Reserve CoreChannel memory
    for (Size i = 0; i < m_coreInfo->coreChannelSize; i += PAGESIZE)
        m_alloc->allocate(m_coreInfo->coreChannelAddress + i);

    // Clear interrupts table
    m_interrupts.fill(ZERO);
}

Error Kernel::heap(Address base, Size size)
{
    Size metaData = sizeof(BubbleAllocator) + sizeof(PoolAllocator);
    Allocator *bubble, *pool;
    const Allocator::Range bubbleRange = { base + metaData, size - metaData, sizeof(u32) };
    const Allocator::Range poolRange   = { 0, size - metaData, sizeof(u32) };

    // Clear the heap first
    MemoryBlock::set((void *) base, 0, size);

    // Setup the dynamic memory heap
    bubble = new (base) BubbleAllocator(bubbleRange);
    pool   = new (base + sizeof(BubbleAllocator)) PoolAllocator(poolRange);
    pool->setParent(bubble);

    // Set default allocator
    Allocator::setDefault(pool);
    return 0;
}

SplitAllocator * Kernel::getAllocator()
{
    return m_alloc;
}

ProcessManager * Kernel::getProcessManager()
{
    return m_procs;
}

API * Kernel::getAPI()
{
    return m_api;
}

MemoryContext * Kernel::getMemoryContext()
{
    return m_procs->current()->getMemoryContext();
}

CoreInfo * Kernel::getCoreInfo()
{
    return m_coreInfo;
}

Timer * Kernel::getTimer()
{
    return m_timer;
}

void Kernel::enableIRQ(u32 irq, bool enabled)
{
    if (m_intControl)
    {
        if (enabled)
            m_intControl->enable(irq);
        else
            m_intControl->disable(irq);
    }
}

Kernel::Result Kernel::sendIRQ(const uint coreId, const uint irq)
{
    if (m_intControl)
    {
        IntController::Result r = m_intControl->send(coreId, irq);
        if (r != IntController::Success)
        {
            ERROR("failed to send IPI to core" << coreId << ": " << (uint) r);
            return IOError;
        }
    }

    return Success;
}

void Kernel::hookIntVector(u32 vec, InterruptHandler h, ulong p)
{
    InterruptHook hook(h, p);

    // Insert into interrupts; create List if neccesary
    if (!m_interrupts[vec])
    {
        m_interrupts.insert(vec, new List<InterruptHook *>());
    }
    // Just append it. */
    if (!m_interrupts[vec]->contains(&hook))
    {
        m_interrupts[vec]->append(new InterruptHook(h, p));
    }
}

void Kernel::executeIntVector(u32 vec, CPUState *state)
{
    // Auto-Mask the IRQ. Any interrupt handler or user program
    // needs to re-enable the IRQ to receive it again. This prevents
    // interrupt loops in case the kernel cannot clear the IRQ immediately.
    enableIRQ(vec, false);

    // Fetch the list of interrupt hooks (for this vector)
    List<InterruptHook *> *lst = m_interrupts[vec];
    if (lst)
    {
        // Execute them all
        for (ListIterator<InterruptHook *> i(lst); i.hasCurrent(); i++)
        {
            i.current()->handler(state, i.current()->param, vec);
        }
    }

    // Raise any interrupt notifications for processes. Note that the IRQ
    // base should be subtracted, since userspace doesn't know about re-mapped
    // IRQ's, such as is done for the PIC on intel
    if (m_procs->interruptNotify(vec - m_intControl->getBase()) != ProcessManager::Success)
    {
        FATAL("failed to raise interrupt notification for IRQ #" << vec);
    }
}

Kernel::Result Kernel::loadBootImage()
{
    BootImage *image = (BootImage *) (m_alloc->toVirtual(m_coreInfo->bootImageAddress));

    NOTICE("bootimage: " << (void *) image <<
           " (" << (unsigned) m_coreInfo->bootImageSize << " bytes)");

    // Verify this is a correct BootImage
    if (image->magic[0] == BOOTIMAGE_MAGIC0 &&
        image->magic[1] == BOOTIMAGE_MAGIC1 &&
        image->layoutRevision == BOOTIMAGE_REVISION)
    {
        // Loop BootPrograms
        for (Size i = 0; i < image->symbolTableCount; i++)
            loadBootProcess(image, m_coreInfo->bootImageAddress, i);

        return Success;
    }
    ERROR("invalid boot image signature: " << (unsigned) image->magic[0] << ", " << (unsigned) image->magic[1]);
    return InvalidBootImage;
}

Kernel::Result Kernel::loadBootProcess(BootImage *image, Address imagePAddr, Size index)
{
    Address imageVAddr = (Address) image;
    BootSymbol *program;
    BootSegment *segment;
    Process *proc;
    char *vaddr;
    Arch::MemoryMap map;
    Memory::Range argRange;
    Allocator::Range alloc_args;

    // Point to the program and segments table
    program = &((BootSymbol *) (imageVAddr + image->symbolTableOffset))[index];
    segment = &((BootSegment *) (imageVAddr + image->segmentsTableOffset))[program->segmentsOffset];

    // Ignore non-BootProgram entries
    if (program->type != BootProgram && program->type != BootPrivProgram)
        return InvalidBootImage;

    // Create process
    proc = m_procs->create(program->entry, map, true, program->type == BootPrivProgram);
    if (!proc)
    {
        FATAL("failed to create boot program: " << program->name);
        return ProcessError;
    }

    // Obtain process memory
    MemoryContext *mem = proc->getMemoryContext();

    // Map program segment into it's virtual memory
    for (Size i = 0; i < program->segmentsCount; i++)
    {
        for (Size j = 0; j < segment[i].size; j += PAGESIZE)
        {
            mem->map(segment[i].virtualAddress + j,
                     imagePAddr + segment[i].offset + j,
                     Memory::User     |
                     Memory::Readable |
                     Memory::Writable |
                     Memory::Executable);
        }
    }

    // Allocate page for program arguments
    argRange = map.range(MemoryMap::UserArgs);
    argRange.access = Memory::User | Memory::Readable | Memory::Writable;
    alloc_args.address = 0;
    alloc_args.size = argRange.size;
    alloc_args.alignment = PAGESIZE;

    if (m_alloc->allocate(alloc_args) != Allocator::Success)
    {
        FATAL("failed to allocate program arguments page");
        return ProcessError;
    }
    argRange.phys = alloc_args.address;

    // Map program arguments in the process
    if (mem->mapRange(&argRange) != MemoryContext::Success)
    {
        FATAL("failed to map program arguments page");
        return ProcessError;
    }

    // Copy program arguments
    vaddr = (char *) m_alloc->toVirtual(argRange.phys);
    MemoryBlock::set(vaddr, 0, argRange.size);
    MemoryBlock::copy(vaddr, program->name, BOOTIMAGE_NAMELEN);

    // Done
    NOTICE("loaded: " << program->name);
    return Success;
}

int Kernel::run()
{
    NOTICE("");

    // Load boot image programs
    loadBootImage();

    // Start the scheduler
    m_procs->schedule();

    // Never actually returns.
    return 0;
}
