// The TMS7000 (TMS7040), after MAME's src/devices/cpu/tms7000/tms7000.cpp
// and tms7000op.cpp (hap, Tim Lindner; BSD-3-Clause).

#include "tms7000.h"

#include <cstring>

Tms7000::Tms7000() = default;

void Tms7000::setRom(std::vector<uint8_t> rom) { m_rom = std::move(rom); }

// ---------------------------------------------------------------------------
// The address space
// ---------------------------------------------------------------------------
uint8_t Tms7000::peek(uint16_t a) const {
    if (a < 0x80) return m_rf[a];
    if (a >= 0x100 && a < 0x10C) {
        switch (a & 0xFF) {
            case 0x00: return m_ioControl;
            case 0x02: return (uint8_t)m_timerDec;
            case 0x03: return m_timerCapture;
            case 0x05: case 0x09: case 0x0B: return m_portDdr[(a & 0xFF) / 2 - 2];
            case 0x06: return m_portLatch[1];
            default: return 0;
        }
    }
    if (!m_rom.empty() && a >= 0x10000 - m_rom.size()) return m_rom[a - (0x10000 - m_rom.size())];
    return 0;
}

uint8_t Tms7000::read(uint16_t a) {
    if (a < 0x80) return m_rf[a];
    if (a >= 0x100 && a < 0x10C) return pfRead((uint8_t)(a & 0xFF));
    return peek(a);
}

void Tms7000::write(uint16_t a, uint8_t v) {
    if (a < 0x80) { m_rf[a] = v; return; }
    if (a >= 0x100 && a < 0x10C) pfWrite((uint8_t)(a & 0xFF), v);
}

// The peripheral file ($0100-$010B on the 70x0)
uint8_t Tms7000::pfRead(uint8_t off) {
    switch (off) {
        case 0x00: return m_ioControl;
        case 0x02: return (uint8_t)m_timerDec;          // the decrementer now
        case 0x03: return m_timerCapture;               // latched by INT3
        case 0x04: case 0x06: case 0x08: case 0x0A: {
            // (port B is output only: it reads its latch, as if its DDR were $FF)
            const int port = off / 2 - 2;
            const uint8_t pins = portIn ? portIn(port) : 0xFF;
            return (uint8_t)((pins & ~m_portDdr[port]) | (m_portLatch[port] & m_portDdr[port]));
        }
        case 0x05: case 0x09: case 0x0B: return m_portDdr[off / 2 - 2];
        default: return 0;
    }
}

void Tms7000::pfWrite(uint8_t off, uint8_t v) {
    switch (off) {
        case 0x00:
            // IOCNT0: d0/d2/d4 INT1/2/3 enable; d1/d3/d5 their flags (1 clears)
            m_ioControl = (uint8_t)((m_ioControl & (~v & 0x2A)) | (v & 0xD5));
            if (v & 0x02) flagExt(0);
            if (v & 0x20) flagExt(1);
            checkInterrupts();
            break;
        case 0x02: m_timerData = v; break;              // the reload value
        case 0x03:
            // d0-d4 prescale, d6 source (external: not modelled), d7 start
            m_timerControl = (uint8_t)(v & ~0x20);
            timerReload();
            break;
        case 0x06: case 0x08: case 0x0A: {
            const int port = off / 2 - 2;
            if (portOut) portOut(port, (uint8_t)(v & m_portDdr[port]));
            m_portLatch[port] = v;
            break;
        }
        case 0x09: case 0x0B: m_portDdr[off / 2 - 2] = v; break;
        default: break;                                 // port A and its DDR: not on the 70x0
    }
}

// ---------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------
void Tms7000::setInput(int line, bool on) {
    if (m_irq[line] == on) return;
    m_irq[line] = on;
    flagExt(line);
    // (taken at the next step(): MAME checks here, inside its time slice)
    if (on && line == 1) m_timerCapture = (uint8_t)m_timerDec;
}

void Tms7000::flagExt(int line) {
    const uint8_t flag = (uint8_t)(0x02 << (4 * line));
    if (m_irq[line]) m_ioControl |= flag;
    else m_ioControl &= (uint8_t)~flag;
}

void Tms7000::checkInterrupts() {
    if (!(m_sr & SR_I)) return;
    for (int line = 0; line < 3; ++line) {             // INT1, INT2 (timer 1), INT3
        const int shift = line * 2;
        if ((m_ioControl >> shift & 3) == 3) {
            m_ioControl &= (uint8_t)~(0x02 << shift);
            if (line == 0 || line == 2) flagExt(line / 2);   // still asserted: flagged again
            doInterrupt(line);
            return;
        }
    }
}

void Tms7000::doInterrupt(int line) {
    if (m_idle) {
        m_cycles += 17;
        m_pc++;
        m_idle = false;
    } else {
        m_cycles += 19;
    }
    push8(m_sr);
    push16(m_pc);
    m_sr = 0;
    m_pc = rm16((uint16_t)(0xFFFC - line * 2));
}

// ---------------------------------------------------------------------------
// Timer 1: every 16 x (prescale + 1) oscillator clocks (8 x that in cycles)
// ---------------------------------------------------------------------------
void Tms7000::timerReload() {
    m_timerRunning = false;
    if (m_timerControl & 0x80) {
        m_timerDec = m_timerData;
        if ((m_timerControl & 0xE0) == 0x80) {
            m_timerRunning = true;
            m_timerLeft = 8 * ((m_timerControl & 0x1F) + 1);
        }
    }
}

void Tms7000::timerTickLow() {
    if (--m_timerDec < 0) {
        timerReload();
        m_ioControl |= 0x08;                            // INT2
    }
}

void Tms7000::timerAdvance(int cycles) {
    while (m_timerRunning && cycles > 0) {
        if (cycles < m_timerLeft) { m_timerLeft -= cycles; return; }
        cycles -= m_timerLeft;
        timerTickLow();
        if (m_timerRunning) m_timerLeft = 8 * ((m_timerControl & 0x1F) + 1);
    }
}

// ---------------------------------------------------------------------------
// Reset and execution
// ---------------------------------------------------------------------------
void Tms7000::reset() {
    m_cycles = 0;
    if (m_idle) { m_pc++; m_idle = false; }
    // While /RESET is low: the ports
    wp(0x06, 0xFF);                                     // port B
    wp(0x09, 0x00);                                     // DDR C
    wp(0x0B, 0x00);                                     // DDR D
    wp(0x08, 0xFF);                                     // port C (NMOS)
    wp(0x0A, 0xFF);                                     // port D
    // /RESET rising
    m_sr = 0;
    wp(0x00, 0x00);                                     // IOCNT0
    m_sp = 0xFF;
    execute(0xFF);                                      // trap 0
    m_cycles += 3;                                      // 17 in all
}

int Tms7000::step() {
    m_cycles = 0;
    checkInterrupts();
    if (m_cycles == 0) execute(read(m_pc++));
    const int n = m_cycles;
    timerAdvance(n);
    return n;
}

void Tms7000::execute(uint8_t op) {
    switch (op) {
        case 0x00: m_cycles += 5; break;                                        // NOP
        case 0x01: m_cycles += 6; m_pc--; m_idle = true; break;                 // IDLE
        case 0x05: m_cycles += 5; m_sr |= SR_N | SR_Z | SR_C | SR_I; checkInterrupts(); break;   // EINT
        case 0x06: m_cycles += 5; m_sr &= (uint8_t)~(SR_N | SR_Z | SR_C | SR_I); break;          // DINT
        case 0x07: m_cycles += 5; m_sr = (uint8_t)((m_sr & ~SR_N) | SR_C | SR_Z); break;         // SETC
        case 0x08: m_cycles += 6; m_sr = pull8() & 0xF0; checkInterrupts(); break;                // POP ST
        case 0x09: m_cycles += 6; wr(1, m_sp); break;                           // STSP
        case 0x0A: m_cycles += 7; m_pc = pull16(); break;                       // RETS
        case 0x0B: m_cycles += 9; m_pc = pull16(); m_sr = pull8() & 0xF0; checkInterrupts(); break;  // RETI
        case 0x0D: m_cycles += 5; m_sp = rr(1); break;                          // LDSP
        case 0x0E: m_cycles += 6; push8(m_sr); break;                           // PUSH ST

        case 0x12: amR2A(&Tms7000::opMov); break;
        case 0x13: amR2A(&Tms7000::opAnd); break;
        case 0x14: amR2A(&Tms7000::opOr); break;
        case 0x15: amR2A(&Tms7000::opXor); break;
        case 0x16: amR2A(&Tms7000::opBtjo); break;
        case 0x17: amR2A(&Tms7000::opBtjz); break;
        case 0x18: amR2A(&Tms7000::opAdd); break;
        case 0x19: amR2A(&Tms7000::opAdc); break;
        case 0x1A: amR2A(&Tms7000::opSub); break;
        case 0x1B: amR2A(&Tms7000::opSbb); break;
        case 0x1C: amR2A(&Tms7000::opMpy); break;
        case 0x1D: amR2A(&Tms7000::opCmp); break;
        case 0x1E: amR2A(&Tms7000::opDac); break;
        case 0x1F: amR2A(&Tms7000::opDsb); break;

        case 0x22: amI2A(&Tms7000::opMov); break;
        case 0x23: amI2A(&Tms7000::opAnd); break;
        case 0x24: amI2A(&Tms7000::opOr); break;
        case 0x25: amI2A(&Tms7000::opXor); break;
        case 0x26: amI2A(&Tms7000::opBtjo); break;
        case 0x27: amI2A(&Tms7000::opBtjz); break;
        case 0x28: amI2A(&Tms7000::opAdd); break;
        case 0x29: amI2A(&Tms7000::opAdc); break;
        case 0x2A: amI2A(&Tms7000::opSub); break;
        case 0x2B: amI2A(&Tms7000::opSbb); break;
        case 0x2C: amI2A(&Tms7000::opMpy); break;
        case 0x2D: amI2A(&Tms7000::opCmp); break;
        case 0x2E: amI2A(&Tms7000::opDac); break;
        case 0x2F: amI2A(&Tms7000::opDsb); break;

        case 0x32: amR2B(&Tms7000::opMov); break;
        case 0x33: amR2B(&Tms7000::opAnd); break;
        case 0x34: amR2B(&Tms7000::opOr); break;
        case 0x35: amR2B(&Tms7000::opXor); break;
        case 0x36: amR2B(&Tms7000::opBtjo); break;
        case 0x37: amR2B(&Tms7000::opBtjz); break;
        case 0x38: amR2B(&Tms7000::opAdd); break;
        case 0x39: amR2B(&Tms7000::opAdc); break;
        case 0x3A: amR2B(&Tms7000::opSub); break;
        case 0x3B: amR2B(&Tms7000::opSbb); break;
        case 0x3C: amR2B(&Tms7000::opMpy); break;
        case 0x3D: amR2B(&Tms7000::opCmp); break;
        case 0x3E: amR2B(&Tms7000::opDac); break;
        case 0x3F: amR2B(&Tms7000::opDsb); break;

        case 0x42: amR2R(&Tms7000::opMov); break;
        case 0x43: amR2R(&Tms7000::opAnd); break;
        case 0x44: amR2R(&Tms7000::opOr); break;
        case 0x45: amR2R(&Tms7000::opXor); break;
        case 0x46: amR2R(&Tms7000::opBtjo); break;
        case 0x47: amR2R(&Tms7000::opBtjz); break;
        case 0x48: amR2R(&Tms7000::opAdd); break;
        case 0x49: amR2R(&Tms7000::opAdc); break;
        case 0x4A: amR2R(&Tms7000::opSub); break;
        case 0x4B: amR2R(&Tms7000::opSbb); break;
        case 0x4C: amR2R(&Tms7000::opMpy); break;
        case 0x4D: amR2R(&Tms7000::opCmp); break;
        case 0x4E: amR2R(&Tms7000::opDac); break;
        case 0x4F: amR2R(&Tms7000::opDsb); break;

        case 0x52: amI2B(&Tms7000::opMov); break;
        case 0x53: amI2B(&Tms7000::opAnd); break;
        case 0x54: amI2B(&Tms7000::opOr); break;
        case 0x55: amI2B(&Tms7000::opXor); break;
        case 0x56: amI2B(&Tms7000::opBtjo); break;
        case 0x57: amI2B(&Tms7000::opBtjz); break;
        case 0x58: amI2B(&Tms7000::opAdd); break;
        case 0x59: amI2B(&Tms7000::opAdc); break;
        case 0x5A: amI2B(&Tms7000::opSub); break;
        case 0x5B: amI2B(&Tms7000::opSbb); break;
        case 0x5C: amI2B(&Tms7000::opMpy); break;
        case 0x5D: amI2B(&Tms7000::opCmp); break;
        case 0x5E: amI2B(&Tms7000::opDac); break;
        case 0x5F: amI2B(&Tms7000::opDsb); break;

        case 0x62: amB2A(&Tms7000::opMov); break;
        case 0x63: amB2A(&Tms7000::opAnd); break;
        case 0x64: amB2A(&Tms7000::opOr); break;
        case 0x65: amB2A(&Tms7000::opXor); break;
        case 0x66: amB2A(&Tms7000::opBtjo); break;
        case 0x67: amB2A(&Tms7000::opBtjz); break;
        case 0x68: amB2A(&Tms7000::opAdd); break;
        case 0x69: amB2A(&Tms7000::opAdc); break;
        case 0x6A: amB2A(&Tms7000::opSub); break;
        case 0x6B: amB2A(&Tms7000::opSbb); break;
        case 0x6C: amB2A(&Tms7000::opMpy); break;
        case 0x6D: amB2A(&Tms7000::opCmp); break;
        case 0x6E: amB2A(&Tms7000::opDac); break;
        case 0x6F: amB2A(&Tms7000::opDsb); break;

        case 0x72: amI2R(&Tms7000::opMov); break;
        case 0x73: amI2R(&Tms7000::opAnd); break;
        case 0x74: amI2R(&Tms7000::opOr); break;
        case 0x75: amI2R(&Tms7000::opXor); break;
        case 0x76: amI2R(&Tms7000::opBtjo); break;
        case 0x77: amI2R(&Tms7000::opBtjz); break;
        case 0x78: amI2R(&Tms7000::opAdd); break;
        case 0x79: amI2R(&Tms7000::opAdc); break;
        case 0x7A: amI2R(&Tms7000::opSub); break;
        case 0x7B: amI2R(&Tms7000::opSbb); break;
        case 0x7C: amI2R(&Tms7000::opMpy); break;
        case 0x7D: amI2R(&Tms7000::opCmp); break;
        case 0x7E: amI2R(&Tms7000::opDac); break;
        case 0x7F: amI2R(&Tms7000::opDsb); break;

        case 0x80: amP2A(&Tms7000::opMov); break;
        case 0x82: amA2P(&Tms7000::opMov); break;
        case 0x83: amA2P(&Tms7000::opAnd); break;
        case 0x84: amA2P(&Tms7000::opOr); break;
        case 0x85: amA2P(&Tms7000::opXor); break;
        case 0x86: amA2P(&Tms7000::opBtjo); break;
        case 0x87: amA2P(&Tms7000::opBtjz); break;
        case 0x88: {                                    // MOVD %>nnnn,Rn
            m_cycles += 15;
            const uint16_t t = imm16();
            wr16(imm8(), t);
            setCNZ(t >> 8);
            break;
        }
        case 0x8A: { m_cycles += 11; const uint8_t t = read(imm16()); wr(0, t); setCNZ(t); break; }    // LDA @nnnn
        case 0x8B: { m_cycles += 11; const uint8_t t = rr(0); write(imm16(), t); setCNZ(t); break; }   // STA @nnnn
        case 0x8C: m_cycles += 10; m_pc = imm16(); break;                                              // BR @nnnn
        case 0x8D: { m_cycles += 12; const unsigned t = (unsigned)(rr(0) - read(imm16())); setNZ(t); setC(~t); break; }  // CMPA @nnnn
        case 0x8E: { m_cycles += 14; const uint16_t t = imm16(); push16(m_pc); m_pc = t; break; }      // CALL @nnnn

        case 0x91: amP2B(&Tms7000::opMov); break;
        case 0x92: amB2P(&Tms7000::opMov); break;
        case 0x93: amB2P(&Tms7000::opAnd); break;
        case 0x94: amB2P(&Tms7000::opOr); break;
        case 0x95: amB2P(&Tms7000::opXor); break;
        case 0x96: amB2P(&Tms7000::opBtjo); break;
        case 0x97: amB2P(&Tms7000::opBtjz); break;
        case 0x98: {                                    // MOVD Rn,Rn
            m_cycles += 14;
            const uint16_t t = rr16(imm8());
            wr16(imm8(), t);
            setCNZ(t >> 8);
            break;
        }
        case 0x9A: { m_cycles += 10; const uint8_t t = read(rr16(imm8())); wr(0, t); setCNZ(t); break; }  // LDA *Rn
        case 0x9B: { m_cycles += 10; const uint8_t t = rr(0); write(rr16(imm8()), t); setCNZ(t); break; } // STA *Rn
        case 0x9C: m_cycles += 9; m_pc = rr16(imm8()); break;                                              // BR *Rn
        case 0x9D: { m_cycles += 11; const unsigned t = (unsigned)(rr(0) - read(rr16(imm8()))); setNZ(t); setC(~t); break; }
        case 0x9E: { m_cycles += 13; const uint16_t t = rr16(imm8()); push16(m_pc); m_pc = t; break; }     // CALL *Rn

        case 0xA2: amI2P(&Tms7000::opMov); break;
        case 0xA3: amI2P(&Tms7000::opAnd); break;
        case 0xA4: amI2P(&Tms7000::opOr); break;
        case 0xA5: amI2P(&Tms7000::opXor); break;
        case 0xA6: amI2P(&Tms7000::opBtjo); break;
        case 0xA7: amI2P(&Tms7000::opBtjz); break;
        case 0xA8: {                                    // MOVD %>nnnn(B),Rn
            m_cycles += 17;
            const uint16_t t = (uint16_t)(imm16() + rr(1));
            wr16(imm8(), t);
            setCNZ(t >> 8);
            break;
        }
        case 0xAA: { m_cycles += 13; const uint8_t t = read((uint16_t)(imm16() + rr(1))); wr(0, t); setCNZ(t); break; }
        case 0xAB: { m_cycles += 13; const uint8_t t = rr(0); write((uint16_t)(imm16() + rr(1)), t); setCNZ(t); break; }
        case 0xAC: m_cycles += 12; m_pc = (uint16_t)(imm16() + rr(1)); break;
        case 0xAD: { m_cycles += 14; const unsigned t = (unsigned)(rr(0) - read((uint16_t)(imm16() + rr(1)))); setNZ(t); setC(~t); break; }
        case 0xAE: { m_cycles += 16; const uint16_t t = (uint16_t)(imm16() + rr(1)); push16(m_pc); m_pc = t; break; }

        case 0xB0: amA2A(&Tms7000::opMov); break;       // CLRC / TSTA
        case 0xB1: amB2A(&Tms7000::opMov); break;       // (undocumented)
        case 0xB2: amA(&Tms7000::opDec); break;
        case 0xB3: amA(&Tms7000::opInc); break;
        case 0xB4: amA(&Tms7000::opInv); break;
        case 0xB5: amA(&Tms7000::opClr); break;
        case 0xB6: amA(&Tms7000::opXchb); break;
        case 0xB7: amA(&Tms7000::opSwap); break;
        case 0xB8: { m_cycles += 6; const uint8_t t = rr(0); push8(t); setCNZ(t); break; }     // PUSH A
        case 0xB9: { m_cycles += 6; const uint8_t t = pull8(); wr(0, t); setCNZ(t); break; }   // POP A
        case 0xBA: amA(&Tms7000::opDjnz); break;
        case 0xBB: { m_cycles += 9; const uint32_t t = (uint32_t)(rr16(0) - 1); wr16(0, (uint16_t)t); setNZ(t >> 8); setC(~(t >> 8)); break; }
        case 0xBC: amA(&Tms7000::opRr); break;
        case 0xBD: amA(&Tms7000::opRrc); break;
        case 0xBE: amA(&Tms7000::opRl); break;
        case 0xBF: amA(&Tms7000::opRlc); break;

        case 0xC0: amA2B(&Tms7000::opMov); break;
        case 0xC1: amB2B(&Tms7000::opMov); break;       // TSTB
        case 0xC2: amB(&Tms7000::opDec); break;
        case 0xC3: amB(&Tms7000::opInc); break;
        case 0xC4: amB(&Tms7000::opInv); break;
        case 0xC5: amB(&Tms7000::opClr); break;
        case 0xC6: amB(&Tms7000::opXchb); break;
        case 0xC7: amB(&Tms7000::opSwap); break;
        case 0xC8: { m_cycles += 6; const uint8_t t = rr(1); push8(t); setCNZ(t); break; }     // PUSH B
        case 0xC9: { m_cycles += 6; const uint8_t t = pull8(); wr(1, t); setCNZ(t); break; }   // POP B
        case 0xCA: amB(&Tms7000::opDjnz); break;
        case 0xCB: { m_cycles += 9; const uint32_t t = (uint32_t)(rr16(1) - 1); wr16(1, (uint16_t)t); setNZ(t >> 8); setC(~(t >> 8)); break; }
        case 0xCC: amB(&Tms7000::opRr); break;
        case 0xCD: amB(&Tms7000::opRrc); break;
        case 0xCE: amB(&Tms7000::opRl); break;
        case 0xCF: amB(&Tms7000::opRlc); break;

        case 0xD0: amA2R(&Tms7000::opMov); break;
        case 0xD1: amB2R(&Tms7000::opMov); break;
        case 0xD2: amR(&Tms7000::opDec); break;
        case 0xD3: amR(&Tms7000::opInc); break;
        case 0xD4: amR(&Tms7000::opInv); break;
        case 0xD5: amR(&Tms7000::opClr); break;
        case 0xD6: amR(&Tms7000::opXchb); break;
        case 0xD7: amR(&Tms7000::opSwap); break;
        case 0xD8: { m_cycles += 8; const uint8_t t = rr(imm8()); push8(t); setCNZ(t); break; }   // PUSH Rn
        case 0xD9: { m_cycles += 8; const uint8_t t = pull8(); wr(imm8(), t); setCNZ(t); break; } // POP Rn
        case 0xDA: amR(&Tms7000::opDjnz); break;
        case 0xDB: {                                    // DECD Rn
            m_cycles += 11;
            const uint8_t r = imm8();
            const uint32_t t = (uint32_t)(rr16(r) - 1);
            wr16(r, (uint16_t)t);
            setNZ(t >> 8);
            setC(~(t >> 8));
            break;
        }
        case 0xDC: amR(&Tms7000::opRr); break;
        case 0xDD: amR(&Tms7000::opRrc); break;
        case 0xDE: amR(&Tms7000::opRl); break;
        case 0xDF: amR(&Tms7000::opRlc); break;

        case 0xE0: jmp(true); break;
        case 0xE1: jmp(m_sr & SR_N); break;             // JN
        case 0xE2: jmp(m_sr & SR_Z); break;             // JZ
        case 0xE3: jmp(m_sr & SR_C); break;             // JC
        case 0xE4: jmp(!(m_sr & (SR_Z | SR_N))); break; // JP
        case 0xE5: jmp(!(m_sr & SR_N)); break;          // JPZ
        case 0xE6: jmp(!(m_sr & SR_Z)); break;          // JNZ
        case 0xE7: jmp(!(m_sr & SR_C)); break;          // JNC

        default:
            if (op >= 0xE8) trap((uint8_t)(op << 1));   // TRAP 23..0
            else m_cycles += 5;                         // illegal (guessed, as MAME)
            break;
    }
}

// ---------------------------------------------------------------------------
// Addressing modes
// ---------------------------------------------------------------------------
#define AM_WB(writeFn, addr, p1, p2) do { const int res = (this->*op)(p1, p2); if (res >= 0) writeFn(addr, (uint8_t)res); } while (0)

void Tms7000::amA(OpFunc op) { m_cycles += 5; AM_WB(wr, 0, rr(0), 0); }
void Tms7000::amB(OpFunc op) { m_cycles += 5; AM_WB(wr, 1, rr(1), 0); }
void Tms7000::amR(OpFunc op) { m_cycles += 7; const uint8_t r = imm8(); AM_WB(wr, r, rr(r), 0); }
void Tms7000::amA2A(OpFunc op) { m_cycles += 6; AM_WB(wr, 0, rr(0), rr(0)); }
void Tms7000::amA2B(OpFunc op) { m_cycles += 6; AM_WB(wr, 1, rr(1), rr(0)); }
void Tms7000::amA2P(OpFunc op) { m_cycles += 10; const uint8_t r = imm8(); AM_WB(wp, r, rp(r), rr(0)); }
void Tms7000::amA2R(OpFunc op) { m_cycles += 8; const uint8_t r = imm8(); AM_WB(wr, r, rr(r), rr(0)); }
void Tms7000::amB2A(OpFunc op) { m_cycles += 5; AM_WB(wr, 0, rr(0), rr(1)); }
void Tms7000::amB2B(OpFunc op) { m_cycles += 6; AM_WB(wr, 1, rr(1), rr(1)); }
void Tms7000::amB2R(OpFunc op) { m_cycles += 7; const uint8_t r = imm8(); AM_WB(wr, r, rr(r), rr(1)); }
void Tms7000::amB2P(OpFunc op) { m_cycles += 9; const uint8_t r = imm8(); AM_WB(wp, r, rp(r), rr(1)); }
void Tms7000::amR2A(OpFunc op) { m_cycles += 8; const uint8_t p2 = rr(imm8()); AM_WB(wr, 0, rr(0), p2); }
void Tms7000::amR2B(OpFunc op) { m_cycles += 8; const uint8_t p2 = rr(imm8()); AM_WB(wr, 1, rr(1), p2); }
void Tms7000::amR2R(OpFunc op) { m_cycles += 10; const uint8_t p2 = rr(imm8()); const uint8_t r = imm8(); AM_WB(wr, r, rr(r), p2); }
void Tms7000::amI2A(OpFunc op) { m_cycles += 7; const uint8_t p2 = imm8(); AM_WB(wr, 0, rr(0), p2); }
void Tms7000::amI2B(OpFunc op) { m_cycles += 7; const uint8_t p2 = imm8(); AM_WB(wr, 1, rr(1), p2); }
void Tms7000::amI2R(OpFunc op) { m_cycles += 9; const uint8_t p2 = imm8(); const uint8_t r = imm8(); AM_WB(wr, r, rr(r), p2); }
void Tms7000::amI2P(OpFunc op) { m_cycles += 11; const uint8_t p2 = imm8(); const uint8_t r = imm8(); AM_WB(wp, r, rp(r), p2); }
void Tms7000::amP2A(OpFunc op) { m_cycles += 9; const uint8_t p2 = rp(imm8()); AM_WB(wr, 0, rr(0), p2); }
void Tms7000::amP2B(OpFunc op) { m_cycles += 8; const uint8_t p2 = rp(imm8()); AM_WB(wr, 1, rr(1), p2); }

#undef AM_WB

// ---------------------------------------------------------------------------
// The ALU
// ---------------------------------------------------------------------------
int Tms7000::opClr(uint8_t, uint8_t) { setCNZ(0); return 0; }
int Tms7000::opDec(uint8_t a, uint8_t) { const unsigned t = (uint16_t)(a - 1); setNZ(t); setC(~t); return t & 0xFF; }
int Tms7000::opInc(uint8_t a, uint8_t) { const unsigned t = (unsigned)a + 1; setCNZ(t); return t & 0xFF; }
int Tms7000::opInv(uint8_t a, uint8_t) { const uint8_t t = (uint8_t)~a; setCNZ(t); return t; }
int Tms7000::opRl(uint8_t a, uint8_t) { const unsigned t = (unsigned)(a << 1 | a >> 7); setCNZ(t); return t & 0xFF; }
int Tms7000::opRlc(uint8_t a, uint8_t) { const unsigned t = (unsigned)(a << 1 | getC()); setCNZ(t); return t & 0xFF; }
int Tms7000::opRr(uint8_t a, uint8_t) { const unsigned t = (uint16_t)(a >> 1 | a << 8 | (a << 7 & 0x80)); setCNZ(t); return t & 0xFF; }
int Tms7000::opRrc(uint8_t a, uint8_t) { const unsigned t = (uint16_t)(a >> 1 | a << 8 | getC() << 7); setCNZ(t); return t & 0xFF; }
int Tms7000::opSwap(uint8_t a, uint8_t) { m_cycles += 3; const unsigned t = (uint16_t)(a >> 4 | a << 4); setCNZ(t); return t & 0xFF; }
int Tms7000::opXchb(uint8_t a, uint8_t) {
    m_cycles += 1;
    const uint8_t t = rr(1);
    setCNZ(t);
    wr(1, a);
    return t;
}
int Tms7000::opAdc(uint8_t a, uint8_t b) { const unsigned t = (unsigned)(a + b + getC()); setCNZ(t); return t & 0xFF; }
int Tms7000::opAdd(uint8_t a, uint8_t b) { const unsigned t = (unsigned)(a + b); setCNZ(t); return t & 0xFF; }
int Tms7000::opAnd(uint8_t a, uint8_t b) { const uint8_t t = a & b; setCNZ(t); return t; }
int Tms7000::opCmp(uint8_t a, uint8_t b) { const unsigned t = (uint16_t)(a - b); setNZ(t); setC(~t); return -1; }
int Tms7000::opMpy(uint8_t a, uint8_t b) {
    m_cycles += 39;
    const unsigned t = (unsigned)(a * b);
    setCNZ(t >> 8);
    wm16(0, (uint16_t)t);                               // always into A:B
    return -1;
}
int Tms7000::opMov(uint8_t, uint8_t b) { setCNZ(b); return b; }
int Tms7000::opOr(uint8_t a, uint8_t b) { const uint8_t t = a | b; setCNZ(t); return t; }
int Tms7000::opSbb(uint8_t a, uint8_t b) { const unsigned t = (uint16_t)(a - b - !getC()); setNZ(t); setC(~t); return t & 0xFF; }
int Tms7000::opSub(uint8_t a, uint8_t b) { const unsigned t = (uint16_t)(a - b); setNZ(t); setC(~t); return t & 0xFF; }
int Tms7000::opXor(uint8_t a, uint8_t b) { const uint8_t t = a ^ b; setCNZ(t); return t; }

namespace {
const uint8_t BCD_OUT[6] = {0x00, 0x06, 0x00, 0x66, 0x60, 0x66};
}

int Tms7000::opDac(uint8_t a, uint8_t b) {
    m_cycles += 2;
    const int c = getC();
    const int h1 = a >> 4 & 15, l1 = a & 15, h2 = b >> 4 & 15, l2 = b & 15;
    uint8_t d = (l1 + l2 + c) < 10 ? 0 : 1;
    if (h1 + h2 == 9) d |= 2;
    else if (h1 + h2 > 9) d |= 4;
    const uint8_t t = (uint8_t)(a + b + c + BCD_OUT[d]);
    setCNZ(t);
    if (d > 2) m_sr |= SR_C;
    return t;
}

int Tms7000::opDsb(uint8_t a, uint8_t b) {
    m_cycles += 2;
    const int c = !getC();
    const int h1 = a >> 4 & 15, l1 = a & 15, h2 = b >> 4 & 15, l2 = b & 15;
    uint8_t d = (l1 - c) >= l2 ? 0 : 1;
    if (h1 == h2) d |= 2;
    else if (h1 < h2) d |= 4;
    const uint8_t t = (uint8_t)(a - b - c - BCD_OUT[d]);
    setCNZ(t);
    if (d <= 2) m_sr |= SR_C;
    return t;
}

// Branches
void Tms7000::shortBranch(bool take) {
    m_cycles += 2;
    const int8_t d = (int8_t)imm8();
    if (take) {
        m_pc = (uint16_t)(m_pc + d);
        m_cycles += 2;
    }
}

void Tms7000::jmp(bool take) {
    m_cycles += 3;
    shortBranch(take);
}

int Tms7000::opDjnz(uint8_t a, uint8_t) {
    const uint8_t t = (uint8_t)(a - 1);
    shortBranch(t != 0);
    return t;
}

int Tms7000::opBtjo(uint8_t a, uint8_t b) {
    const uint8_t t = a & b;
    setCNZ(t);
    shortBranch(t != 0);
    return -1;
}

int Tms7000::opBtjz(uint8_t a, uint8_t b) {
    const uint8_t t = (uint8_t)(~a & b);
    setCNZ(t);
    shortBranch(t != 0);
    return -1;
}

void Tms7000::trap(uint8_t vec) {
    m_cycles += 14;
    push16(m_pc);
    m_pc = rm16((uint16_t)(0xFF00 | vec));
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------
std::vector<uint8_t> Tms7000::saveState() const {
    std::vector<uint8_t> v(m_rf, m_rf + 128);
    auto put = [&v](uint32_t x, int n) { for (int i = 0; i < n; ++i) v.push_back((uint8_t)(x >> (8 * i))); };
    put(m_pc, 2);
    put(m_sp, 1);
    put(m_sr, 1);
    put((m_irq[0] ? 1u : 0u) | (m_irq[1] ? 2u : 0u) | (m_idle ? 4u : 0u) | (m_timerRunning ? 8u : 0u), 1);
    put(m_ioControl, 1);
    put(m_timerData, 1);
    put(m_timerControl, 1);
    put(m_timerCapture, 1);
    put((uint32_t)m_timerDec, 4);
    put((uint32_t)m_timerLeft, 4);
    for (int i = 0; i < 4; ++i) { put(m_portLatch[i], 1); put(m_portDdr[i], 1); }
    return v;
}

void Tms7000::loadState(const std::vector<uint8_t>& s) {
    if (s.size() < 128 + 25) return;
    std::memcpy(m_rf, s.data(), 128);
    size_t p = 128;
    auto get = [&s, &p](int n) { uint32_t x = 0; for (int i = 0; i < n; ++i) x |= (uint32_t)s[p++] << (8 * i); return x; };
    m_pc = (uint16_t)get(2);
    m_sp = (uint8_t)get(1);
    m_sr = (uint8_t)get(1);
    const uint32_t f = get(1);
    m_irq[0] = f & 1; m_irq[1] = f & 2; m_idle = f & 4; m_timerRunning = f & 8;
    m_ioControl = (uint8_t)get(1);
    m_timerData = (uint8_t)get(1);
    m_timerControl = (uint8_t)get(1);
    m_timerCapture = (uint8_t)get(1);
    m_timerDec = (int)get(4);
    m_timerLeft = (int)get(4);
    for (int i = 0; i < 4; ++i) { m_portLatch[i] = (uint8_t)get(1); m_portDdr[i] = (uint8_t)get(1); }
}
