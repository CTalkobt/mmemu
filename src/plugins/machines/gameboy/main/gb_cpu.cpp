// The SM83, after Mesen2 (Core/Gameboy/GbCpu.cpp), GPL-3.0, Sour and the Mesen contributors.

#include "gb_cpu.h"
#include "gameboy.h"

namespace gb {

void Cpu::init() {
    m_s = State{};
    m_s.pc = 0;
    m_s.sp = 0xFFFF;
    m_s.cycles = 6;            // Mesen's, with a boot ROM (a start without one sets it: Gameboy::postBootState)
    m_prevIrq = 0;
}

// ---------------------------------------------------------------------------
// Time and memory
// ---------------------------------------------------------------------------

void Cpu::cycle() { m_gb->exec(); m_gb->exec(); }
void Cpu::masterCycle() { m_gb->execMasterCycle(); }

uint8_t Cpu::readOp() { return m_gb->read<OamCorruption::ReadIncDec>(m_s.pc++); }
uint8_t Cpu::readCode() {
    cycle();
    return m_gb->read<OamCorruption::ReadIncDec>(m_s.pc++);
}
uint16_t Cpu::readCodeWord() {
    const uint8_t lo = readCode();
    const uint8_t hi = readCode();
    return (uint16_t)(hi << 8 | lo);
}
template <OamCorruption C> uint8_t Cpu::read(uint16_t addr) {
    cycle();
    const uint8_t v = m_gb->read<C>(addr);
    if (auto* h = m_gb->accessHook()) h->cpuRead(addr, v);
    return v;
}
void Cpu::write(uint16_t addr, uint8_t v) {
    if (auto* h = m_gb->accessHook()) h->cpuWrite(addr, m_gb->peek(addr), v);
    m_gb->cpuWrite(addr, v);
}

void Cpu::pushByte(uint8_t v) {
    m_ppu->oamCorruption<OamCorruption::Write>(m_s.sp);
    --m_s.sp;
    write(m_s.sp, v);
}
void Cpu::pushWord(uint16_t v) { pushByte((uint8_t)(v >> 8)); pushByte((uint8_t)v); }
uint16_t Cpu::popWord() {
    const uint8_t lo = read<OamCorruption::ReadIncDec>(m_s.sp);
    ++m_s.sp;
    const uint8_t hi = read<OamCorruption::Read>(m_s.sp);
    ++m_s.sp;
    return (uint16_t)(hi << 8 | lo);
}

uint16_t Cpu::rr(int i) const {
    switch (i) { case 0: return bc(); case 1: return de(); case 2: return hl(); default: return m_s.sp; }
}
void Cpu::setRr(int i, uint16_t v) {
    switch (i) { case 0: setBc(v); break; case 1: setDe(v); break; case 2: setHl(v); break; default: m_s.sp = v; break; }
}
uint8_t& Cpu::r8(int i) {
    switch (i) {
    case 0: return m_s.b; case 1: return m_s.c; case 2: return m_s.d; case 3: return m_s.e;
    case 4: return m_s.h; case 5: return m_s.l; default: return m_s.a;
    }
}
bool Cpu::cond(int cc) const {
    switch (cc & 3) {
    case 0: return !flag(FLAG_Z);
    case 1: return flag(FLAG_Z);
    case 2: return !flag(FLAG_C);
    default: return flag(FLAG_C);
    }
}

// ---------------------------------------------------------------------------
// The instruction loop (Mesen's Exec / ProcessNextCycleStart)
// ---------------------------------------------------------------------------

bool Cpu::stoppedStep() {
    if ((m_gb->readJoypad() & 0x0F) != 0x0F) {
        m_s.stopped = false;
        m_s.haltCounter = 0;
        m_ppu->setCpuStopState(false);
        return true;
    }
    return false;
}

void Cpu::exec() {
    if (m_s.stopped) {
        if (stoppedStep()) return;
    } else if (m_prevIrq && !m_s.haltBug) {
        if (m_s.haltCounter > 0) m_s.haltCounter = 0;
        if (m_s.ime) {
            cycle();
            cycle();
            pushByte((uint8_t)(m_s.pc >> 8));
            const uint8_t vector = m_gb->pendingIrq();       // IE/IF read again here (ie_push)
            pushByte((uint8_t)m_s.pc);
            cycle();
            switch (vector) {
            case 0: m_s.pc = 0; break;                       // nothing requested any more: $0000
            case IRQ_VBLANK: m_s.pc = 0x40; break;
            case IRQ_STAT: m_s.pc = 0x48; break;
            case IRQ_TIMER: m_s.pc = 0x50; break;
            case IRQ_SERIAL: m_s.pc = 0x58; break;
            case IRQ_JOYPAD: m_s.pc = 0x60; break;
            }
            if (vector) m_gb->clearIrq(vector);
            m_s.eiPending = false;                           // EI twice just before (Batman, as Mesen)
            m_s.ime = false;
            if (m_splitDispatch) return;                     // the debugger's step: the handler next
        }
    }

    if (m_s.haltCounter) {
        if (m_s.haltBug) haltBugStep();
    } else {
        if (m_s.eiPending) { m_s.eiPending = false; m_s.ime = true; }
        execOp(readOp());
    }
    nextCycleStart();
}

void Cpu::nextCycleStart() {
    if (m_s.haltCounter) {
        // The DMG halted checks the interrupts a clock into the cycle
        masterCycle();
        m_prevIrq = m_gb->pendingIrq();
        masterCycle();
        masterCycle();
        masterCycle();
    } else {
        cycle();
        m_prevIrq = m_gb->pendingIrq();
    }
}

void Cpu::haltBugStep() {
    if (m_s.eiPending) { m_s.eiPending = false; m_s.ime = true; }
    // The HALT bug: the next byte is read twice
    cycle();
    const uint8_t op = readOp();
    --m_s.pc;
    m_s.haltCounter = 0;
    m_s.haltBug = false;
    execOp(op);
}

void Cpu::halt() {
    m_s.haltCounter = 1;
    if (!m_s.ime && m_gb->pendingIrq() != 0) m_s.haltBug = true;
}

void Cpu::stop() {
    readCode();                     // the unused operand
    m_s.stopped = true;
    m_s.haltCounter = 1;
    m_ppu->setCpuStopState(true);
}

void Cpu::invalidOp() {
    m_gb->write(0xFFFF, 0);         // every interrupt off
    m_s.haltCounter = 1;            // and locked up
}

// ---------------------------------------------------------------------------
// The ALU
// ---------------------------------------------------------------------------

void Cpu::add(uint8_t v) {
    const int r = m_s.a + v;
    setFlag(FLAG_H, ((m_s.a ^ v ^ r) & 0x10) != 0);
    m_s.a = (uint8_t)r;
    setFlag(FLAG_C, r > 0xFF);
    setFlag(FLAG_Z, m_s.a == 0);
    setFlag(FLAG_N, false);
}
void Cpu::adc(uint8_t v) {
    const uint8_t carry = (m_s.f & FLAG_C) >> 4;
    const int r = m_s.a + v + carry;
    setFlag(FLAG_H, (m_s.a & 0x0F) + (v & 0x0F) + carry > 0x0F);
    m_s.a = (uint8_t)r;
    setFlag(FLAG_C, r > 0xFF);
    setFlag(FLAG_Z, m_s.a == 0);
    setFlag(FLAG_N, false);
}
void Cpu::sub(uint8_t v) {
    const int r = (int)m_s.a - v;
    setFlag(FLAG_H, (m_s.a & 0x0F) < (v & 0x0F));
    m_s.a = (uint8_t)r;
    setFlag(FLAG_C, r < 0);
    setFlag(FLAG_Z, m_s.a == 0);
    setFlag(FLAG_N, true);
}
void Cpu::sbc(uint8_t v) {
    const uint8_t carry = (m_s.f & FLAG_C) >> 4;
    const int r = (int)m_s.a - v - carry;
    setFlag(FLAG_H, (m_s.a & 0x0F) < (v & 0x0F) + carry);
    m_s.a = (uint8_t)r;
    setFlag(FLAG_C, r < 0);
    setFlag(FLAG_Z, m_s.a == 0);
    setFlag(FLAG_N, true);
}
void Cpu::andA(uint8_t v) {
    m_s.a &= v;
    m_s.f = (uint8_t)((m_s.a == 0 ? FLAG_Z : 0) | FLAG_H | (m_s.f & 0x0F));
}
void Cpu::orA(uint8_t v) {
    m_s.a |= v;
    m_s.f = (uint8_t)((m_s.a == 0 ? FLAG_Z : 0) | (m_s.f & 0x0F));
}
void Cpu::xorA(uint8_t v) {
    m_s.a ^= v;
    m_s.f = (uint8_t)((m_s.a == 0 ? FLAG_Z : 0) | (m_s.f & 0x0F));
}
void Cpu::cp(uint8_t v) {
    const int r = (int)m_s.a - v;
    setFlag(FLAG_C, r < 0);
    setFlag(FLAG_H, ((m_s.a ^ v ^ r) & 0x10) != 0);
    setFlag(FLAG_Z, (uint8_t)r == 0);
    setFlag(FLAG_N, true);
}
void Cpu::inc(uint8_t& v) {
    setFlag(FLAG_H, ((v ^ 1 ^ (v + 1)) & 0x10) != 0);
    ++v;
    setFlag(FLAG_Z, v == 0);
    setFlag(FLAG_N, false);
}
void Cpu::dec(uint8_t& v) {
    setFlag(FLAG_H, (v & 0x0F) == 0);
    --v;
    setFlag(FLAG_Z, v == 0);
    setFlag(FLAG_N, true);
}
void Cpu::daa() {
    if (flag(FLAG_N)) {
        if (flag(FLAG_C)) m_s.a -= 0x60;
        if (flag(FLAG_H)) m_s.a -= 0x06;
    } else {
        if (flag(FLAG_C) || m_s.a > 0x99) { m_s.a += 0x60; setFlag(FLAG_C, true); }
        if (flag(FLAG_H) || (m_s.a & 0x0F) > 0x09) m_s.a += 0x06;
    }
    setFlag(FLAG_Z, m_s.a == 0);
    setFlag(FLAG_H, false);
}
void Cpu::shiftOp(int op, uint8_t& v) {
    bool carry = false;
    switch (op) {
    case 0: carry = v & 0x80; v = (uint8_t)(v << 1 | v >> 7); break;                      // RLC
    case 1: carry = v & 0x01; v = (uint8_t)(v >> 1 | v << 7); break;                      // RRC
    case 2: carry = v & 0x80; v = (uint8_t)(v << 1 | (flag(FLAG_C) ? 1 : 0)); break;      // RL
    case 3: carry = v & 0x01; v = (uint8_t)(v >> 1 | (flag(FLAG_C) ? 0x80 : 0)); break;   // RR
    case 4: carry = v & 0x80; v = (uint8_t)(v << 1); break;                               // SLA
    case 5: carry = v & 0x01; v = (uint8_t)((v & 0x80) | v >> 1); break;                  // SRA
    case 6: carry = false; v = (uint8_t)(v << 4 | v >> 4); break;                         // SWAP
    default: carry = v & 0x01; v = (uint8_t)(v >> 1); break;                              // SRL
    }
    m_s.f = (uint8_t)((v == 0 ? FLAG_Z : 0) | (carry ? FLAG_C : 0));
}
void Cpu::addHl(uint16_t v) {
    const int r = hl() + v;
    setFlag(FLAG_H, ((hl() ^ v ^ r) & 0x1000) != 0);
    setHl((uint16_t)r);
    setFlag(FLAG_C, r > 0xFFFF);
    setFlag(FLAG_N, false);
    cycle();
}
void Cpu::incRr(int i) {
    cycle();
    m_ppu->oamCorruption<OamCorruption::Write>(rr(i));
    setRr(i, (uint16_t)(rr(i) + 1));
}
void Cpu::decRr(int i) {
    if (i == 3) {
        // DEC SP: the corruption before the cycle (as Mesen)
        m_ppu->oamCorruption<OamCorruption::Write>(m_s.sp);
        --m_s.sp;
        cycle();
        return;
    }
    cycle();
    m_ppu->oamCorruption<OamCorruption::Write>(rr(i));
    setRr(i, (uint16_t)(rr(i) - 1));
}
void Cpu::ret() {
    m_s.pc = popWord();
    cycle();
}
void Cpu::call(uint16_t to) {
    cycle();
    pushWord(m_s.pc);
    m_s.pc = to;
}

// ---------------------------------------------------------------------------
// The opcodes
// ---------------------------------------------------------------------------

void Cpu::execOp(uint8_t op) {
    // LD r,r' and the ALU on registers, decoded
    if (op >= 0x40 && op < 0x80 && op != 0x76) {
        const int dst = (op >> 3) & 7, src = op & 7;
        if (src == 6) { const uint8_t v = read(hl()); r8(dst) = v; }
        else if (dst == 6) write(hl(), r8(src));
        else r8(dst) = r8(src);
        return;
    }
    if (op >= 0x80 && op < 0xC0) {
        const int src = op & 7;
        const uint8_t v = src == 6 ? read(hl()) : r8(src);
        switch ((op >> 3) & 7) {
        case 0: add(v); break; case 1: adc(v); break; case 2: sub(v); break; case 3: sbc(v); break;
        case 4: andA(v); break; case 5: xorA(v); break; case 6: orA(v); break; default: cp(v); break;
        }
        return;
    }
    switch (op) {
    case 0x00: break;
    case 0x01: case 0x11: case 0x21: case 0x31: setRr(op >> 4, readCodeWord()); break;
    case 0x02: write(bc(), m_s.a); break;
    case 0x12: write(de(), m_s.a); break;
    case 0x22: write(hl(), m_s.a); setHl((uint16_t)(hl() + 1)); break;
    case 0x32: write(hl(), m_s.a); setHl((uint16_t)(hl() - 1)); break;
    case 0x03: case 0x13: case 0x23: case 0x33: incRr(op >> 4); break;
    case 0x0B: case 0x1B: case 0x2B: case 0x3B: decRr(op >> 4); break;
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x3C: inc(r8(op >> 3)); break;
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x3D: dec(r8(op >> 3)); break;
    case 0x34: { uint8_t v = read(hl()); inc(v); write(hl(), v); break; }
    case 0x35: { uint8_t v = read(hl()); dec(v); write(hl(), v); break; }
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x3E: r8(op >> 3) = readCode(); break;
    case 0x36: { const uint8_t v = readCode(); write(hl(), v); break; }
    case 0x07: shiftOp(0, m_s.a); setFlag(FLAG_Z, false); break;     // RLCA
    case 0x0F: shiftOp(1, m_s.a); setFlag(FLAG_Z, false); break;     // RRCA
    case 0x17: shiftOp(2, m_s.a); setFlag(FLAG_Z, false); break;     // RLA
    case 0x1F: shiftOp(3, m_s.a); setFlag(FLAG_Z, false); break;     // RRA
    case 0x08: {
        const uint16_t a = readCodeWord();
        write(a, (uint8_t)m_s.sp);
        write((uint16_t)(a + 1), (uint8_t)(m_s.sp >> 8));
        break;
    }
    case 0x09: case 0x19: case 0x29: case 0x39: addHl(rr(op >> 4)); break;
    case 0x0A: m_s.a = read(bc()); break;
    case 0x1A: m_s.a = read(de()); break;
    case 0x2A: m_s.a = read<OamCorruption::ReadIncDec>(hl()); setHl((uint16_t)(hl() + 1)); break;
    case 0x3A: m_s.a = read<OamCorruption::ReadIncDec>(hl()); setHl((uint16_t)(hl() - 1)); break;
    case 0x10: stop(); break;
    case 0x18: { const int8_t e = (int8_t)readCode(); m_s.pc = (uint16_t)(m_s.pc + e); cycle(); break; }
    case 0x20: case 0x28: case 0x30: case 0x38: {
        const int8_t e = (int8_t)readCode();
        if (cond((op >> 3) & 3)) { m_s.pc = (uint16_t)(m_s.pc + e); cycle(); }
        break;
    }
    case 0x27: daa(); break;
    case 0x2F: m_s.a ^= 0xFF; setFlag(FLAG_N, true); setFlag(FLAG_H, true); break;
    case 0x37: setFlag(FLAG_C, true); setFlag(FLAG_N, false); setFlag(FLAG_H, false); break;
    case 0x3F: m_s.f ^= FLAG_C; setFlag(FLAG_N, false); setFlag(FLAG_H, false); break;
    case 0x76: halt(); break;
    case 0xC0: case 0xC8: case 0xD0: case 0xD8:
        cycle();
        if (cond((op >> 3) & 3)) ret();
        break;
    case 0xC9: ret(); break;
    case 0xD9: m_s.pc = popWord(); m_s.ime = true; cycle(); break;
    case 0xC1: setBc(popWord()); break;
    case 0xD1: setDe(popWord()); break;
    case 0xE1: setHl(popWord()); break;
    case 0xF1: { const uint16_t v = popWord() & 0xFFF0; m_s.a = (uint8_t)(v >> 8); m_s.f = (uint8_t)v; break; }
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: {
        const uint16_t to = readCodeWord();
        if (cond((op >> 3) & 3)) { m_s.pc = to; cycle(); }
        break;
    }
    case 0xC3: m_s.pc = readCodeWord(); cycle(); break;
    case 0xE9: m_s.pc = hl(); break;
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: {
        const uint16_t to = readCodeWord();
        if (cond((op >> 3) & 3)) call(to);
        break;
    }
    case 0xCD: call(readCodeWord()); break;
    case 0xC5: cycle(); pushWord(bc()); break;
    case 0xD5: cycle(); pushWord(de()); break;
    case 0xE5: cycle(); pushWord(hl()); break;
    case 0xF5: cycle(); pushWord(af()); break;
    case 0xC6: add(readCode()); break;
    case 0xCE: adc(readCode()); break;
    case 0xD6: sub(readCode()); break;
    case 0xDE: sbc(readCode()); break;
    case 0xE6: andA(readCode()); break;
    case 0xEE: xorA(readCode()); break;
    case 0xF6: orA(readCode()); break;
    case 0xFE: cp(readCode()); break;
    case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF: call(op & 0x38); break;
    case 0xCB: execCb(); break;
    case 0xE0: { const uint8_t n = readCode(); write((uint16_t)(0xFF00 | n), m_s.a); break; }
    case 0xE2: write((uint16_t)(0xFF00 | m_s.c), m_s.a); break;
    case 0xF0: { const uint8_t n = readCode(); m_s.a = read((uint16_t)(0xFF00 | n)); break; }
    case 0xF2: m_s.a = read((uint16_t)(0xFF00 | m_s.c)); break;
    case 0xEA: { const uint16_t a = readCodeWord(); write(a, m_s.a); break; }
    case 0xFA: { const uint16_t a = readCodeWord(); m_s.a = read(a); break; }
    case 0xE8: case 0xF8: {
        // ADD SP,e and LD HL,SP+e: the flags from the low byte plus the unsigned operand
        const int8_t e = (int8_t)readCode();
        const int r = (uint8_t)m_s.sp + (uint8_t)e;
        setFlag(FLAG_H, (((uint8_t)m_s.sp ^ r ^ e) & 0x10) != 0);
        setFlag(FLAG_C, r > 0xFF);
        setFlag(FLAG_N, false);
        setFlag(FLAG_Z, false);
        if (op == 0xE8) { m_s.sp = (uint16_t)(m_s.sp + e); cycle(); cycle(); }
        else { setHl((uint16_t)(m_s.sp + e)); cycle(); }
        break;
    }
    case 0xF9: m_s.sp = hl(); cycle(); break;
    case 0xF3: m_s.ime = false; break;
    case 0xFB: m_s.eiPending = true; break;
    default: invalidOp(); break;      // D3 DB DD E3 E4 EB EC ED F4 FC FD
    }
}

void Cpu::execCb() {
    const uint8_t op = readCode();
    const int reg = op & 7, kind = op >> 6, bit = (op >> 3) & 7;
    if (kind == 1) {
        // BIT
        const uint8_t v = reg == 6 ? read(hl()) : r8(reg);
        setFlag(FLAG_Z, !(v & (1 << bit)));
        setFlag(FLAG_N, false);
        setFlag(FLAG_H, true);
        return;
    }
    uint8_t v = reg == 6 ? read(hl()) : r8(reg);
    if (kind == 0) shiftOp(bit, v);
    else if (kind == 2) v = (uint8_t)(v & ~(1 << bit));
    else v = (uint8_t)(v | (1 << bit));
    if (reg == 6) write(hl(), v);
    else r8(reg) = v;
}

template uint8_t Cpu::read<OamCorruption::Read>(uint16_t);
template uint8_t Cpu::read<OamCorruption::ReadIncDec>(uint16_t);

}  // namespace gb
