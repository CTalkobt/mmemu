// Motorola 6809 core, after the MC6809/MC6809E data sheet (Motorola):
// instructions, addressing modes, flags, cycle counts, interrupts.

#include "cpu6809.h"
#include "libdebug/main/execution_observer.h"
#include "libcore/main/intercept.h"
#include "libtoolchain/main/idisasm.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace {
const RegDescriptor REGS[M6809::R_COUNT] = {
    {"A", RegWidth::R8, REGFLAG_ACCUMULATOR, nullptr, nullptr},
    {"B", RegWidth::R8, 0, nullptr, nullptr},
    {"D", RegWidth::R16, 0, nullptr, nullptr},
    {"X", RegWidth::R16, 0, nullptr, nullptr},
    {"Y", RegWidth::R16, 0, nullptr, nullptr},
    {"U", RegWidth::R16, 0, nullptr, nullptr},
    {"S", RegWidth::R16, REGFLAG_SP, nullptr, nullptr},
    {"PC", RegWidth::R16, REGFLAG_PC, nullptr, nullptr},
    {"DP", RegWidth::R8, 0, nullptr, nullptr},
    {"CC", RegWidth::R8, REGFLAG_STATUS, nullptr, "EFHINZVC"},
};
inline uint16_t sext8(uint8_t v) { return (uint16_t)(int16_t)(int8_t)v; }
}  // namespace

M6809::M6809() { reset(); }

void M6809::reset() {
    m_dp = 0;
    m_cc |= CC_I | CC_F;
    m_wait = WAIT_NONE;
    m_cwaiStacked = false;
    m_nmiArmed = false;
    m_nmiPending = false;
    m_pc = vec(0xE);
}

const RegDescriptor* M6809::regDescriptor(int idx) const {
    return idx >= 0 && idx < R_COUNT ? &REGS[idx] : nullptr;
}

uint32_t M6809::regRead(int idx) const {
    switch (idx) {
        case R_A: return m_a;
        case R_B: return m_b;
        case R_D: return d();
        case R_X: return m_x;
        case R_Y: return m_y;
        case R_U: return m_u;
        case R_S: return m_s;
        case R_PC: return m_pc;
        case R_DP: return m_dp;
        case R_CC: return m_cc;
    }
    return 0;
}

void M6809::regWrite(int idx, uint32_t v) {
    switch (idx) {
        case R_A: m_a = (uint8_t)v; break;
        case R_B: m_b = (uint8_t)v; break;
        case R_D: setD((uint16_t)v); break;
        case R_X: m_x = (uint16_t)v; break;
        case R_Y: m_y = (uint16_t)v; break;
        case R_U: m_u = (uint16_t)v; break;
        case R_S: m_s = (uint16_t)v; m_nmiArmed = true; break;
        case R_PC: setPc(v); break;
        case R_DP: m_dp = (uint8_t)v; break;
        case R_CC: m_cc = (uint8_t)v; break;
    }
}

void M6809::saveState(uint8_t* b) const {
    const uint16_t w[] = {m_x, m_y, m_u, m_s, m_pc};
    std::memcpy(b, w, sizeof w);
    b[10] = m_a; b[11] = m_b; b[12] = m_dp; b[13] = m_cc;
    b[14] = (uint8_t)((m_irq ? 1 : 0) | (m_firq ? 2 : 0) | (m_nmiPending ? 4 : 0) | (m_nmiArmed ? 8 : 0) |
                      (m_cwaiStacked ? 16 : 0));
    b[15] = (uint8_t)m_wait;
}

void M6809::loadState(const uint8_t* b) {
    uint16_t w[5];
    std::memcpy(w, b, sizeof w);
    m_x = w[0]; m_y = w[1]; m_u = w[2]; m_s = w[3]; m_pc = w[4];
    m_a = b[10]; m_b = b[11]; m_dp = b[12]; m_cc = b[13];
    m_irq = b[14] & 1; m_firq = b[14] & 2; m_nmiPending = b[14] & 4; m_nmiArmed = b[14] & 8;
    m_cwaiStacked = b[14] & 16;
    m_wait = (Wait)(b[15] % 3);
}

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------
uint8_t M6809::add8(uint8_t a, uint8_t b, int carry) {
    const unsigned r = a + b + carry;
    flag(CC_H, (a ^ b ^ r) & 0x10);
    flag(CC_V, (a ^ r) & (b ^ r) & 0x80);
    flag(CC_C, r & 0x100);
    nz8((uint8_t)r);
    return (uint8_t)r;
}

uint8_t M6809::sub8(uint8_t a, uint8_t b, int borrow) {
    const unsigned r = (unsigned)a - b - borrow;
    flag(CC_V, (a ^ b) & (a ^ r) & 0x80);
    flag(CC_C, r & 0x100);
    nz8((uint8_t)r);
    return (uint8_t)r;
}

uint16_t M6809::add16(uint16_t a, uint16_t b) {
    const uint32_t r = (uint32_t)a + b;
    flag(CC_V, (a ^ r) & (b ^ r) & 0x8000);
    flag(CC_C, r & 0x10000);
    nz16((uint16_t)r);
    return (uint16_t)r;
}

uint16_t M6809::sub16(uint16_t a, uint16_t b) {
    const uint32_t r = (uint32_t)a - b;
    flag(CC_V, (a ^ b) & (a ^ r) & 0x8000);
    flag(CC_C, r & 0x10000);
    nz16((uint16_t)r);
    return (uint16_t)r;
}

// The read-modify-write group by its low nibble (NEG COM LSR ROR ASR ASL
// ROL DEC INC TST CLR); $1, $2, $5 and $B as the 6809 decodes them
uint8_t M6809::rmw(int op, uint8_t v) {
    uint8_t r = v;
    switch (op) {
        case 0x0: case 0x1: r = sub8(0, v, 0); break;                         // NEG
        case 0x2:                                                             // NEG or COM by C
            r = (m_cc & CC_C) ? (uint8_t)~v : sub8(0, v, 0);
            if (m_cc & CC_C) { nz8(r); flag(CC_V, false); flag(CC_C, true); }
            break;
        case 0x3: r = (uint8_t)~v; nz8(r); flag(CC_V, false); flag(CC_C, true); break;   // COM
        case 0x4: case 0x5: flag(CC_C, v & 1); r = v >> 1; nz8(r); break;    // LSR
        case 0x6: r = (uint8_t)(v >> 1 | ((m_cc & CC_C) ? 0x80 : 0)); flag(CC_C, v & 1); nz8(r); break;  // ROR
        case 0x7: r = (uint8_t)(v >> 1 | (v & 0x80)); flag(CC_C, v & 1); nz8(r); break;  // ASR
        case 0x8:                                                             // ASL / LSL
            r = (uint8_t)(v << 1);
            flag(CC_C, v & 0x80); flag(CC_V, (v ^ r) & 0x80); nz8(r);
            break;
        case 0x9:                                                             // ROL
            r = (uint8_t)(v << 1 | (m_cc & CC_C));
            flag(CC_C, v & 0x80); flag(CC_V, (v ^ r) & 0x80); nz8(r);
            break;
        case 0xA: case 0xB: r = (uint8_t)(v - 1); flag(CC_V, v == 0x80); nz8(r); break;  // DEC
        case 0xC: r = (uint8_t)(v + 1); flag(CC_V, v == 0x7F); nz8(r); break;  // INC
        case 0xD: nz8(v); flag(CC_V, false); break;                          // TST
        case 0xF: r = 0; m_cc = (uint8_t)((m_cc & ~(CC_N | CC_V | CC_C)) | CC_Z); break;  // CLR
    }
    return r;
}

// ---------------------------------------------------------------------------
// Registers and addressing
// ---------------------------------------------------------------------------
uint16_t& M6809::indexReg(int r) {
    switch (r & 3) {
        case 0: return m_x;
        case 1: return m_y;
        case 2: return m_u;
        default: return m_s;
    }
}

// The indexed modes (data sheet table 2): the effective address, and the
// cycles the postbyte adds in m_extra
uint16_t M6809::indexed() {
    const uint8_t pb = fetch();
    uint16_t& r = indexReg(pb >> 5);
    uint16_t ea;
    if (!(pb & 0x80)) {                                      // 5-bit offset
        ea = (uint16_t)(r + ((pb & 0x10) ? (int)(pb & 0x1F) - 32 : (pb & 0x1F)));
        m_extra = 1;
        return ea;
    }
    switch (pb & 0x0F) {
        case 0x0: ea = r; r = (uint16_t)(r + 1); m_extra = 2; break;          // ,R+
        case 0x1: ea = r; r = (uint16_t)(r + 2); m_extra = 3; break;          // ,R++
        case 0x2: r = (uint16_t)(r - 1); ea = r; m_extra = 2; break;          // ,-R
        case 0x3: r = (uint16_t)(r - 2); ea = r; m_extra = 3; break;          // ,--R
        case 0x4: ea = r; m_extra = 0; break;                                 // ,R
        case 0x5: ea = (uint16_t)(r + sext8(m_b)); m_extra = 1; break;        // B,R
        case 0x6: ea = (uint16_t)(r + sext8(m_a)); m_extra = 1; break;        // A,R
        case 0x8: ea = (uint16_t)(r + sext8(fetch())); m_extra = 1; break;    // n8,R
        case 0x9: ea = (uint16_t)(r + fetch16()); m_extra = 4; break;         // n16,R
        case 0xB: ea = (uint16_t)(r + d()); m_extra = 4; break;               // D,R
        case 0xC: { const uint16_t o = sext8(fetch()); ea = (uint16_t)(m_pc + o); m_extra = 1; break; }   // n8,PC
        case 0xD: { const uint16_t o = fetch16(); ea = (uint16_t)(m_pc + o); m_extra = 5; break; }       // n16,PC
        case 0xF: ea = fetch16(); m_extra = 2; break;                         // [n16]
        default: ea = r; m_extra = 0; break;                                  // $7, $A, $E: undefined
    }
    if (pb & 0x10) { ea = rd16(ea); m_extra += 3; }                           // indirect
    return ea;
}

// TFR/EXG register codes; an 8-bit register reads as $FF00 | it in a 16-bit
// transfer, a missing one as $FFFF
uint16_t M6809::regTfr(int code) const {
    switch (code) {
        case 0x0: return d();
        case 0x1: return m_x;
        case 0x2: return m_y;
        case 0x3: return m_u;
        case 0x4: return m_s;
        case 0x5: return m_pc;
        case 0x8: return (uint16_t)(0xFF00 | m_a);
        case 0x9: return (uint16_t)(0xFF00 | m_b);
        case 0xA: return (uint16_t)(0xFF00 | m_cc);
        case 0xB: return (uint16_t)(0xFF00 | m_dp);
    }
    return 0xFFFF;
}

void M6809::setRegTfr(int code, uint16_t v) {
    switch (code) {
        case 0x0: setD(v); break;
        case 0x1: m_x = v; break;
        case 0x2: m_y = v; break;
        case 0x3: m_u = v; break;
        case 0x4: m_s = v; m_nmiArmed = true; break;
        case 0x5: m_pc = v; break;
        case 0x8: m_a = v & 0xFF; break;
        case 0x9: m_b = v & 0xFF; break;
        case 0xA: m_cc = v & 0xFF; break;
        case 0xB: m_dp = v & 0xFF; break;
    }
}

// ---------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------
void M6809::pushAll() {
    push16(m_s, m_pc);
    push16(m_s, m_u);
    push16(m_s, m_y);
    push16(m_s, m_x);
    push8(m_s, m_dp);
    push8(m_s, m_b);
    push8(m_s, m_a);
    push8(m_s, m_cc);
}

int M6809::interrupt() {
    const bool nmi = m_nmiPending && m_nmiArmed;
    const bool firq = m_firq && !(m_cc & CC_F);
    const bool irq = m_irq && !(m_cc & CC_I);
    if (!nmi && !firq && !irq) return 0;
    int c;
    if (nmi) {
        m_nmiPending = false;
        if (!m_cwaiStacked) { m_cc |= CC_E; pushAll(); }
        m_cc |= CC_I | CC_F;
        m_pc = vec(0xC);
        c = 19;
    } else if (firq) {
        if (!m_cwaiStacked) { m_cc &= (uint8_t)~CC_E; push16(m_s, m_pc); push8(m_s, m_cc); }
        m_cc |= CC_I | CC_F;
        m_pc = vec(0x6);
        c = 10;
    } else {
        if (!m_cwaiStacked) { m_cc |= CC_E; pushAll(); }
        m_cc |= CC_I;
        m_pc = vec(0x8);
        c = 19;
    }
    if (m_cwaiStacked) c = 0;                               // CWAI already counted its cycles
    m_cwaiStacked = false;
    m_wait = WAIT_NONE;
    return c ? c : 1;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------
int M6809::step() {
    if (m_pcHook && m_pcHook->armed(m_pc) && m_pcHook->fire(this, m_pc)) {
        m_cycles += 2;
        return 2;
    }
    if (m_nmiPending && !m_nmiArmed) m_nmiPending = false;   // NMI waits until S is loaded
    if (m_wait == WAIT_SYNC) {
        // SYNC: any interrupt line ends it; a masked one just continues
        if (m_nmiPending || m_firq || m_irq) {
            m_wait = WAIT_NONE;
            if (const int c = interrupt()) { m_cycles += (uint64_t)c; return c; }
        } else {
            m_cycles += 1;
            return 1;
        }
    }
    if (int c = interrupt()) { m_cycles += (uint64_t)c; return c; }
    if (m_wait == WAIT_CWAI) { m_cycles += 1; return 1; }

    if (m_observer) {
        if (m_observer->needsDisasm()) {
            DisasmEntry entry;
            disassembleEntry(m_bus, m_pc, &entry);
            if (!m_observer->onStep(this, m_bus, entry)) return 1;
        } else {
            m_observer->onStepLite(this, m_pc);
        }
    }
    const uint8_t op = fetch();
    int c;
    if (op == 0x10) c = execPage2(fetch());
    else if (op == 0x11) c = execPage3(fetch());
    else c = execPage0(op);
    m_cycles += (uint64_t)c;
    return c;
}

int M6809::execPage0(uint8_t op) {
    const int hi = op >> 4, lo = op & 0x0F;
    // $00-$0F, $60-$6F, $70-$7F: read-modify-write in memory, JMP
    if (hi == 0x0 || hi == 0x6 || hi == 0x7) {
        uint16_t ea;
        int base;
        if (hi == 0x0) { ea = (uint16_t)(m_dp << 8 | fetch()); base = 6; m_extra = 0; }
        else if (hi == 0x6) { ea = indexed(); base = 6; }
        else { ea = fetch16(); base = 7; m_extra = 0; }
        if (lo == 0xE) { m_pc = ea; return base - 3 + m_extra; }                 // JMP
        const uint8_t v = rd(ea);
        const uint8_t r = rmw(lo, v);
        if (lo != 0xD) wr(ea, r);                                                 // TST only reads
        return base + m_extra;
    }
    if (hi == 0x4 || hi == 0x5) {                                                // A / B inherent
        uint8_t& reg = hi == 0x4 ? m_a : m_b;
        if (lo == 0xE) { return 2; }                                              // undefined
        reg = rmw(lo, reg);
        return 2;
    }
    if (hi == 0x2) {                                                             // branches
        const uint16_t o = sext8(fetch());
        bool t;
        const bool n = m_cc & CC_N, v = m_cc & CC_V, z = m_cc & CC_Z, cf = m_cc & CC_C;
        switch (lo) {
            case 0x0: t = true; break;  case 0x1: t = false; break;
            case 0x2: t = !(cf || z); break;  case 0x3: t = cf || z; break;
            case 0x4: t = !cf; break;  case 0x5: t = cf; break;
            case 0x6: t = !z; break;  case 0x7: t = z; break;
            case 0x8: t = !v; break;  case 0x9: t = v; break;
            case 0xA: t = !n; break;  case 0xB: t = n; break;
            case 0xC: t = n == v; break;  case 0xD: t = n != v; break;
            case 0xE: t = !z && n == v; break;  default: t = z || n != v; break;
        }
        if (t) m_pc = (uint16_t)(m_pc + o);
        return 3;
    }
    if (hi == 0x1) {
        switch (lo) {
            case 0x2: return 2;                                                   // NOP
            case 0x3: m_wait = WAIT_SYNC; return 4;                               // SYNC
            case 0x6: { const uint16_t o = fetch16(); m_pc = (uint16_t)(m_pc + o); return 5; }   // LBRA
            case 0x7: { const uint16_t o = fetch16(); push16(m_s, m_pc); m_pc = (uint16_t)(m_pc + o); return 9; }  // LBSR
            case 0x9: {                                                           // DAA
                const uint8_t lsn = m_a & 0x0F, msn = m_a >> 4;
                uint8_t cf = 0;
                if ((m_cc & CC_H) || lsn > 9) cf |= 0x06;
                if ((m_cc & CC_C) || msn > 9 || (msn > 8 && lsn > 9)) cf |= 0x60;
                const unsigned r = m_a + cf;
                if (r & 0x100) m_cc |= CC_C;
                m_a = (uint8_t)r;
                nz8(m_a);
                flag(CC_V, false);
                return 2;
            }
            case 0xA: m_cc |= fetch(); return 3;                                  // ORCC
            case 0xC: m_cc &= fetch(); return 3;                                  // ANDCC
            case 0xD: m_a = (m_b & 0x80) ? 0xFF : 0x00; nz16(d()); return 2;      // SEX
            case 0xE: {                                                           // EXG
                const uint8_t pb = fetch();
                const uint16_t s = regTfr(pb >> 4), t = regTfr(pb & 15);
                setRegTfr(pb >> 4, t);
                setRegTfr(pb & 15, s);
                return 8;
            }
            case 0xF: { const uint8_t pb = fetch(); setRegTfr(pb & 15, regTfr(pb >> 4)); return 6; }   // TFR
        }
        return 2;                                                                 // undefined
    }
    if (hi == 0x3) {
        switch (lo) {
            case 0x0: { m_x = indexed(); flag(CC_Z, !m_x); return 4 + m_extra; }  // LEAX
            case 0x1: { m_y = indexed(); flag(CC_Z, !m_y); return 4 + m_extra; }  // LEAY
            case 0x2: { const uint16_t ea = indexed(); m_s = ea; m_nmiArmed = true; return 4 + m_extra; }  // LEAS
            case 0x3: { const uint16_t ea = indexed(); m_u = ea; return 4 + m_extra; }  // LEAU
            case 0x4: case 0x6: {                                                 // PSHS, PSHU
                const uint8_t pb = fetch();
                uint16_t& sp = lo == 0x4 ? m_s : m_u;
                const uint16_t other = lo == 0x4 ? m_u : m_s;
                int c = 5;
                if (pb & 0x80) { push16(sp, m_pc); c += 2; }
                if (pb & 0x40) { push16(sp, other); c += 2; }
                if (pb & 0x20) { push16(sp, m_y); c += 2; }
                if (pb & 0x10) { push16(sp, m_x); c += 2; }
                if (pb & 0x08) { push8(sp, m_dp); c += 1; }
                if (pb & 0x04) { push8(sp, m_b); c += 1; }
                if (pb & 0x02) { push8(sp, m_a); c += 1; }
                if (pb & 0x01) { push8(sp, m_cc); c += 1; }
                return c;
            }
            case 0x5: case 0x7: {                                                 // PULS, PULU
                const uint8_t pb = fetch();
                uint16_t& sp = lo == 0x5 ? m_s : m_u;
                int c = 5;
                if (pb & 0x01) { m_cc = pull8(sp); c += 1; }
                if (pb & 0x02) { m_a = pull8(sp); c += 1; }
                if (pb & 0x04) { m_b = pull8(sp); c += 1; }
                if (pb & 0x08) { m_dp = pull8(sp); c += 1; }
                if (pb & 0x10) { m_x = pull16(sp); c += 2; }
                if (pb & 0x20) { m_y = pull16(sp); c += 2; }
                if (pb & 0x40) {
                    const uint16_t v = pull16(sp);
                    if (lo == 0x5) m_u = v; else { m_s = v; m_nmiArmed = true; }
                    c += 2;
                }
                if (pb & 0x80) { m_pc = pull16(sp); c += 2; }
                return c;
            }
            case 0x9: m_pc = pull16(m_s); return 5;                               // RTS
            case 0xA: m_x = (uint16_t)(m_x + m_b); return 3;                      // ABX
            case 0xB: {                                                           // RTI
                m_cc = pull8(m_s);
                if (m_cc & CC_E) {
                    m_a = pull8(m_s); m_b = pull8(m_s); m_dp = pull8(m_s);
                    m_x = pull16(m_s); m_y = pull16(m_s); m_u = pull16(m_s);
                    m_pc = pull16(m_s);
                    return 15;
                }
                m_pc = pull16(m_s);
                return 6;
            }
            case 0xC: {                                                           // CWAI
                m_cc &= fetch();
                m_cc |= CC_E;
                pushAll();
                m_wait = WAIT_CWAI;
                m_cwaiStacked = true;
                return 20;
            }
            case 0xD: {                                                           // MUL
                const uint16_t r = (uint16_t)(m_a * m_b);
                setD(r);
                flag(CC_Z, !r);
                flag(CC_C, r & 0x80);
                return 11;
            }
            case 0xF:                                                             // SWI
                m_cc |= CC_E;
                pushAll();
                m_cc |= CC_I | CC_F;
                m_pc = vec(0xA);
                return 19;
        }
        return 2;                                                                 // undefined
    }
    // $80-$FF: the accumulator and register group; mode by bits 4-5
    const int mode = (op >> 4) & 3;                                               // 0 imm, 1 dir, 2 idx, 3 ext
    const bool bSide = op & 0x40;
    auto ea = [&]() -> uint16_t {
        m_extra = 0;
        if (mode == 1) return (uint16_t)(m_dp << 8 | fetch());
        if (mode == 2) return indexed();
        return fetch16();
    };
    auto cyc8 = [&]() { return (mode == 0 ? 2 : mode == 1 ? 4 : mode == 2 ? 4 : 5) + m_extra; };
    auto cyc16 = [&](int base) { return (mode == 0 ? base : mode == 1 ? base + 2 : mode == 2 ? base + 2 : base + 3) + m_extra; };
    auto read8op = [&]() -> uint8_t { if (mode == 0) { m_extra = 0; return fetch(); } return rd(ea()); };
    auto read16op = [&]() -> uint16_t { if (mode == 0) { m_extra = 0; return fetch16(); } return rd16(ea()); };
    uint8_t& acc = bSide ? m_b : m_a;
    switch (lo) {
        case 0x0: acc = sub8(acc, read8op(), 0); return cyc8();                   // SUB
        case 0x1: sub8(acc, read8op(), 0); return cyc8();                         // CMP
        case 0x2: acc = sub8(acc, read8op(), m_cc & CC_C); return cyc8();         // SBC
        case 0x3: {                                                               // SUBD / ADDD
            const uint16_t v = read16op();
            setD(bSide ? add16(d(), v) : sub16(d(), v));
            return cyc16(4);
        }
        case 0x4: acc &= read8op(); nz8(acc); flag(CC_V, false); return cyc8();   // AND
        case 0x5: { const uint8_t r = acc & read8op(); nz8(r); flag(CC_V, false); return cyc8(); }   // BIT
        case 0x6: acc = read8op(); nz8(acc); flag(CC_V, false); return cyc8();    // LD
        case 0x7: {                                                               // ST
            if (mode == 0) return 2;                                              // undefined
            const uint16_t a = ea();
            wr(a, acc); nz8(acc); flag(CC_V, false);
            return cyc8();
        }
        case 0x8: acc ^= read8op(); nz8(acc); flag(CC_V, false); return cyc8();   // EOR
        case 0x9: acc = add8(acc, read8op(), m_cc & CC_C); return cyc8();         // ADC
        case 0xA: acc |= read8op(); nz8(acc); flag(CC_V, false); return cyc8();   // OR
        case 0xB: acc = add8(acc, read8op(), 0); return cyc8();                   // ADD
        case 0xC:
            if (!bSide) { sub16(m_x, read16op()); return cyc16(4); }              // CMPX
            setD(read16op()); nz16(d()); flag(CC_V, false); return cyc16(3);      // LDD
        case 0xD:
            if (!bSide) {
                if (mode == 0) {                                                  // BSR
                    const uint16_t o = sext8(fetch());
                    push16(m_s, m_pc);
                    m_pc = (uint16_t)(m_pc + o);
                    return 7;
                }
                const uint16_t a = ea();                                          // JSR
                push16(m_s, m_pc);
                m_pc = a;
                return (mode == 1 ? 7 : mode == 2 ? 7 : 8) + m_extra;
            }
            if (mode == 0) return 2;                                              // undefined
            { const uint16_t a = ea(); wr16(a, d()); nz16(d()); flag(CC_V, false); return cyc16(3); }   // STD
        case 0xE: {                                                               // LDX / LDU
            uint16_t& r = bSide ? m_u : m_x;
            r = read16op(); nz16(r); flag(CC_V, false);
            return cyc16(3);
        }
        default: {                                                                // STX / STU
            if (mode == 0) return 2;                                              // undefined
            const uint16_t r = bSide ? m_u : m_x;
            const uint16_t a = ea();
            wr16(a, r); nz16(r); flag(CC_V, false);
            return cyc16(3);
        }
    }
}

int M6809::execPage2(uint8_t op) {
    if (op >= 0x21 && op <= 0x2F) {                                              // long branches
        const uint16_t o = fetch16();
        const bool n = m_cc & CC_N, v = m_cc & CC_V, z = m_cc & CC_Z, cf = m_cc & CC_C;
        bool t;
        switch (op & 0x0F) {
            case 0x1: t = false; break;
            case 0x2: t = !(cf || z); break;  case 0x3: t = cf || z; break;
            case 0x4: t = !cf; break;  case 0x5: t = cf; break;
            case 0x6: t = !z; break;  case 0x7: t = z; break;
            case 0x8: t = !v; break;  case 0x9: t = v; break;
            case 0xA: t = !n; break;  case 0xB: t = n; break;
            case 0xC: t = n == v; break;  case 0xD: t = n != v; break;
            case 0xE: t = !z && n == v; break;  default: t = z || n != v; break;
        }
        if (t) { m_pc = (uint16_t)(m_pc + o); return 6; }
        return 5;
    }
    if (op == 0x3F) {                                                            // SWI2
        m_cc |= CC_E;
        pushAll();
        m_pc = vec(0x4);
        return 20;
    }
    const int mode = (op >> 4) & 3;
    if (op < 0x80) return 2;                                                     // undefined
    auto ea = [&]() -> uint16_t {
        m_extra = 0;
        if (mode == 1) return (uint16_t)(m_dp << 8 | fetch());
        if (mode == 2) return indexed();
        return fetch16();
    };
    auto read16op = [&]() -> uint16_t { if (mode == 0) { m_extra = 0; return fetch16(); } return rd16(ea()); };
    auto cyc = [&](int base) { return (mode == 0 ? base : mode == 1 ? base + 2 : mode == 2 ? base + 2 : base + 3) + m_extra; };
    switch (op & 0xCF) {
        case 0x83: sub16(d(), read16op()); return cyc(5);                        // CMPD
        case 0x8C: sub16(m_y, read16op()); return cyc(5);                        // CMPY
        case 0x8E: m_y = read16op(); nz16(m_y); flag(CC_V, false); return cyc(4); // LDY
        case 0x8F:                                                               // STY
            if (mode == 0) return 2;
            { const uint16_t a = ea(); wr16(a, m_y); nz16(m_y); flag(CC_V, false); return cyc(4); }
        case 0xCE: m_s = read16op(); m_nmiArmed = true; nz16(m_s); flag(CC_V, false); return cyc(4);  // LDS
        case 0xCF:                                                               // STS
            if (mode == 0) return 2;
            { const uint16_t a = ea(); wr16(a, m_s); nz16(m_s); flag(CC_V, false); return cyc(4); }
    }
    return 2;                                                                    // undefined
}

int M6809::execPage3(uint8_t op) {
    if (op == 0x3F) {                                                            // SWI3
        m_cc |= CC_E;
        pushAll();
        m_pc = vec(0x2);
        return 20;
    }
    if (op < 0x80 || op >= 0xC0) return 2;                                       // undefined
    const int mode = (op >> 4) & 3;
    auto read16op = [&]() -> uint16_t {
        m_extra = 0;
        if (mode == 0) return fetch16();
        if (mode == 1) return rd16((uint16_t)(m_dp << 8 | fetch()));
        if (mode == 2) return rd16(indexed());
        return rd16(fetch16());
    };
    auto cyc = [&](int base) { return (mode == 0 ? base : mode == 1 ? base + 2 : mode == 2 ? base + 2 : base + 3) + m_extra; };
    switch (op & 0x0F) {
        case 0x3: sub16(m_u, read16op()); return cyc(5);                         // CMPU
        case 0xC: sub16(m_s, read16op()); return cyc(5);                         // CMPS
    }
    return 2;
}

// ---------------------------------------------------------------------------
// Disassembler (Motorola mnemonics)
// ---------------------------------------------------------------------------
namespace {
const char* const RMW[16] = {"NEG", "NEG", "NGC", "COM", "LSR", "LSR", "ROR", "ASR",
                             "ASL", "ROL", "DEC", "DEC", "INC", "TST", "JMP", "CLR"};
const char* const BR[16] = {"BRA", "BRN", "BHI", "BLS", "BCC", "BCS", "BNE", "BEQ",
                            "BVC", "BVS", "BPL", "BMI", "BGE", "BLT", "BGT", "BLE"};
const char* const ACCA[16] = {"SUBA", "CMPA", "SBCA", "SUBD", "ANDA", "BITA", "LDA", "STA",
                              "EORA", "ADCA", "ORA", "ADDA", "CMPX", "JSR", "LDX", "STX"};
const char* const ACCB[16] = {"SUBB", "CMPB", "SBCB", "ADDD", "ANDB", "BITB", "LDB", "STB",
                              "EORB", "ADCB", "ORB", "ADDB", "LDD", "STD", "LDU", "STU"};
const char* const TREG[16] = {"D", "X", "Y", "U", "S", "PC", "?", "?", "A", "B", "CC", "DP", "?", "?", "?", "?"};

struct Dis {
    IBus* bus;
    uint32_t pc;
    int len = 0;
    uint8_t byte() { return bus ? bus->peek8((pc + len++) & 0xFFFF) : 0; }
    uint16_t word() { uint8_t h = byte(); return (uint16_t)(h << 8 | byte()); }
    std::string hex2(unsigned v) { char b[8]; std::snprintf(b, sizeof b, "$%02X", v & 0xFF); return b; }
    std::string hex4(unsigned v) { char b[8]; std::snprintf(b, sizeof b, "$%04X", v & 0xFFFF); return b; }
    std::string idx() {
        static const char* R[4] = {"X", "Y", "U", "S"};
        const uint8_t pb = byte();
        const std::string r = R[(pb >> 5) & 3];
        if (!(pb & 0x80)) {
            const int o = (pb & 0x10) ? (int)(pb & 0x1F) - 32 : (pb & 0x1F);
            return std::to_string(o) + "," + r;
        }
        std::string s;
        switch (pb & 0x0F) {
            case 0x0: s = "," + r + "+"; break;
            case 0x1: s = "," + r + "++"; break;
            case 0x2: s = ",-" + r; break;
            case 0x3: s = ",--" + r; break;
            case 0x4: s = "," + r; break;
            case 0x5: s = "B," + r; break;
            case 0x6: s = "A," + r; break;
            case 0x8: s = std::to_string((int8_t)byte()) + "," + r; break;
            case 0x9: s = hex4(word()) + "," + r; break;
            case 0xB: s = "D," + r; break;
            case 0xC: { const int8_t o = (int8_t)byte(); s = hex4(pc + len + o) + ",PCR"; break; }
            case 0xD: { const uint16_t o = word(); s = hex4(pc + len + o) + ",PCR"; break; }
            case 0xF: s = hex4(word()); break;
            default: s = "?"; break;
        }
        return (pb & 0x10) ? "[" + s + "]" : s;
    }
    std::string operand(int mode, bool wide) {
        switch (mode) {
            case 0: return "#" + (wide ? hex4(word()) : hex2(byte()));
            case 1: return "<" + hex2(byte());
            case 2: return idx();
            default: return hex4(word());
        }
    }
    std::string regList(uint8_t pb, bool u) {
        static const char* N[8] = {"CC", "A", "B", "DP", "X", "Y", "S", "PC"};
        std::string s;
        for (int i = 0; i < 8; ++i) {
            if (!(pb & (1 << i))) continue;
            if (!s.empty()) s += ",";
            s += (i == 6) ? (u ? "S" : "U") : N[i];
        }
        return s;
    }
};
}  // namespace

int M6809::disassembleOne(IBus* bus, uint32_t addr, char* buf, int bufsz) {
    Dis d{bus, addr};
    std::string s;
    const uint8_t op = d.byte();
    const int hi = op >> 4, lo = op & 15;
    if (op == 0x10 || op == 0x11) {
        const uint8_t op2 = d.byte();
        const int mode = (op2 >> 4) & 3;
        if (op == 0x10 && op2 >= 0x21 && op2 <= 0x2F) {
            const uint16_t o = d.word();
            s = std::string("L") + BR[op2 & 15] + " " + d.hex4(addr + d.len + o);
        } else if (op2 == 0x3F) {
            s = op == 0x10 ? "SWI2" : "SWI3";
        } else if (op == 0x10 && op2 >= 0x80) {
            const char* n = nullptr;
            switch (op2 & 0xCF) {
                case 0x83: n = "CMPD"; break; case 0x8C: n = "CMPY"; break; case 0x8E: n = "LDY"; break;
                case 0x8F: n = "STY"; break; case 0xCE: n = "LDS"; break; case 0xCF: n = "STS"; break;
            }
            s = n ? std::string(n) + " " + d.operand(mode, true) : "???";
        } else if (op == 0x11 && op2 >= 0x80 && op2 < 0xC0 && ((op2 & 15) == 3 || (op2 & 15) == 0xC)) {
            s = std::string((op2 & 15) == 3 ? "CMPU" : "CMPS") + " " + d.operand(mode, true);
        } else {
            s = "???";
        }
    } else if (hi == 0x0 || hi == 0x6 || hi == 0x7) {
        s = std::string(RMW[lo]) + " " + d.operand(hi == 0 ? 1 : hi == 6 ? 2 : 3, false);
    } else if (hi == 0x4 || hi == 0x5) {
        s = std::string(RMW[lo]) + (hi == 4 ? "A" : "B");
    } else if (hi == 0x2) {
        const int8_t o = (int8_t)d.byte();
        s = std::string(BR[lo]) + " " + d.hex4(addr + d.len + o);
    } else if (hi == 0x1) {
        switch (lo) {
            case 0x2: s = "NOP"; break;
            case 0x3: s = "SYNC"; break;
            case 0x6: { const uint16_t o = d.word(); s = "LBRA " + d.hex4(addr + d.len + o); break; }
            case 0x7: { const uint16_t o = d.word(); s = "LBSR " + d.hex4(addr + d.len + o); break; }
            case 0x9: s = "DAA"; break;
            case 0xA: s = "ORCC #" + d.hex2(d.byte()); break;
            case 0xC: s = "ANDCC #" + d.hex2(d.byte()); break;
            case 0xD: s = "SEX"; break;
            case 0xE: case 0xF: {
                const uint8_t pb = d.byte();
                s = std::string(lo == 0xE ? "EXG " : "TFR ") + TREG[pb >> 4] + "," + TREG[pb & 15];
                break;
            }
            default: s = "???"; break;
        }
    } else if (hi == 0x3) {
        switch (lo) {
            case 0x0: s = "LEAX " + d.idx(); break;
            case 0x1: s = "LEAY " + d.idx(); break;
            case 0x2: s = "LEAS " + d.idx(); break;
            case 0x3: s = "LEAU " + d.idx(); break;
            case 0x4: s = "PSHS " + d.regList(d.byte(), false); break;
            case 0x5: s = "PULS " + d.regList(d.byte(), false); break;
            case 0x6: s = "PSHU " + d.regList(d.byte(), true); break;
            case 0x7: s = "PULU " + d.regList(d.byte(), true); break;
            case 0x9: s = "RTS"; break;
            case 0xA: s = "ABX"; break;
            case 0xB: s = "RTI"; break;
            case 0xC: s = "CWAI #" + d.hex2(d.byte()); break;
            case 0xD: s = "MUL"; break;
            case 0xF: s = "SWI"; break;
            default: s = "???"; break;
        }
    } else {
        const int mode = (op >> 4) & 3;
        const bool bSide = op & 0x40;
        if (!bSide && lo == 0xD && mode == 0) {
            const int8_t o = (int8_t)d.byte();
            s = "BSR " + d.hex4(addr + d.len + o);
        } else {
            const bool wide = lo == 3 || lo >= 0xC;
            s = std::string(bSide ? ACCB[lo] : ACCA[lo]) + " " + d.operand(mode, wide);
        }
    }
    std::snprintf(buf, (size_t)bufsz, "%s", s.c_str());
    return d.len;
}

int M6809::disassembleEntry(IBus* bus, uint32_t addr, void* entryOut) {
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
    e->isBranch = e->mnemonic.size() >= 2 && (e->mnemonic[0] == 'B' || e->mnemonic[0] == 'L' || e->mnemonic == "JMP") &&
                  e->mnemonic != "BITA" && e->mnemonic != "BITB" && !e->isCall;
    e->isIllegal = s == "???";
    e->targetAddr = 0;
    e->rawLen = (uint8_t)std::min(e->bytes, (int)DisasmEntry::MAX_RAW);
    for (int i = 0; i < e->rawLen; ++i) e->raw[i] = bus ? bus->peek8((addr + i) & 0xFFFF) : 0;
    return e->bytes;
}

int M6809::isCallAt(IBus* bus, uint32_t addr) {
    if (!bus) return 0;
    const uint8_t op = bus->peek8(addr & 0xFFFF);
    switch (op) {
        case 0x8D: return 2;                                // BSR
        case 0x17: return 3;                                // LBSR
        case 0x9D: return 2;                                // JSR direct
        case 0xBD: return 3;                                // JSR extended
        case 0xAD: { char b[64]; return disassembleOne(bus, addr, b, sizeof b); }   // JSR indexed
        case 0x3F: return 1;                                // SWI
    }
    return 0;
}

bool M6809::isReturnAt(IBus* bus, uint32_t addr) {
    if (!bus) return false;
    const uint8_t op = bus->peek8(addr & 0xFFFF);
    if (op == 0x39 || op == 0x3B) return true;              // RTS, RTI
    if (op == 0x35 || op == 0x37) return bus->peek8((addr + 1) & 0xFFFF) & 0x80;   // PULS/PULU with PC
    return false;
}

bool M6809::isProgramEnd(IBus* bus) { (void)bus; return m_wait == WAIT_SYNC && (m_cc & CC_I) && (m_cc & CC_F); }

int M6809::writeCallHarness(IBus* bus, uint32_t scratch, uint32_t routineAddr) {
    // JSR routine ; ORCC #$50 ; SYNC (ends with interrupts masked)
    bus->write8(scratch, 0xBD);
    bus->write8(scratch + 1, (routineAddr >> 8) & 0xFF);
    bus->write8(scratch + 2, routineAddr & 0xFF);
    bus->write8(scratch + 3, 0x1A);
    bus->write8(scratch + 4, 0x50);
    bus->write8(scratch + 5, 0x13);
    return 6;
}
