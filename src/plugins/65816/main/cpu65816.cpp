// WDC 65C816 core, after the W65C816S data sheet (WDC): registers, modes,
// addressing, cycle sequences (data sheet table 5-7), decimal arithmetic.

#include "cpu65816.h"
#include "libdebug/main/execution_observer.h"
#include "libcore/main/intercept.h"
#include "libtoolchain/main/idisasm.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace {
const RegDescriptor REGS[W65C816::R_COUNT] = {
    {"A", RegWidth::R16, REGFLAG_ACCUMULATOR, nullptr, nullptr},
    {"X", RegWidth::R16, 0, nullptr, nullptr},
    {"Y", RegWidth::R16, 0, nullptr, nullptr},
    {"S", RegWidth::R16, REGFLAG_SP, nullptr, nullptr},
    {"D", RegWidth::R16, 0, nullptr, nullptr},
    {"DBR", RegWidth::R8, 0, nullptr, nullptr},
    {"PBR", RegWidth::R8, 0, nullptr, nullptr},
    {"PC", RegWidth::R16, REGFLAG_PC, nullptr, nullptr},
    {"P", RegWidth::R8, REGFLAG_STATUS, nullptr, "NVMXDIZC"},
    {"E", RegWidth::R8, REGFLAG_INTERNAL, nullptr, nullptr},
};

// An operand's location: the direct page (with emulation mode's wrap), a
// 24-bit address, or bank 0 (stack relative)
struct Loc {
    enum Kind { DIRECT, LINEAR, BANK0 } kind;
    uint32_t addr;
};
}  // namespace

W65C816::W65C816() { reset(); }

void W65C816::reset() {
    m_e = true;
    m_p = (uint8_t)((m_p & ~FD) | FM | FX | FI);
    m_x &= 0xFF; m_y &= 0xFF;
    m_s = (uint16_t)(0x100 | (m_s & 0xFF));
    m_d = 0;
    m_dbr = m_pbr = 0;
    m_wait = m_stop = false;
    m_nmiPending = false;
    m_vpb = true;
    m_pc = (uint16_t)(read(0xFFFC) | read(0xFFFD) << 8);
    m_vpb = false;
}

const RegDescriptor* W65C816::regDescriptor(int idx) const { return idx >= 0 && idx < R_COUNT ? &REGS[idx] : nullptr; }

uint32_t W65C816::regRead(int idx) const {
    switch (idx) {
        case R_A: return m_a; case R_X: return m_x; case R_Y: return m_y; case R_S: return m_s;
        case R_D: return m_d; case R_DBR: return m_dbr; case R_PBR: return m_pbr; case R_PC: return m_pc;
        case R_P: return m_p; case R_E: return m_e;
    }
    return 0;
}

void W65C816::regWrite(int idx, uint32_t v) {
    switch (idx) {
        case R_A: m_a = (uint16_t)v; break;
        case R_X: m_x = (m_p & FX) ? (uint16_t)(v & 0xFF) : (uint16_t)v; break;
        case R_Y: m_y = (m_p & FX) ? (uint16_t)(v & 0xFF) : (uint16_t)v; break;
        case R_S: m_s = m_e ? (uint16_t)(0x100 | (v & 0xFF)) : (uint16_t)v; break;   // page 1 in emulation mode
        case R_D: m_d = (uint16_t)v; break;
        case R_DBR: m_dbr = (uint8_t)v; break;
        case R_PBR: m_pbr = (uint8_t)v; break;
        case R_PC: m_pc = (uint16_t)v; m_wait = m_stop = false; break;
        case R_P: setP((uint8_t)v); break;                       // M, X set in emulation mode
        case R_E:
            m_e = v & 1;
            if (m_e) { m_p |= FM | FX; m_x &= 0xFF; m_y &= 0xFF; m_s = (uint16_t)(0x100 | (m_s & 0xFF)); }
            break;
    }
}

void W65C816::saveState(uint8_t* b) const {
    const uint16_t w[] = {m_a, m_x, m_y, m_s, m_d, m_pc};
    std::memcpy(b, w, sizeof w);
    b[12] = m_dbr; b[13] = m_pbr; b[14] = m_p;
    b[15] = (uint8_t)(m_e | m_irqLine << 1 | m_nmiPending << 2 | m_wait << 3 | m_stop << 4);
    b[16] = b[17] = b[18] = b[19] = 0;
}

void W65C816::loadState(const uint8_t* b) {
    uint16_t w[6];
    std::memcpy(w, b, sizeof w);
    m_a = w[0]; m_x = w[1]; m_y = w[2]; m_s = w[3]; m_d = w[4]; m_pc = w[5];
    m_dbr = b[12]; m_pbr = b[13]; m_p = b[14];
    m_e = b[15] & 1; m_irqLine = b[15] & 2; m_nmiPending = b[15] & 4; m_wait = b[15] & 8; m_stop = b[15] & 16;
}

// P: in emulation mode M and X stay set; an 8-bit index clears X and Y's high bytes
void W65C816::setP(uint8_t v) {
    if (m_e) v |= FM | FX;
    m_p = v;
    if (m_p & FX) { m_x &= 0xFF; m_y &= 0xFF; }
}

// IRQ, NMI (two internal cycles first), BRK, COP: the bank (native mode),
// PC and P on the stack, then the vector; I set, D clear, bank 0
void W65C816::interrupt(uint16_t nativeVec, uint16_t emuVec, bool hardware) {
    if (!m_e) push(m_pbr);
    push(m_pc >> 8);
    push(m_pc & 0xFF);
    push(m_e ? (uint8_t)(hardware ? (m_p & ~FX) : (m_p | FX)) : m_p);   // emulation: B set for BRK/COP
    m_p = (uint8_t)((m_p | FI) & ~FD);
    m_pbr = 0;
    const uint16_t v = m_e ? emuVec : nativeVec;
    m_vpb = true;                                         // /VP low for the vector's two reads
    const uint8_t lo = read(v);
    m_pc = (uint16_t)(lo | read((uint16_t)(v + 1)) << 8);
    m_vpb = false;
}

int W65C816::step() {
    const uint64_t start = m_cycles;
    if (m_pcHook && m_pcHook->armed(pc()) && m_pcHook->fire(this, pc())) { m_cycles += 2; return 2; }
    if (m_stop) { m_cycles += 1; return 1; }
    const bool holdIrq = m_holdIrq, holdNmi = m_holdNmi;
    m_holdIrq = m_holdNmi = false;
    const bool nmi = m_nmiPending && !holdNmi;
    const bool irqLine = m_irqLine && !holdIrq;
    const bool irq = irqLine && !(m_p & FI);
    if (m_wait) {
        if (!nmi && !irqLine) { m_cycles += 1; return 1; }
        m_wait = false;                                   // an IRQ wakes WAI even when masked
    }
    if (nmi || irq) {
        idleAt(pc()); idleAt(pc());
        if (nmi) { m_nmiPending = false; interrupt(0xFFEA, 0xFFFA, true); }
        else interrupt(0xFFEE, 0xFFFE, true);
        return (int)(m_cycles - start);
    }
    if (m_observer) {
        if (m_observer->needsDisasm()) {
            DisasmEntry entry;
            disassembleEntry(m_bus, pc(), &entry);
            if (!m_observer->onStep(this, m_bus, entry)) return 1;
        } else {
            m_observer->onStepLite(this, pc());
        }
    }
    execute(fetch());
    return (int)(m_cycles - start);
}

int W65C816::execute(uint8_t op) {
    // ---- addressing (the data sheet's cycles; `write`: stores and
    // read-modify-writes always take the index cycle)
    auto ea = [&](int mode, bool write) -> Loc {
        switch (mode) {
            case 0: {                                             // dp
                const uint8_t o = fetch(); idle2();
                return {Loc::DIRECT, o};
            }
            case 1: case 2: {                                     // dp,X  dp,Y
                const uint8_t o = fetch(); idle2(); idle();
                return {Loc::DIRECT, (uint16_t)(o + (mode == 1 ? m_x : m_y))};
            }
            case 3: {                                             // (dp)
                const uint8_t o = fetch(); idle2();
                const uint8_t l = readDirect(o), h = readDirect((uint16_t)(o + 1));
                return {Loc::LINEAR, (uint32_t)m_dbr << 16 | (uint32_t)(h << 8 | l)};
            }
            case 4: {                                             // (dp,X)
                const uint8_t o = fetch(); idle2(); idle();
                const uint16_t p = (uint16_t)(o + m_x);
                const uint8_t l = readDirect(p), h = readDirect((uint16_t)(p + 1));
                return {Loc::LINEAR, (uint32_t)m_dbr << 16 | (uint32_t)(h << 8 | l)};
            }
            case 5: {                                             // (dp),Y
                const uint8_t o = fetch(); idle2();
                const uint8_t l = readDirect(o), h = readDirect((uint16_t)(o + 1));
                const uint16_t p = (uint16_t)(h << 8 | l);
                if (write) idle(); else idle4(p, (uint16_t)(p + m_y));
                return {Loc::LINEAR, (((uint32_t)m_dbr << 16 | p) + m_y) & 0xFFFFFF};
            }
            case 6: case 7: {                                     // [dp]  [dp],Y
                const uint8_t o = fetch(); idle2();
                const uint8_t l = readDirectN(o), h = readDirectN((uint16_t)(o + 1)), b = readDirectN((uint16_t)(o + 2));
                const uint32_t a = (uint32_t)b << 16 | (uint32_t)(h << 8 | l);
                return {Loc::LINEAR, mode == 7 ? (a + m_y) & 0xFFFFFF : a};
            }
            case 8: {                                             // abs
                const uint8_t l = fetch(), h = fetch();
                return {Loc::LINEAR, (uint32_t)m_dbr << 16 | (uint32_t)(h << 8 | l)};
            }
            case 9: case 10: {                                    // abs,X  abs,Y
                const uint8_t l = fetch(), h = fetch();
                const uint16_t a = (uint16_t)(h << 8 | l), i = mode == 9 ? m_x : m_y;
                if (write) idle(); else idle4(a, (uint16_t)(a + i));
                return {Loc::LINEAR, (((uint32_t)m_dbr << 16 | a) + i) & 0xFFFFFF};
            }
            case 11: case 12: {                                   // long  long,X
                const uint8_t l = fetch(), h = fetch(), b = fetch();
                const uint32_t a = (uint32_t)b << 16 | (uint32_t)(h << 8 | l);
                return {Loc::LINEAR, mode == 12 ? (a + m_x) & 0xFFFFFF : a};
            }
            case 13: {                                            // sr,S
                const uint8_t o = fetch(); idle();
                return {Loc::BANK0, (uint16_t)(m_s + o)};
            }
            default: {                                            // (sr,S),Y
                const uint8_t o = fetch(); idle();
                const uint8_t l = read((uint16_t)(m_s + o)), h = read((uint16_t)(m_s + o + 1));
                idle();
                return {Loc::LINEAR, (((uint32_t)m_dbr << 16 | (uint32_t)(h << 8 | l)) + m_y) & 0xFFFFFF};
            }
        }
    };
    auto rd = [&](const Loc& l, int i) -> uint8_t {
        if (l.kind == Loc::DIRECT) return readDirect((uint16_t)(l.addr + i));
        if (l.kind == Loc::BANK0) return read((uint16_t)(l.addr + i));
        return read((l.addr + (uint32_t)i) & 0xFFFFFF);
    };
    auto wr = [&](const Loc& l, int i, uint8_t v) {
        if (l.kind == Loc::DIRECT) writeDirect((uint16_t)(l.addr + i), v);
        else if (l.kind == Loc::BANK0) write((uint16_t)(l.addr + i), v);
        else write((l.addr + (uint32_t)i) & 0xFFFFFF, v);
    };
    auto load = [&](const Loc& l, bool wide) -> uint16_t {
        const uint8_t lo = rd(l, 0);
        return wide ? (uint16_t)(lo | rd(l, 1) << 8) : lo;
    };
    auto store = [&](const Loc& l, bool wide, uint16_t v) {
        wr(l, 0, v & 0xFF);
        if (wide) wr(l, 1, v >> 8);
    };
    auto imm = [&](bool wide) -> uint16_t { const uint8_t lo = fetch(); return wide ? (uint16_t)(lo | fetch() << 8) : lo; };

    // ---- the operations
    auto nzw = [&](uint16_t v, bool wide) { if (wide) nz16(v); else nz8((uint8_t)v); };
    auto adc = [&](uint16_t data) {
        const int c = m_p & FC ? 1 : 0;
        int r;
        if (m8()) {
            const int a = m_a & 0xFF;
            if (!(m_p & FD)) r = a + data + c;
            else {
                r = (a & 0x0F) + (data & 0x0F) + c;
                if (r > 0x09) r += 0x06;
                const int cc = r > 0x0F;
                r = (a & 0xF0) + (data & 0xF0) + (cc << 4) + (r & 0x0F);
            }
            flag(FV, ~(a ^ data) & (a ^ r) & 0x80);
            if ((m_p & FD) && r > 0x9F) r += 0x60;
            flag(FC, r > 0xFF);
            m_a = (uint16_t)((m_a & 0xFF00) | (r & 0xFF));
            nz8((uint8_t)r);
        } else {
            const int a = m_a;
            if (!(m_p & FD)) r = a + data + c;
            else {
                r = (a & 0x000F) + (data & 0x000F) + c;
                if (r > 0x0009) r += 0x0006;
                int cc = r > 0x000F;
                r = (a & 0x00F0) + (data & 0x00F0) + (cc << 4) + (r & 0x000F);
                if (r > 0x009F) r += 0x0060;
                cc = r > 0x00FF;
                r = (a & 0x0F00) + (data & 0x0F00) + (cc << 8) + (r & 0x00FF);
                if (r > 0x09FF) r += 0x0600;
                cc = r > 0x0FFF;
                r = (a & 0xF000) + (data & 0xF000) + (cc << 12) + (r & 0x0FFF);
            }
            flag(FV, ~(a ^ data) & (a ^ r) & 0x8000);
            if ((m_p & FD) && r > 0x9FFF) r += 0x6000;
            flag(FC, r > 0xFFFF);
            m_a = (uint16_t)r;
            nz16(m_a);
        }
    };
    auto sbc = [&](uint16_t data) {
        const int c = m_p & FC ? 1 : 0;
        int r;
        if (m8()) {
            const int a = m_a & 0xFF;
            data = (uint16_t)(~data & 0xFF);
            if (!(m_p & FD)) r = a + data + c;
            else {
                r = (a & 0x0F) + (data & 0x0F) + c;
                if (r <= 0x0F) r -= 0x06;
                const int cc = r > 0x0F;
                r = (a & 0xF0) + (data & 0xF0) + (cc << 4) + (r & 0x0F);
            }
            flag(FV, ~(a ^ data) & (a ^ r) & 0x80);
            if ((m_p & FD) && r <= 0xFF) r -= 0x60;
            flag(FC, r > 0xFF);
            m_a = (uint16_t)((m_a & 0xFF00) | (r & 0xFF));
            nz8((uint8_t)r);
        } else {
            const int a = m_a;
            data = (uint16_t)~data;
            if (!(m_p & FD)) r = a + data + c;
            else {
                r = (a & 0x000F) + (data & 0x000F) + c;
                if (r <= 0x000F) r -= 0x0006;
                int cc = r > 0x000F;
                r = (a & 0x00F0) + (data & 0x00F0) + (cc << 4) + (r & 0x000F);
                if (r <= 0x00FF) r -= 0x0060;
                cc = r > 0x00FF;
                r = (a & 0x0F00) + (data & 0x0F00) + (cc << 8) + (r & 0x00FF);
                if (r <= 0x0FFF) r -= 0x0600;
                cc = r > 0x0FFF;
                r = (a & 0xF000) + (data & 0xF000) + (cc << 12) + (r & 0x0FFF);
            }
            flag(FV, ~(a ^ data) & (a ^ r) & 0x8000);
            if ((m_p & FD) && r <= 0xFFFF) r -= 0x6000;
            flag(FC, r > 0xFFFF);
            m_a = (uint16_t)r;
            nz16(m_a);
        }
    };
    auto cmp = [&](uint16_t reg, uint16_t data, bool wide) {
        const int r = (int)reg - (int)data;
        flag(FC, r >= 0);
        nzw((uint16_t)r, wide);
    };
    // The accumulator group: ORA AND EOR ADC (STA) LDA CMP SBC
    auto aluA = [&](int g, uint16_t data) {
        const bool w = !m8();
        switch (g) {
            case 0: m_a = w ? (uint16_t)(m_a | data) : (uint16_t)(m_a | (data & 0xFF)); nzw(m_a, w); break;
            case 1: m_a = w ? (uint16_t)(m_a & data) : (uint16_t)(m_a & (0xFF00 | data)); nzw(m_a, w); break;
            case 2: m_a = w ? (uint16_t)(m_a ^ data) : (uint16_t)(m_a ^ (data & 0xFF)); nzw(m_a, w); break;
            case 3: adc(data); break;
            case 5: m_a = w ? data : (uint16_t)((m_a & 0xFF00) | (data & 0xFF)); nzw(m_a, w); break;
            case 6: cmp(w ? m_a : (uint16_t)(m_a & 0xFF), data, w); break;
            case 7: sbc(data); break;
        }
    };
    // Read-modify-write: ASL ROL LSR ROR DEC INC TSB TRB
    auto rmwOp = [&](int k, uint16_t v, bool w) -> uint16_t {
        const uint16_t top = w ? 0x8000 : 0x80, mask = w ? 0xFFFF : 0xFF;
        uint16_t r = v;
        switch (k) {
            case 0: flag(FC, v & top); r = (uint16_t)((v << 1) & mask); nzw(r, w); break;
            case 1: { const bool c = m_p & FC; flag(FC, v & top); r = (uint16_t)(((v << 1) | (c ? 1 : 0)) & mask); nzw(r, w); break; }
            case 2: flag(FC, v & 1); r = (uint16_t)(v >> 1); nzw(r, w); break;
            case 3: { const bool c = m_p & FC; flag(FC, v & 1); r = (uint16_t)((v >> 1) | (c ? top : 0)); nzw(r, w); break; }
            case 4: r = (uint16_t)((v - 1) & mask); nzw(r, w); break;
            case 5: r = (uint16_t)((v + 1) & mask); nzw(r, w); break;
            case 6: flag(FZ, !(v & m_a & mask)); r = (uint16_t)(v | (m_a & mask)); break;   // TSB
            case 7: flag(FZ, !(v & m_a & mask)); r = (uint16_t)(v & ~m_a & mask); break;   // TRB
        }
        return r;
    };
    auto rmwMem = [&](int mode, int k) {
        const Loc l = ea(mode, true);
        const bool w = !m8();
        const uint16_t v = load(l, w);
        if (m_e) wr(l, 0, (uint8_t)v);                            // emulation mode: the 6502's dummy write
        else idle();
        const uint16_t r = rmwOp(k, v, w);
        if (w) { wr(l, 1, r >> 8); wr(l, 0, r & 0xFF); }        // 16 bits: the high byte first
        else wr(l, 0, (uint8_t)r);
    };
    auto branch = [&](bool take) {
        const int8_t o = (int8_t)fetch();
        if (!take) return;
        const uint16_t to = (uint16_t)(m_pc + o);
        idle6(to);
        idle();
        m_pc = to;
    };
    auto pushW = [&](uint16_t v, bool w) { if (w) push(v >> 8); push(v & 0xFF); };
    auto pullW = [&](bool w) -> uint16_t { const uint8_t lo = pull(); return w ? (uint16_t)(lo | pull() << 8) : lo; };
    auto setX = [&](uint16_t& r, uint16_t v) { r = x8() ? (uint16_t)(v & 0xFF) : v; nzw(r, !x8()); };

    // ---- the accumulator group by addressing slot (op & 0x1F)
    static const int8_t GROUP_MODE[32] = {
        -1, 4, -1, 13, -1, 0, -1, 6, -1, -2, -1, -1, -1, 8, -1, 11,
        -1, 5, 3, 14, -1, 1, -1, 7, -1, 10, -1, -1, -1, 9, -1, 12};
    const int slot = op & 0x1F, grp = op >> 5;
    if ((op & 1) && GROUP_MODE[slot] != -1 && !(op == 0x89)) {
        const int mode = GROUP_MODE[slot];
        if (grp == 4) {                                           // STA
            if (mode == -2) { imm(!m8()); return 0; }             // (unused: $89 is BIT #)
            const Loc l = ea(mode, true);
            store(l, !m8(), m_a);
            return 0;
        }
        if (mode == -2) { aluA(grp, imm(!m8())); return 0; }      // immediate
        const Loc l = ea(mode, false);
        aluA(grp, load(l, !m8()));
        return 0;
    }
    if ((op & 1) == 0 && slot == 0x12) {                          // (dp) group: ORA..SBC (dp)
        const Loc l = ea(3, grp == 4);
        if (grp == 4) store(l, !m8(), m_a); else aluA(grp, load(l, !m8()));
        return 0;
    }

    switch (op) {
    // ---- read-modify-write
    case 0x06: rmwMem(0, 0); break;  case 0x0E: rmwMem(8, 0); break;
    case 0x16: rmwMem(1, 0); break;  case 0x1E: rmwMem(9, 0); break;
    case 0x26: rmwMem(0, 1); break;  case 0x2E: rmwMem(8, 1); break;
    case 0x36: rmwMem(1, 1); break;  case 0x3E: rmwMem(9, 1); break;
    case 0x46: rmwMem(0, 2); break;  case 0x4E: rmwMem(8, 2); break;
    case 0x56: rmwMem(1, 2); break;  case 0x5E: rmwMem(9, 2); break;
    case 0x66: rmwMem(0, 3); break;  case 0x6E: rmwMem(8, 3); break;
    case 0x76: rmwMem(1, 3); break;  case 0x7E: rmwMem(9, 3); break;
    case 0xC6: rmwMem(0, 4); break;  case 0xCE: rmwMem(8, 4); break;
    case 0xD6: rmwMem(1, 4); break;  case 0xDE: rmwMem(9, 4); break;
    case 0xE6: rmwMem(0, 5); break;  case 0xEE: rmwMem(8, 5); break;
    case 0xF6: rmwMem(1, 5); break;  case 0xFE: rmwMem(9, 5); break;
    case 0x04: rmwMem(0, 6); break;  case 0x0C: rmwMem(8, 6); break;
    case 0x14: rmwMem(0, 7); break;  case 0x1C: rmwMem(8, 7); break;
    case 0x0A: case 0x2A: case 0x4A: case 0x6A: case 0x3A: case 0x1A: {   // accumulator forms
        idle();
        const int k = op == 0x0A ? 0 : op == 0x2A ? 1 : op == 0x4A ? 2 : op == 0x6A ? 3 : op == 0x3A ? 4 : 5;
        const bool w = !m8();
        const uint16_t r = rmwOp(k, w ? m_a : (uint16_t)(m_a & 0xFF), w);
        m_a = w ? r : (uint16_t)((m_a & 0xFF00) | (r & 0xFF));
        break;
    }
    // ---- BIT
    case 0x89: {                                                  // BIT #: Z only
        const uint16_t v = imm(!m8());
        flag(FZ, !(v & m_a & (m8() ? 0xFF : 0xFFFF)));
        break;
    }
    case 0x24: case 0x2C: case 0x34: case 0x3C: {
        const Loc l = ea(op == 0x24 ? 0 : op == 0x2C ? 8 : op == 0x34 ? 1 : 9, false);
        const bool w = !m8();
        const uint16_t v = load(l, w);
        flag(FZ, !(v & m_a & (w ? 0xFFFF : 0xFF)));
        flag(FN, v & (w ? 0x8000 : 0x80));
        flag(FV, v & (w ? 0x4000 : 0x40));
        break;
    }
    // ---- index loads, stores, compares
    case 0xA0: setX(m_y, imm(!x8())); break;
    case 0xA2: setX(m_x, imm(!x8())); break;
    case 0xA4: case 0xAC: case 0xB4: case 0xBC: {
        const Loc l = ea(op == 0xA4 ? 0 : op == 0xAC ? 8 : op == 0xB4 ? 1 : 9, false);
        setX(m_y, load(l, !x8()));
        break;
    }
    case 0xA6: case 0xAE: case 0xB6: case 0xBE: {
        const Loc l = ea(op == 0xA6 ? 0 : op == 0xAE ? 8 : op == 0xB6 ? 2 : 10, false);
        setX(m_x, load(l, !x8()));
        break;
    }
    case 0x84: store(ea(0, true), !x8(), m_y); break;
    case 0x8C: store(ea(8, true), !x8(), m_y); break;
    case 0x94: store(ea(1, true), !x8(), m_y); break;
    case 0x86: store(ea(0, true), !x8(), m_x); break;
    case 0x8E: store(ea(8, true), !x8(), m_x); break;
    case 0x96: store(ea(2, true), !x8(), m_x); break;
    case 0x64: store(ea(0, true), !m8(), 0); break;               // STZ
    case 0x74: store(ea(1, true), !m8(), 0); break;
    case 0x9C: store(ea(8, true), !m8(), 0); break;
    case 0x9E: store(ea(9, true), !m8(), 0); break;
    case 0xC0: cmp(m_y, imm(!x8()), !x8()); break;
    case 0xE0: cmp(m_x, imm(!x8()), !x8()); break;
    case 0xC4: case 0xCC: { const Loc l = ea(op == 0xC4 ? 0 : 8, false); cmp(m_y, load(l, !x8()), !x8()); break; }
    case 0xE4: case 0xEC: { const Loc l = ea(op == 0xE4 ? 0 : 8, false); cmp(m_x, load(l, !x8()), !x8()); break; }
    // ---- branches and jumps
    case 0x10: branch(!(m_p & FN)); break;  case 0x30: branch(m_p & FN); break;
    case 0x50: branch(!(m_p & FV)); break;  case 0x70: branch(m_p & FV); break;
    case 0x90: branch(!(m_p & FC)); break;  case 0xB0: branch(m_p & FC); break;
    case 0xD0: branch(!(m_p & FZ)); break;  case 0xF0: branch(m_p & FZ); break;
    case 0x80: branch(true); break;
    case 0x82: {                                                  // BRL
        const uint8_t l = fetch(), h = fetch();
        idle();
        m_pc = (uint16_t)(m_pc + (h << 8 | l));
        break;
    }
    case 0x4C: { const uint8_t l = fetch(), h = fetch(); m_pc = (uint16_t)(h << 8 | l); break; }
    case 0x5C: {                                                  // JML long
        const uint8_t l = fetch(), h = fetch(), b = fetch();
        m_pc = (uint16_t)(h << 8 | l); m_pbr = b;
        break;
    }
    case 0x6C: {                                                  // JMP (abs): bank 0
        const uint8_t l = fetch(), h = fetch();
        const uint16_t a = (uint16_t)(h << 8 | l);
        const uint8_t pl = read(a), ph = read((uint16_t)(a + 1));
        m_pc = (uint16_t)(ph << 8 | pl);
        break;
    }
    case 0xDC: {                                                  // JML [abs]
        const uint8_t l = fetch(), h = fetch();
        const uint16_t a = (uint16_t)(h << 8 | l);
        const uint8_t pl = read(a), ph = read((uint16_t)(a + 1)), pb = read((uint16_t)(a + 2));
        m_pc = (uint16_t)(ph << 8 | pl); m_pbr = pb;
        break;
    }
    case 0x7C: {                                                  // JMP (abs,X): the program bank
        const uint8_t l = fetch(), h = fetch();
        idle();
        const uint16_t a = (uint16_t)((h << 8 | l) + m_x);
        const uint8_t pl = read((uint32_t)m_pbr << 16 | a), ph = read((uint32_t)m_pbr << 16 | (uint16_t)(a + 1));
        m_pc = (uint16_t)(ph << 8 | pl);
        break;
    }
    case 0x20: {                                                  // JSR abs
        const uint8_t l = fetch(), h = fetch();
        idle();
        const uint16_t r = (uint16_t)(m_pc - 1);
        push(r >> 8); push(r & 0xFF);
        m_pc = (uint16_t)(h << 8 | l);
        break;
    }
    case 0xFC: {                                                  // JSR (abs,X)
        const uint8_t l = fetch();
        pushN(m_pc >> 8); pushN(m_pc & 0xFF);
        const uint8_t h = fetch();
        idle();
        const uint16_t a = (uint16_t)((h << 8 | l) + m_x);
        const uint8_t pl = read((uint32_t)m_pbr << 16 | a), ph = read((uint32_t)m_pbr << 16 | (uint16_t)(a + 1));
        m_pc = (uint16_t)(ph << 8 | pl);
        fixStack();
        break;
    }
    case 0x22: {                                                  // JSL
        const uint8_t l = fetch(), h = fetch();
        pushN(m_pbr);
        idle();
        const uint8_t b = fetch();
        const uint16_t r = (uint16_t)(m_pc - 1);
        pushN(r >> 8); pushN(r & 0xFF);
        m_pc = (uint16_t)(h << 8 | l); m_pbr = b;
        fixStack();
        break;
    }
    case 0x60: {                                                  // RTS
        idle(); idle();
        const uint8_t l = pull(), h = pull();
        idle();
        m_pc = (uint16_t)((h << 8 | l) + 1);
        break;
    }
    case 0x6B: {                                                  // RTL
        idle(); idle();
        const uint8_t l = pullN(), h = pullN(), b = pullN();
        m_pc = (uint16_t)((h << 8 | l) + 1); m_pbr = b;
        fixStack();
        break;
    }
    case 0x40: {                                                  // RTI
        idle(); idle();
        setP(pull());
        if (m_e) m_p |= FM | FX;
        const uint8_t l = pull(), h = pull();
        m_pc = (uint16_t)(h << 8 | l);
        if (!m_e) m_pbr = pull();
        break;
    }
    case 0x00: case 0x02: {                                       // BRK, COP
        fetch();                                                  // the signature byte
        if (op == 0x00) interrupt(0xFFE6, 0xFFFE, false);
        else interrupt(0xFFE4, 0xFFF4, false);
        break;
    }
    // ---- the stack
    case 0x08: idle(); push(m_p); break;                                          // PHP
    case 0x28: idle(); idle(); setP(pull()); break;                               // PLP
    case 0x48: idle(); pushW(m_a, !m8()); break;                                  // PHA
    case 0x68: { idle(); idle(); const bool w = !m8(); const uint16_t v = pullW(w);
                 m_a = w ? v : (uint16_t)((m_a & 0xFF00) | v); nzw(v, w); break; }  // PLA
    case 0xDA: idle(); pushW(m_x, !x8()); break;                                  // PHX
    case 0x5A: idle(); pushW(m_y, !x8()); break;                                  // PHY
    case 0xFA: idle(); idle(); setX(m_x, pullW(!x8())); break;                    // PLX
    case 0x7A: idle(); idle(); setX(m_y, pullW(!x8())); break;                    // PLY
    case 0x8B: idle(); push(m_dbr); break;                                        // PHB
    case 0x4B: idle(); push(m_pbr); break;                                        // PHK
    case 0xAB: idle(); idle(); m_dbr = pullN(); fixStack(); nz8(m_dbr); break;    // PLB
    case 0x0B: idle(); pushN(m_d >> 8); pushN(m_d & 0xFF); fixStack(); break;     // PHD
    case 0x2B: { idle(); idle(); const uint8_t l = pullN(), h = pullN(); m_d = (uint16_t)(h << 8 | l);
                 fixStack(); nz16(m_d); break; }                                  // PLD
    case 0xF4: { const uint8_t l = fetch(), h = fetch(); pushN(h); pushN(l); fixStack(); break; }   // PEA
    case 0xD4: {                                                  // PEI
        const uint8_t o = fetch(); idle2();
        const uint8_t l = readDirectN(o), h = readDirectN((uint16_t)(o + 1));   // no page wrap
        pushN(h); pushN(l); fixStack();
        break;
    }
    case 0x62: {                                                  // PER
        const uint8_t l = fetch(), h = fetch();
        idle();
        const uint16_t v = (uint16_t)(m_pc + (h << 8 | l));
        pushN(v >> 8); pushN(v & 0xFF); fixStack();
        break;
    }
    // ---- transfers and the rest
    case 0xAA: idle(); setX(m_x, m_a); break;                                     // TAX
    case 0xA8: idle(); setX(m_y, m_a); break;                                     // TAY
    case 0x8A: idle(); if (m8()) { m_a = (uint16_t)((m_a & 0xFF00) | (m_x & 0xFF)); nz8((uint8_t)m_a); }
               else { m_a = m_x; nz16(m_a); } break;                             // TXA
    case 0x98: idle(); if (m8()) { m_a = (uint16_t)((m_a & 0xFF00) | (m_y & 0xFF)); nz8((uint8_t)m_a); }
               else { m_a = m_y; nz16(m_a); } break;                             // TYA
    case 0x9B: idle(); setX(m_y, m_x); break;                                     // TXY
    case 0xBB: idle(); setX(m_x, m_y); break;                                     // TYX
    case 0xBA: idle(); setX(m_x, m_s); break;                                     // TSX
    case 0x9A: idle(); m_s = m_e ? (uint16_t)(0x100 | (m_x & 0xFF)) : m_x; break; // TXS
    case 0x1B: idle(); m_s = m_e ? (uint16_t)(0x100 | (m_a & 0xFF)) : m_a; break; // TCS
    case 0x3B: idle(); m_a = m_s; nz16(m_a); break;                               // TSC
    case 0x5B: idle(); m_d = m_a; nz16(m_d); break;                               // TCD
    case 0x7B: idle(); m_a = m_d; nz16(m_a); break;                               // TDC
    case 0xEB: idle(); idle(); m_a = (uint16_t)(m_a << 8 | m_a >> 8); nz8((uint8_t)m_a); break;   // XBA
    case 0xFB: {                                                  // XCE
        idle();
        const bool c = m_p & FC;
        flag(FC, m_e);
        m_e = c;
        if (m_e) { m_p |= FM | FX; m_x &= 0xFF; m_y &= 0xFF; m_s = (uint16_t)(0x100 | (m_s & 0xFF)); }
        break;
    }
    case 0xE8: idle(); setX(m_x, (uint16_t)(m_x + 1)); break;                     // INX
    case 0xC8: idle(); setX(m_y, (uint16_t)(m_y + 1)); break;                     // INY
    case 0xCA: idle(); setX(m_x, (uint16_t)(m_x - 1)); break;                     // DEX
    case 0x88: idle(); setX(m_y, (uint16_t)(m_y - 1)); break;                     // DEY
    case 0x18: idle(); m_p &= (uint8_t)~FC; break;
    case 0x38: idle(); m_p |= FC; break;
    case 0x58: idle(); m_p &= (uint8_t)~FI; break;
    case 0x78: idle(); m_p |= FI; break;
    case 0xD8: idle(); m_p &= (uint8_t)~FD; break;
    case 0xF8: idle(); m_p |= FD; break;
    case 0xB8: idle(); m_p &= (uint8_t)~FV; break;
    case 0xC2: { const uint8_t v = fetch(); idle(); setP((uint8_t)(m_p & ~v)); break; }   // REP
    case 0xE2: { const uint8_t v = fetch(); idle(); setP((uint8_t)(m_p | v)); break; }    // SEP
    case 0xEA: idle(); break;                                                     // NOP
    case 0x42: idleAt((uint32_t)m_pbr << 16 | m_pc); m_pc++; break;              // WDM: its byte is not read
    case 0xCB: idle(); idle(); idle(); m_wait = true; break;                      // WAI (then the bus stops)
    case 0xDB: idle(); idle(); idle(); m_stop = true; break;                      // STP
    case 0x44: case 0x54: {                                       // MVP, MVN: one byte a pass
        const uint8_t dst = fetch(), src = fetch();
        m_dbr = dst;
        const uint8_t v = read((uint32_t)src << 16 | m_x);
        write((uint32_t)dst << 16 | m_y, v);
        idle(); idle();
        const int d = op == 0x54 ? 1 : -1;
        if (x8()) { m_x = (uint16_t)((m_x + d) & 0xFF); m_y = (uint16_t)((m_y + d) & 0xFF); }
        else { m_x = (uint16_t)(m_x + d); m_y = (uint16_t)(m_y + d); }
        if (m_a-- != 0) m_pc = (uint16_t)(m_pc - 3);              // until A runs out ($FFFF)
        break;
    }
    default:
        break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Disassembler (WDC mnemonics; operand widths from the current M and X)
// ---------------------------------------------------------------------------
namespace {
const char* const MN[256] = {
    "BRK","ORA","COP","ORA","TSB","ORA","ASL","ORA","PHP","ORA","ASL","PHD","TSB","ORA","ASL","ORA",
    "BPL","ORA","ORA","ORA","TRB","ORA","ASL","ORA","CLC","ORA","INC","TCS","TRB","ORA","ASL","ORA",
    "JSR","AND","JSL","AND","BIT","AND","ROL","AND","PLP","AND","ROL","PLD","BIT","AND","ROL","AND",
    "BMI","AND","AND","AND","BIT","AND","ROL","AND","SEC","AND","DEC","TSC","BIT","AND","ROL","AND",
    "RTI","EOR","WDM","EOR","MVP","EOR","LSR","EOR","PHA","EOR","LSR","PHK","JMP","EOR","LSR","EOR",
    "BVC","EOR","EOR","EOR","MVN","EOR","LSR","EOR","CLI","EOR","PHY","TCD","JML","EOR","LSR","EOR",
    "RTS","ADC","PER","ADC","STZ","ADC","ROR","ADC","PLA","ADC","ROR","RTL","JMP","ADC","ROR","ADC",
    "BVS","ADC","ADC","ADC","STZ","ADC","ROR","ADC","SEI","ADC","PLY","TDC","JMP","ADC","ROR","ADC",
    "BRA","STA","BRL","STA","STY","STA","STX","STA","DEY","BIT","TXA","PHB","STY","STA","STX","STA",
    "BCC","STA","STA","STA","STY","STA","STX","STA","TYA","STA","TXS","TXY","STZ","STA","STZ","STA",
    "LDY","LDA","LDX","LDA","LDY","LDA","LDX","LDA","TAY","LDA","TAX","PLB","LDY","LDA","LDX","LDA",
    "BCS","LDA","LDA","LDA","LDY","LDA","LDX","LDA","CLV","LDA","TSX","TYX","LDY","LDA","LDX","LDA",
    "CPY","CMP","REP","CMP","CPY","CMP","DEC","CMP","INY","CMP","DEX","WAI","CPY","CMP","DEC","CMP",
    "BNE","CMP","CMP","CMP","PEI","CMP","DEC","CMP","CLD","CMP","PHX","STP","JML","CMP","DEC","CMP",
    "CPX","SBC","SEP","SBC","CPX","SBC","INC","SBC","INX","SBC","NOP","XBA","CPX","SBC","INC","SBC",
    "BEQ","SBC","SBC","SBC","PEA","SBC","INC","SBC","SED","SBC","PLX","XCE","JSR","SBC","INC","SBC"};
// Modes: 0 implied, 1 #M, 2 #X, 3 #8, 4 dp, 5 dp,X, 6 dp,Y, 7 (dp), 8 [dp], 9 (dp,X), 10 (dp),Y,
// 11 [dp],Y, 12 abs, 13 abs,X, 14 abs,Y, 15 long, 16 long,X, 17 sr,S, 18 (sr,S),Y, 19 rel8,
// 20 rel16, 21 (abs), 22 [abs], 23 (abs,X), 24 accumulator, 25 block move
const uint8_t MODE[256] = {
    3,9,3,17,4,4,4,8,0,1,24,0,12,12,12,15,   19,10,7,18,4,5,5,11,0,14,24,0,12,13,13,16,
    12,9,15,17,4,4,4,8,0,1,24,0,12,12,12,15, 19,10,7,18,5,5,5,11,0,14,24,0,13,13,13,16,
    0,9,3,17,25,4,4,8,0,1,24,0,12,12,12,15,  19,10,7,18,25,5,5,11,0,14,0,0,15,13,13,16,
    0,9,20,17,4,4,4,8,0,1,24,0,21,12,12,15,  19,10,7,18,5,5,5,11,0,14,0,0,23,13,13,16,
    19,9,20,17,4,4,4,8,0,1,0,0,12,12,12,15,  19,10,7,18,5,5,6,11,0,14,0,0,12,13,13,16,
    2,9,2,17,4,4,4,8,0,1,0,0,12,12,12,15,    19,10,7,18,5,5,6,11,0,14,0,0,13,13,14,16,
    2,9,3,17,4,4,4,8,0,1,0,0,12,12,12,15,    19,10,7,18,4,5,5,11,0,14,0,0,22,13,13,16,
    2,9,3,17,4,4,4,8,0,1,0,0,12,12,12,15,    19,10,7,18,12,5,5,11,0,14,0,0,23,13,13,16};
}  // namespace

int W65C816::disassembleOne(IBus* bus, uint32_t addr, char* buf, int bufsz) {
    auto b = [&](int i) { return bus ? bus->peek8((addr + (uint32_t)i) & 0xFFFFFF) : 0; };
    const uint8_t op = b(0);
    const int mode = MODE[op];
    int len = 1;
    char o[48] = "";
    auto w8 = [&]() { len = 2; return (unsigned)b(1); };
    auto w16 = [&]() { len = 3; return (unsigned)(b(1) | b(2) << 8); };
    auto w24 = [&]() { len = 4; return (unsigned)(b(1) | b(2) << 8 | b(3) << 16); };
    switch (mode) {
        case 1: if (m8()) std::snprintf(o, sizeof o, "#$%02X", w8()); else std::snprintf(o, sizeof o, "#$%04X", w16()); break;
        case 2: if (x8()) std::snprintf(o, sizeof o, "#$%02X", w8()); else std::snprintf(o, sizeof o, "#$%04X", w16()); break;
        case 3: std::snprintf(o, sizeof o, "#$%02X", w8()); break;
        case 4: std::snprintf(o, sizeof o, "$%02X", w8()); break;
        case 5: std::snprintf(o, sizeof o, "$%02X,X", w8()); break;
        case 6: std::snprintf(o, sizeof o, "$%02X,Y", w8()); break;
        case 7: std::snprintf(o, sizeof o, "($%02X)", w8()); break;
        case 8: std::snprintf(o, sizeof o, "[$%02X]", w8()); break;
        case 9: std::snprintf(o, sizeof o, "($%02X,X)", w8()); break;
        case 10: std::snprintf(o, sizeof o, "($%02X),Y", w8()); break;
        case 11: std::snprintf(o, sizeof o, "[$%02X],Y", w8()); break;
        case 12: std::snprintf(o, sizeof o, "$%04X", w16()); break;
        case 13: std::snprintf(o, sizeof o, "$%04X,X", w16()); break;
        case 14: std::snprintf(o, sizeof o, "$%04X,Y", w16()); break;
        case 15: std::snprintf(o, sizeof o, "$%06X", w24()); break;
        case 16: std::snprintf(o, sizeof o, "$%06X,X", w24()); break;
        case 17: std::snprintf(o, sizeof o, "$%02X,S", w8()); break;
        case 18: std::snprintf(o, sizeof o, "($%02X,S),Y", w8()); break;
        case 19: { const int8_t r = (int8_t)w8(); std::snprintf(o, sizeof o, "$%04X", (unsigned)((addr + 2 + r) & 0xFFFF)); break; }
        case 20: { const int16_t r = (int16_t)w16(); std::snprintf(o, sizeof o, "$%04X", (unsigned)((addr + 3 + r) & 0xFFFF)); break; }
        case 21: std::snprintf(o, sizeof o, "($%04X)", w16()); break;
        case 22: std::snprintf(o, sizeof o, "[$%04X]", w16()); break;
        case 23: std::snprintf(o, sizeof o, "($%04X,X)", w16()); break;
        case 24: std::snprintf(o, sizeof o, "A"); break;
        case 25: { len = 3; std::snprintf(o, sizeof o, "$%02X,$%02X", (unsigned)b(2), (unsigned)b(1)); break; }
    }
    if (op == 0xF4) { std::snprintf(o, sizeof o, "$%04X", w16()); }     // PEA
    if (op == 0xD4) { std::snprintf(o, sizeof o, "($%02X)", w8()); }    // PEI
    std::snprintf(buf, (size_t)bufsz, *o ? "%s %s" : "%s", MN[op], o);
    return len;
}

int W65C816::disassembleEntry(IBus* bus, uint32_t addr, void* entryOut) {
    auto* e = static_cast<DisasmEntry*>(entryOut);
    char buf[64];
    e->addr = addr;
    e->bytes = disassembleOne(bus, addr, buf, sizeof buf);
    std::string s(buf);
    const auto sp = s.find(' ');
    e->mnemonic = sp == std::string::npos ? s : s.substr(0, sp);
    e->operands = sp == std::string::npos ? "" : s.substr(sp + 1);
    e->complete = s;
    e->isCall = isCallAt(bus, addr) > 0;
    e->isReturn = isReturnAt(bus, addr);
    e->isBranch = (e->mnemonic.size() == 3 && e->mnemonic[0] == 'B' && e->mnemonic != "BIT" && e->mnemonic != "BRK") ||
                  e->mnemonic == "JMP" || e->mnemonic == "JML" || e->mnemonic == "BRL";
    e->isIllegal = false;
    e->targetAddr = 0;
    e->rawLen = (uint8_t)std::min(e->bytes, (int)DisasmEntry::MAX_RAW);
    for (int i = 0; i < e->rawLen; ++i) e->raw[i] = bus ? bus->peek8((addr + (uint32_t)i) & 0xFFFFFF) : 0;
    return e->bytes;
}

int W65C816::isCallAt(IBus* bus, uint32_t addr) {
    if (!bus) return 0;
    const uint8_t op = bus->peek8(addr & 0xFFFFFF);
    if (op == 0x20 || op == 0xFC) return 3;
    if (op == 0x22) return 4;
    return 0;
}

bool W65C816::isReturnAt(IBus* bus, uint32_t addr) {
    if (!bus) return false;
    const uint8_t op = bus->peek8(addr & 0xFFFFFF);
    return op == 0x60 || op == 0x6B || op == 0x40;
}

int W65C816::writeCallHarness(IBus* bus, uint32_t scratch, uint32_t routineAddr) {
    // JSL routine ; STP
    bus->write8(scratch, 0x22);
    bus->write8(scratch + 1, routineAddr & 0xFF);
    bus->write8(scratch + 2, (routineAddr >> 8) & 0xFF);
    bus->write8(scratch + 3, (routineAddr >> 16) & 0xFF);
    bus->write8(scratch + 4, 0xDB);
    return 5;
}
