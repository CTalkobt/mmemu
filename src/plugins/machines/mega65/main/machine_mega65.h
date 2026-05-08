#pragma once

#include "libcore/main/machine_desc.h"

/**
 * MEGA65 Machine Factory
 *
 * Creates a MEGA65 machine with:
 * - 45GS02 CPU (16-bit virtual address space)
 * - SparseMemoryBus (28-bit physical address space)
 * - MapMmu (address translator between CPU and physical bus)
 * - F018B DMA controller
 * - VIC-IV video chip
 * - SID synthesizers
 * - MEGA65-specific I/O (address translator, fast I/O)
 */

class Mega65MachineFactory {
public:
    static MachineDescriptor* create();
};
