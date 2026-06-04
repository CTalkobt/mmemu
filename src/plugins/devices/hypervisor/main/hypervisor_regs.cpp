#include "hypervisor_regs.h"

HypervisorRegs::HypervisorRegs(MOS45GS02* cpu) : m_cpu(cpu) {}

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
        case 0x02: *val = 0;        break; // $D642 (REGY — not saved separately)
        case 0x03: *val = h.regZ;   break; // $D643
        case 0x04: *val = h.regB;   break; // $D644
        case 0x05: *val = h.spl;    break; // $D645
        case 0x06: *val = h.sph;    break; // $D646
        case 0x07: *val = h.pflags; break; // $D647
        case 0x08: *val = h.pc & 0xFF; break; // $D648 PCL
        case 0x09: *val = (h.pc >> 8) & 0xFF; break; // $D649 PCH
        case 0x0A: *val = h.mapLo0;  break; // $D64A
        case 0x0B: *val = h.mapLo1;  break; // $D64B
        case 0x0C: *val = h.mapHi0;  break; // $D64C
        case 0x0D: *val = h.mapHi1;  break; // $D64D
        case 0x0E: *val = h.mapLoMB; break; // $D64E
        case 0x0F: *val = h.mapHiMB; break; // $D64F
        case 0x10: *val = h.port00;  break; // $D650
        case 0x11: *val = h.port01;  break; // $D651
        case 0x12: *val = h.vicMode; break; // $D652
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
        uint16_t trapAddr = 0x8000 + (uint16_t)off * 4;
        m_cpu->enterHypervisor(trapAddr);
        return true;
    }

    // Hypervisor mode: write to virtualisation control registers
    auto& h = m_cpu->hyperState();
    switch (off) {
        case 0x00: h.regA   = val; break;
        case 0x01: h.regX   = val; break;
        case 0x03: h.regZ   = val; break;
        case 0x04: h.regB   = val; break;
        case 0x05: h.spl    = val; break;
        case 0x06: h.sph    = val; break;
        case 0x07: h.pflags = val; break;
        case 0x08: h.pc = (h.pc & 0xFF00) | val; break;
        case 0x09: h.pc = (h.pc & 0x00FF) | ((uint16_t)val << 8); break;
        case 0x0A: h.mapLo0  = val; break;
        case 0x0B: h.mapLo1  = val; break;
        case 0x0C: h.mapHi0  = val; break;
        case 0x0D: h.mapHi1  = val; break;
        case 0x0E: h.mapLoMB = val; break;
        case 0x0F: h.mapHiMB = val; break;
        case 0x10: h.port00  = val; break;
        case 0x11: h.port01  = val; break;
        case 0x12: h.vicMode = val; break;

        case 0x3F: // $D67F — ENTEREXIT: exit hypervisor mode
            m_cpu->exitHypervisor();
            break;

        default: break;
    }
    return true;
}
