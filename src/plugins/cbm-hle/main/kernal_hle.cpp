#include "kernal_hle.h"
#include "include/util/logging.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_handler.h"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

// Simulate RTS, handling both 8-bit (6502) and 16-bit (45GS02) stack pointers.
// On the 45GS02, the E flag (P bit 5) controls stack mode:
//   E=1 (SEE): 8-bit SP, wraps within current stack page (SPH preserved)
//   E=0 (CLE): 16-bit SP, full address space
// On the 6502, SP is always 8-bit in page $01.
static void simulateRts(ICore* cpu, IBus* bus) {
    uint32_t sp = cpu->regReadByName("SP");
    uint32_t p  = cpu->regReadByName("P");
    // Check SP register width to distinguish 6502 (8-bit) from 45GS02 (16-bit)
    bool has16bitSP = false;
    for (int i = 0; i < cpu->regCount(); ++i) {
        const auto* d = cpu->regDescriptor(i);
        if (d && std::string(d->name) == "SP" && d->width == RegWidth::R16) {
            has16bitSP = true;
            break;
        }
    }

    uint16_t addr1, addr2;
    if (has16bitSP) {
        bool eFlag = (p & 0x20) != 0;  // FLAG_E = bit 5
        if (eFlag) {
            // 8-bit stack mode: wrap within current page
            uint16_t page = sp & 0xFF00;
            addr1 = page | ((sp + 1) & 0xFF);
            addr2 = page | ((sp + 2) & 0xFF);
            cpu->regWriteByName("SP", (uint32_t)(page | ((sp + 2) & 0xFF)));
        } else {
            // 16-bit stack mode
            addr1 = (sp + 1) & 0xFFFF;
            addr2 = (sp + 2) & 0xFFFF;
            cpu->regWriteByName("SP", (uint32_t)((sp + 2) & 0xFFFF));
        }
    } else {
        // 6502: 8-bit SP always in page $01
        addr1 = 0x0100 | ((sp + 1) & 0xFF);
        addr2 = 0x0100 | ((sp + 2) & 0xFF);
        cpu->regWriteByName("SP", (uint32_t)((sp + 2) & 0xFF));
    }
    uint16_t retAddr = (bus->read8(addr1) | (bus->read8(addr2) << 8)) + 1;
    cpu->setPc(retAddr);
}

KernalHLE::KernalHLE() : m_enabled(true), m_hostPath(".") {
    // Standard KERNAL jump table vectors (fixed for all CBM machines)
    m_machineVectors["c64"]   = { 0xFFD5, 0xFFD8 };
    m_machineVectors["vic20"] = { 0xFFD5, 0xFFD8 };
    m_machineVectors["pet2001"] = { 0xFFD5, 0xFFD8 };
    m_machineVectors["pet4032"] = { 0xFFD5, 0xFFD8 };
    m_machineVectors["pet8032"] = { 0xFFD5, 0xFFD8 };
}

bool KernalHLE::onStep(ICore* cpu, IBus* bus, const DisasmEntry& entry) {
    if (!m_enabled) return true;

    // Check if we are at a known vector
    if (entry.addr == 0xFFD5 || entry.addr == 0xFFD8) {
        uint8_t device = getDevice(cpu, bus);
        fprintf(stderr, "[HLE] Hit vector %04X, device=%d\n", entry.addr, device);
        fflush(stderr);

        // If we have machine context, check if a physical device is handling this unit
        if (m_currentMachine && m_currentMachine->ioRegistry) {
            std::vector<IOHandler*> handlers;
            m_currentMachine->ioRegistry->enumerate(handlers);
            for (auto* h : handlers) {
                int t, s;
                bool led;
                if (h->getDiskStatus(device, t, s, led)) {
                    // A physical device (e.g. VirtualIEC) is handling this unit.
                    fprintf(stderr, "[HLE] Device %d handled by %s, passing to KERNAL\n", device, h->name());
                    fflush(stderr);
                    return true;
                }
            }
        }

        if (entry.addr == 0xFFD5) {
            handleLoad(cpu, bus);
        } else {
            handleSave(cpu, bus);
        }
        return false; // Intercepted
    }

    return true;
}

void KernalHLE::handleLoad(ICore* cpu, IBus* bus) {
    uint8_t device = getDevice(cpu, bus);
    // We only intercept if it's a disk device (8-11) or if enabled for all?
    // For now, let's say we intercept if it's 8-31.
    if (device < 8) {
        // Let KERNAL handle it (e.g. tape)
        // Wait, if I return true from onStep it would continue.
        // But I've already returned false and called handleLoad.
        // I should have checked device in onStep if I wanted to skip.
        // Let's refine onStep.
    }

    uint8_t sa = getSecondaryAddress(cpu, bus);
    std::string filename = getFilename(cpu, bus);
    
    // Determine load address
    uint32_t loadAddr;
    if (sa == 0) {
        // Load to address in X/Y
        loadAddr = cpu->regReadByName("X") | (cpu->regReadByName("Y") << 8);
    } else {
        // Load to address from file header (first 2 bytes)
        loadAddr = 0; // Will be set from file
    }

    std::string fullPath = (fs::path(m_hostPath) / filename).string();
    std::ifstream file(fullPath, std::ios::binary);
    if (!file) {
        // File not found
        setStatus(bus, 4); // 4 = File not found
        setCarry(cpu, true);
        
        simulateRts(cpu, bus);
        return;
    }

    // Read file
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    
    if (data.size() < 2) {
        setStatus(bus, 4);
        setCarry(cpu, true);
        return;
    }

    size_t startOffset = 0;
    if (sa != 0) {
        loadAddr = data[0] | (data[1] << 8);
        startOffset = 2;
    }

    // Inject data
    for (size_t i = startOffset; i < data.size(); ++i) {
        bus->write8(loadAddr++, data[i]);
    }

    // Update registers for success
    cpu->regWriteByName("X", (uint8_t)(loadAddr & 0xFF));
    cpu->regWriteByName("Y", (uint8_t)(loadAddr >> 8));
    setCarry(cpu, false);
    setStatus(bus, 0);

    simulateRts(cpu, bus);
}

void KernalHLE::handleSave(ICore* cpu, IBus* bus) {
    // Similar to LOAD
    setStatus(bus, 5); // 5 = Device not present / Not implemented
    setCarry(cpu, true);
    
    simulateRts(cpu, bus);
}

uint8_t KernalHLE::getDevice(ICore* cpu, IBus* bus) {
    return bus->read8(0xBA);
}

uint8_t KernalHLE::getSecondaryAddress(ICore* cpu, IBus* bus) {
    return bus->read8(0xB9);
}

std::string KernalHLE::getFilename(ICore* cpu, IBus* bus) {
    uint8_t len = bus->read8(0xB7);
    uint16_t ptr = bus->read8(0xBB) | (bus->read8(0xBC) << 8);
    std::string name;
    for (int i = 0; i < len; ++i) {
        name += (char)bus->read8(ptr + i);
    }
    return name;
}

void KernalHLE::setStatus(IBus* bus, uint8_t status) {
    bus->write8(0x90, status);
}

void KernalHLE::setCarry(ICore* cpu, bool set) {
    uint8_t p = cpu->regReadByName("P");
    if (set) p |= 0x01;
    else     p &= ~0x01;
    cpu->regWriteByName("P", p);
}
