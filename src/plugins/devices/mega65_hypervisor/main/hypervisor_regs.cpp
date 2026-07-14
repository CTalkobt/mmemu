#include "hypervisor_regs.h"

HypervisorRegs::HypervisorRegs(MOS45GS02* cpu) : m_cpu(cpu) {}

bool HypervisorRegs::handleDosTrap() {
    if (!m_hdosTrap) return false;

    // The A register contains the function code (func = A & 0x7E)
    uint8_t funcCode = m_cpu->regRead(0) & 0x7E; // A register, mask to even

    // Enter hypervisor to save user state
    m_cpu->enterHypervisor(0x8000);

    // Try to virtualize the function
    if (m_hdosTrap(funcCode, m_cpu)) {
        // Virtualized — exit hypervisor with the handler's register values
        m_cpu->exitHypervisor();
        return true;
    }

    // Not virtualized — hypervisor is entered, HYPPO's trap handler at $8000 runs.
    // Return true because enterHypervisor already happened.
    return true;
}

bool HypervisorRegs::ioRead(IBus* /*bus*/, uint32_t addr, uint8_t* val) {
    if ((addr & ~(uint32_t)0x3F) != 0xD640) return false;
    uint8_t off = addr & 0x3F;

    // In user mode: reads return $FF (registers not accessible)
    if (!m_cpu->isHypervisor()) {
        *val = 0xFF;
        return true;
    }

    auto& h = m_cpu->hyperState();
    switch (off) {
        case 0x00: *val = h.regA;   break; // $D640
        case 0x01: *val = h.regX;   break; // $D641
        case 0x02: *val = h.regY;   break; // $D642
        case 0x03: *val = h.regZ;   break; // $D643
        case 0x04: *val = h.regB;   break; // $D644
        case 0x05: *val = h.spl;    break; // $D645
        case 0x06: *val = h.sph;    break; // $D646
        case 0x07: *val = h.pflags; break; // $D647
        case 0x08: *val = h.pc & 0xFF; break; // $D648 PCL
        case 0x09: *val = (h.pc >> 8) & 0xFF; break; // $D649 PCH
        case 0x0A: *val = h.mapOffsets[0][0];  break; // $D64A (block 0 offset byte 0)
        case 0x0B: *val = h.mapOffsets[0][1];  break; // $D64B (block 0 offset byte 1)
        case 0x0C: *val = h.mapOffsets[4][0];  break; // $D64C (block 4 offset byte 0)
        case 0x0D: *val = h.mapOffsets[4][1];  break; // $D64D (block 4 offset byte 1)
        case 0x0E: *val = (h.mapEnables & 0x0F); break; // $D64E (lower block enables)
        case 0x0F: *val = ((h.mapEnables >> 4) & 0x0F); break; // $D64F (upper block enables)
        case 0x10: *val = h.port00;  break; // $D650
        case 0x11: *val = h.port01;  break; // $D651
        case 0x12: *val = h.vicMode; break; // $D652
        // $D653-$D659: DMAgic & SD state (not yet fully implemented)
        // These registers are for saving DMA list addresses, bank megabytes, and SD virtualization state
        // For now, return 0 as stub values
        case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x19:
            *val = 0;  // $D653-$D659: DMAgic/SD state stub
            break;
        default:   *val = 0; break;
    }
    return true;
}

bool HypervisorRegs::ioWrite(IBus* /*bus*/, uint32_t addr, uint8_t val) {
    if ((addr & ~(uint32_t)0x3F) != 0xD640) return false;
    uint8_t off = addr & 0x3F;

    if (!m_cpu->isHypervisor()) {
        // User mode: write to $D640-$D67F triggers SYSCALL trap.
        // Validate that the opcode at current PC is NOP ($EA) or CLV ($B8).
        // This matches real MEGA65 hardware behavior — the STA $D640+n
        // instruction must be followed by NOP or CLV for the trap to fire.
        IBus* cpuBus = m_cpu->getDataBus();
        if (cpuBus) {
            uint8_t nextOp = cpuBus->peek8(m_cpu->pc());
            if (nextOp != 0xEA && nextOp != 0xB8) {
                return true;  // Silently ignore invalid trap sequence
            }
        }

        // SYSCALL number = offset (0-63), entry point = $8000 + off*4
        // Trap 0 (DOS): try HDOS virtualization first
        if (off == 0x00 && handleDosTrap()) {
            return true;  // Virtualized — hypervisor entered and exited
        }

        uint16_t trapAddr = 0x8000 + (uint16_t)off * 4;
        m_cpu->enterHypervisor(trapAddr);
        return true;
    }

    // Hypervisor mode: write to virtualisation control registers
    auto& h = m_cpu->hyperState();
    switch (off) {
        case 0x00: h.regA   = val; break;
        case 0x01: h.regX   = val; break;
        case 0x02: h.regY   = val; break;  // $D642
        case 0x03: h.regZ   = val; break;
        case 0x04: h.regB   = val; break;
        case 0x05: h.spl    = val; break;
        case 0x06: h.sph    = val; break;
        case 0x07: h.pflags = val; break;
        case 0x08: h.pc = (h.pc & 0xFF00) | val; break;
        case 0x09: h.pc = (h.pc & 0x00FF) | ((uint16_t)val << 8); break;
        case 0x0A: h.mapOffsets[0][0] = val; break;  // $D64A (block 0 offset byte 0)
        case 0x0B: h.mapOffsets[0][1] = val; break;  // $D64B (block 0 offset byte 1)
        case 0x0C: h.mapOffsets[4][0] = val; break;  // $D64C (block 4 offset byte 0)
        case 0x0D: h.mapOffsets[4][1] = val; break;  // $D64D (block 4 offset byte 1)
        case 0x0E: h.mapEnables = (h.mapEnables & 0xF0) | (val & 0x0F); break;  // $D64E (lower enables)
        case 0x0F: h.mapEnables = (h.mapEnables & 0x0F) | ((val & 0x0F) << 4); break;  // $D64F (upper enables)
        case 0x10: h.port00  = val; break;
        case 0x11: h.port01  = val; break;
        case 0x12: h.vicMode = val; break;
        // $D653-$D659: DMAgic & SD state (not yet fully implemented)
        // These registers are for saving DMA list addresses, bank megabytes, and SD virtualization state
        // For now, writes are silently ignored (stub implementation)
        case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x19:
            // $D653-$D659: DMAgic/SD state stub — write ignored for now
            break;

        case 0x3F: // $D67F — ENTEREXIT: exit hypervisor mode
            m_cpu->exitHypervisor();
            break;

        default: break;
    }
    return true;
}
