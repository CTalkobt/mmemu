// Zilog Z80 core. Decoding after "Decoding Z80 Opcodes" (Cristian Dinu):
// op = x(7-6) y(5-3) z(2-0), y = p(5-4) q(3). Flags, undocumented
// behaviour and timings after "The Undocumented Z80 Documented" (Sean
// Young); MEMPTR after the "MEMPTR, esoteric register of the Z80" notes;
// SCF/CCF's X/Y from Q after Patrik Rak.

#include "cpu_z80.h"
#include "libdebug/main/execution_observer.h"
#include "libcore/main/intercept.h"
#include "libtoolchain/main/idisasm.h"
#include <cstdio>
#include <cstring>

namespace {
// Parity: FP set for an even number of 1 bits
inline uint8_t parity(uint8_t v) {
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (v & 1) ? 0 : Z80::FP;
}
inline uint8_t sz53(uint8_t v) { return (uint8_t)((v & (Z80::FS | Z80::FY | Z80::FX)) | (v ? 0 : Z80::FZ)); }
inline uint8_t sz53p(uint8_t v) { return (uint8_t)(sz53(v) | parity(v)); }

const RegDescriptor REGS[Z80::R_COUNT] = {
    {"AF", RegWidth::R16, REGFLAG_STATUS | REGFLAG_ACCUMULATOR, nullptr, "SZYHXPNC"},
    {"BC", RegWidth::R16, 0, nullptr, nullptr},
    {"DE", RegWidth::R16, 0, nullptr, nullptr},
    {"HL", RegWidth::R16, 0, nullptr, nullptr},
    {"IX", RegWidth::R16, 0, nullptr, nullptr},
    {"IY", RegWidth::R16, 0, nullptr, nullptr},
    {"SP", RegWidth::R16, REGFLAG_SP, nullptr, nullptr},
    {"PC", RegWidth::R16, REGFLAG_PC, nullptr, nullptr},
    {"AF'", RegWidth::R16, REGFLAG_SHADOW, nullptr, nullptr},
    {"BC'", RegWidth::R16, REGFLAG_SHADOW, nullptr, nullptr},
    {"DE'", RegWidth::R16, REGFLAG_SHADOW, nullptr, nullptr},
    {"HL'", RegWidth::R16, REGFLAG_SHADOW, nullptr, nullptr},
    {"I", RegWidth::R8, 0, nullptr, nullptr},
    {"R", RegWidth::R8, 0, nullptr, nullptr},
    {"IM", RegWidth::R8, REGFLAG_INTERNAL, nullptr, nullptr},
    {"IFF", RegWidth::R8, REGFLAG_INTERNAL, nullptr, nullptr},    // bit 0 IFF1, bit 1 IFF2
    {"WZ", RegWidth::R16, REGFLAG_INTERNAL, nullptr, nullptr},
};
}  // namespace

Z80::Z80() { reset(); }

void Z80::reset() {
    m_pc = 0; m_i = 0; m_r = 0; m_im = 0;
    m_iff1 = m_iff2 = false;
    m_halted = false;
    m_eiDelay = false;
    m_nmiPending = false;
    m_a = 0xFF; m_f = 0xFF; m_sp = 0xFFFF;
    m_q = m_qNext = 0;
}

const RegDescriptor* Z80::regDescriptor(int idx) const {
    return idx >= 0 && idx < R_COUNT ? &REGS[idx] : nullptr;
}

uint32_t Z80::regRead(int idx) const {
    switch (idx) {
        case R_AF: return (uint32_t)(m_a << 8 | m_f);
        case R_BC: return bc();
        case R_DE: return de();
        case R_HL: return hl();
        case R_IX: return m_ix;
        case R_IY: return m_iy;
        case R_SP: return m_sp;
        case R_PC: return m_pc;
        case R_AF2: return m_af2;
        case R_BC2: return m_bc2;
        case R_DE2: return m_de2;
        case R_HL2: return m_hl2;
        case R_I: return m_i;
        case R_R: return m_r;
        case R_IM: return m_im;
        case R_IFF: return (m_iff1 ? 1u : 0u) | (m_iff2 ? 2u : 0u);
        case R_WZ: return m_wz;
    }
    return 0;
}

void Z80::regWrite(int idx, uint32_t v) {
    switch (idx) {
        case R_AF: m_a = (uint8_t)(v >> 8); m_f = (uint8_t)v; break;
        case R_BC: m_b = (uint8_t)(v >> 8); m_c = (uint8_t)v; break;
        case R_DE: m_d = (uint8_t)(v >> 8); m_e = (uint8_t)v; break;
        case R_HL: m_h = (uint8_t)(v >> 8); m_l = (uint8_t)v; break;
        case R_IX: m_ix = (uint16_t)v; break;
        case R_IY: m_iy = (uint16_t)v; break;
        case R_SP: m_sp = (uint16_t)v; break;
        case R_PC: m_pc = (uint16_t)v; m_halted = false; break;
        case R_AF2: m_af2 = (uint16_t)v; break;
        case R_BC2: m_bc2 = (uint16_t)v; break;
        case R_DE2: m_de2 = (uint16_t)v; break;
        case R_HL2: m_hl2 = (uint16_t)v; break;
        case R_I: m_i = (uint8_t)v; break;
        case R_R: m_r = (uint8_t)v; break;
        case R_IM: m_im = (uint8_t)(v & 3); break;
        case R_IFF: m_iff1 = v & 1; m_iff2 = v & 2; break;
        case R_WZ: m_wz = (uint16_t)v; break;
    }
}

void Z80::saveState(uint8_t* b) const {
    uint16_t w[] = {(uint16_t)(m_a << 8 | m_f), bc(), de(), hl(), m_ix, m_iy, m_sp, m_pc,
                    m_af2, m_bc2, m_de2, m_hl2, m_wz};
    std::memcpy(b, w, sizeof w);
    b += sizeof w;
    b[0] = m_i; b[1] = m_r; b[2] = m_im;
    b[3] = (uint8_t)((m_iff1 ? 1 : 0) | (m_iff2 ? 2 : 0) | (m_halted ? 4 : 0) | (m_eiDelay ? 8 : 0) |
                     (m_irqLine ? 16 : 0) | (m_nmiPending ? 32 : 0));
    b[4] = m_qNext;
}

void Z80::loadState(const uint8_t* b) {
    uint16_t w[13];
    std::memcpy(w, b, sizeof w);
    b += sizeof w;
    regWrite(R_AF, w[0]); regWrite(R_BC, w[1]); regWrite(R_DE, w[2]); regWrite(R_HL, w[3]);
    m_ix = w[4]; m_iy = w[5]; m_sp = w[6]; m_pc = w[7];
    m_af2 = w[8]; m_bc2 = w[9]; m_de2 = w[10]; m_hl2 = w[11]; m_wz = w[12];
    m_i = b[0]; m_r = b[1]; m_im = b[2];
    m_iff1 = b[3] & 1; m_iff2 = b[3] & 2; m_halted = b[3] & 4; m_eiDelay = b[3] & 8;
    m_irqLine = b[3] & 16; m_nmiPending = b[3] & 32;
    m_qNext = b[4];
}

// ---------------------------------------------------------------------------
// Register helpers
// ---------------------------------------------------------------------------

uint16_t& Z80::idx() { return m_prefix == 1 ? m_ix : m_iy; }

uint16_t Z80::dispAddr() {
    const int8_t d = (int8_t)fetch();
    idle((uint16_t)(m_pc - 1), 5);
    m_dispAddr = (uint16_t)(idx() + d);
    m_wz = m_dispAddr;
    m_dispFetched = true;
    return m_dispAddr;
}

uint8_t Z80::reg8(int r) {
    switch (r) {
        case 0: return m_b;
        case 1: return m_c;
        case 2: return m_d;
        case 3: return m_e;
        case 4: return m_prefix ? (uint8_t)(idx() >> 8) : m_h;
        case 5: return m_prefix ? (uint8_t)idx() : m_l;
        case 6: {
            if (!m_prefix) return rd(hlReg());
            if (!m_dispFetched) dispAddr();
            return rd(m_dispAddr);
        }
        default: return m_a;
    }
}

void Z80::setReg8(int r, uint8_t v) {
    switch (r) {
        case 0: m_b = v; break;
        case 1: m_c = v; break;
        case 2: m_d = v; break;
        case 3: m_e = v; break;
        case 4: if (m_prefix) idx() = (uint16_t)((idx() & 0x00FF) | v << 8); else m_h = v; break;
        case 5: if (m_prefix) idx() = (uint16_t)((idx() & 0xFF00) | v); else m_l = v; break;
        case 6:
            if (!m_prefix) { wr(hlReg(), v); break; }
            if (!m_dispFetched) dispAddr();
            wr(m_dispAddr, v);
            break;
        default: m_a = v; break;
    }
}

uint16_t Z80::rp(int p) const {
    switch (p) {
        case 0: return bc();
        case 1: return de();
        case 2: return m_prefix == 1 ? m_ix : m_prefix == 2 ? m_iy : hl();
        default: return m_sp;
    }
}

void Z80::setRp(int p, uint16_t v) {
    switch (p) {
        case 0: m_b = v >> 8; m_c = v & 0xFF; break;
        case 1: m_d = v >> 8; m_e = v & 0xFF; break;
        case 2: if (m_prefix == 1) m_ix = v; else if (m_prefix == 2) m_iy = v; else setHl(v); break;
        default: m_sp = v; break;
    }
}

uint16_t Z80::rp2(int p) const { return p == 3 ? (uint16_t)(m_a << 8 | m_f) : rp(p); }
void Z80::setRp2(int p, uint16_t v) {
    if (p == 3) { m_a = v >> 8; m_f = v & 0xFF; } else setRp(p, v);
}

bool Z80::cond(int y) const {
    switch (y) {
        case 0: return !(m_f & FZ);
        case 1: return m_f & FZ;
        case 2: return !(m_f & FC);
        case 3: return m_f & FC;
        case 4: return !(m_f & FP);
        case 5: return m_f & FP;
        case 6: return !(m_f & FS);
        default: return m_f & FS;
    }
}

// ---------------------------------------------------------------------------
// ALU
// ---------------------------------------------------------------------------

void Z80::alu(int op, uint8_t v) {
    const uint8_t a = m_a;
    switch (op) {
        case 0: case 1: {                                   // ADD, ADC
            int c = (op == 1 && (m_f & FC)) ? 1 : 0;
            int r = a + v + c;
            uint8_t res = (uint8_t)r;
            uint8_t f = sz53(res) | (r > 0xFF ? FC : 0) | ((a ^ v ^ res) & FH);
            if (((a ^ ~v) & (a ^ res)) & 0x80) f |= FP;
            m_a = res; setF(f);
            break;
        }
        case 2: case 3: case 7: {                           // SUB, SBC, CP
            int c = (op == 3 && (m_f & FC)) ? 1 : 0;
            int r = a - v - c;
            uint8_t res = (uint8_t)r;
            uint8_t f = (uint8_t)((res & FS) | (res ? 0 : FZ) | FN | (r < 0 ? FC : 0) | ((a ^ v ^ res) & FH));
            if (((a ^ v) & (a ^ res)) & 0x80) f |= FP;
            if (op == 7) f = (uint8_t)((f & ~(FX | FY)) | (v & (FX | FY)));   // CP: X/Y from the operand
            else { f |= res & (FX | FY); m_a = res; }
            setF(f);
            break;
        }
        case 4: m_a &= v; setF(sz53p(m_a) | FH); break;     // AND
        case 5: m_a ^= v; setF(sz53p(m_a)); break;          // XOR
        case 6: m_a |= v; setF(sz53p(m_a)); break;          // OR
    }
}

uint8_t Z80::inc8(uint8_t v) {
    uint8_t r = (uint8_t)(v + 1);
    setF((uint8_t)((m_f & FC) | sz53(r) | ((v ^ r) & FH) | (v == 0x7F ? FP : 0)));
    return r;
}

uint8_t Z80::dec8(uint8_t v) {
    uint8_t r = (uint8_t)(v - 1);
    setF((uint8_t)((m_f & FC) | sz53(r) | FN | ((v ^ r) & FH) | (v == 0x80 ? FP : 0)));
    return r;
}

uint8_t Z80::rot(int y, uint8_t v) {
    uint8_t r, c;
    switch (y) {
        case 0: c = v >> 7; r = (uint8_t)(v << 1 | c); break;                       // RLC
        case 1: c = v & 1; r = (uint8_t)(v >> 1 | c << 7); break;                   // RRC
        case 2: c = v >> 7; r = (uint8_t)(v << 1 | (m_f & FC)); break;              // RL
        case 3: c = v & 1; r = (uint8_t)(v >> 1 | (m_f & FC) << 7); break;          // RR
        case 4: c = v >> 7; r = (uint8_t)(v << 1); break;                           // SLA
        case 5: c = v & 1; r = (uint8_t)((v >> 1) | (v & 0x80)); break;             // SRA
        case 6: c = v >> 7; r = (uint8_t)(v << 1 | 1); break;                       // SLL (undocumented)
        default: c = v & 1; r = (uint8_t)(v >> 1); break;                           // SRL
    }
    setF(sz53p(r) | c);
    return r;
}

void Z80::add16(uint16_t& r, uint16_t v) {
    m_wz = (uint16_t)(r + 1);
    uint32_t s = (uint32_t)r + v;
    uint8_t f = (uint8_t)((m_f & (FS | FZ | FP)) | (s > 0xFFFF ? FC : 0) |
                          (((r ^ v ^ s) >> 8) & FH) | ((s >> 8) & (FX | FY)));
    r = (uint16_t)s;
    setF(f);
}

void Z80::adc16(uint16_t v) {
    uint16_t h = hlReg();
    m_wz = (uint16_t)(h + 1);
    uint32_t s = (uint32_t)h + v + (m_f & FC);
    uint16_t res = (uint16_t)s;
    uint8_t f = (uint8_t)(((res >> 8) & (FS | FX | FY)) | (res ? 0 : FZ) | (s > 0xFFFF ? FC : 0) |
                          (((h ^ v ^ s) >> 8) & FH));
    if (((h ^ ~v) & (h ^ res)) & 0x8000) f |= FP;
    setHl(res);
    setF(f);
}

void Z80::sbc16(uint16_t v) {
    uint16_t h = hlReg();
    m_wz = (uint16_t)(h + 1);
    int32_t s = (int32_t)h - v - (m_f & FC);
    uint16_t res = (uint16_t)s;
    uint8_t f = (uint8_t)(((res >> 8) & (FS | FX | FY)) | (res ? 0 : FZ) | (s < 0 ? FC : 0) | FN |
                          (((h ^ v ^ res) >> 8) & FH));
    if (((h ^ v) & (h ^ res)) & 0x8000) f |= FP;
    setHl(res);
    setF(f);
}

// BIT n: Z and P from the bit, S for bit 7 set, X/Y from `xy` (the register,
// or MEMPTR's high byte for (HL) / (IX+d))
void Z80::bit(int n, uint8_t v, uint8_t xy) {
    uint8_t b = v & (1 << n);
    uint8_t f = (uint8_t)((m_f & FC) | FH | (xy & (FX | FY)));
    if (!b) f |= FZ | FP;
    if (n == 7 && b) f |= FS;
    setF(f);
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

int Z80::interrupt() {
    if (m_halted) { m_halted = false; m_pc++; }             // past the HALT
    if (m_nmiPending) {
        m_nmiPending = false;
        m_iff1 = false;
        incR();
        m_cycles += 5;
        push(m_pc);
        m_pc = 0x0066;
        m_wz = m_pc;
        return 11;
    }
    m_iff1 = m_iff2 = false;
    incR();
    m_cycles += 7;                                           // the acknowledge M1
    push(m_pc);
    if (m_im == 2) m_pc = rd16((uint16_t)(m_i << 8 | m_intVector));   // the data bus's byte
    else m_pc = 0x0038;                                      // IM 1, and IM 0's RST 38h
    m_wz = m_pc;
    return m_im == 2 ? 19 : 13;
}

int Z80::step() {
    if (m_pcHook && m_pcHook->armed(m_pc) && m_pcHook->fire(this, m_pc)) {
        m_cycles += 4;
        return 4;
    }
    const uint64_t start = m_cycles;
    // Interrupts are taken between instructions (not right after EI)
    if (m_nmiPending || (m_irqLine && m_iff1 && !m_eiDelay)) {
        interrupt();
        m_q = m_qNext = 0;
        return (int)(m_cycles - start);
    }
    m_eiDelay = false;
    if (m_halted) {                                          // HALT fetches itself again
        m1();
        m_pc--;
        incR();
        return (int)(m_cycles - start);
    }

    if (m_observer) {
        if (m_observer->needsDisasm()) {
            DisasmEntry entry;
            disassembleEntry(m_bus, m_pc, &entry);
            if (!m_observer->onStep(this, m_bus, entry)) return 1;
        } else {
            m_observer->onStepLite(this, m_pc);
        }
    }

    // The exec functions return the documented totals; the clock, advanced
    // per bus cycle, is what counts
    m_q = m_qNext;
    m_qNext = 0;
    m_prefix = 0;
    m_dispFetched = false;
    uint8_t op = m1();
    incR();
    while (op == 0xDD || op == 0xFD) {                       // the last prefix wins
        m_prefix = op == 0xDD ? 1 : 2;
        op = m1();
        incR();
    }
    if (op == 0xCB) { if (m_prefix) execIndexCB(); else execCB(); }
    else if (op == 0xED) { m_prefix = 0; execED(); }
    else execMain(op);
    m_prefix = 0;
    return (int)(m_cycles - start);
}

int Z80::execMain(uint8_t op) {
    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    const int ext = m_prefix ? 8 : 0;                        // (IX+d): fetch d and add
    switch (x) {
    case 0:
        switch (z) {
        case 0:
            switch (y) {
            case 0: return 4;                                // NOP
            case 1: {                                        // EX AF,AF'
                uint16_t t = (uint16_t)(m_a << 8 | m_f);
                m_a = m_af2 >> 8; m_f = m_af2 & 0xFF; m_af2 = t;
                return 4;
            }
            case 2:                                          // DJNZ d
                idle(ir(), 1);
                if (--m_b) { jr(); return 13; }
                mcycle(m_pc++);
                return 8;
            case 3: jr(); return 12;
            default:                                         // JR cc,d
                if (cond(y - 4)) { jr(); return 12; }
                mcycle(m_pc++);
                return 7;
            }
        case 1:
            if (!q) { setRp(p, fetch16()); return 10; }      // LD rp,nn
            idle(ir(), 7);
            if (m_prefix == 1) add16(m_ix, rp(p));           // ADD HL/IX/IY,rp
            else if (m_prefix == 2) add16(m_iy, rp(p));
            else { uint16_t h = hlReg(); add16(h, rp(p)); setHl(h); }
            return 11;
        case 2:
            switch (y) {
            case 0: wr(bc(), m_a); m_wz = (uint16_t)(m_a << 8 | ((bc() + 1) & 0xFF)); return 7;
            case 1: m_a = rd(bc()); m_wz = (uint16_t)(bc() + 1); return 7;
            case 2: wr(de(), m_a); m_wz = (uint16_t)(m_a << 8 | ((de() + 1) & 0xFF)); return 7;
            case 3: m_a = rd(de()); m_wz = (uint16_t)(de() + 1); return 7;
            case 4: { uint16_t a = fetch16(); wr16(a, rp(2)); m_wz = (uint16_t)(a + 1); return 16; }
            case 5: { uint16_t a = fetch16(); setRp(2, rd16(a)); m_wz = (uint16_t)(a + 1); return 16; }
            case 6: { uint16_t a = fetch16(); wr(a, m_a); m_wz = (uint16_t)(m_a << 8 | ((a + 1) & 0xFF)); return 13; }
            default: { uint16_t a = fetch16(); m_a = rd(a); m_wz = (uint16_t)(a + 1); return 13; }
            }
        case 3:                                              // INC/DEC rp
            idle(ir(), 2);
            setRp(p, (uint16_t)(rp(p) + (q ? -1 : 1)));
            return 6;
        case 4: case 5: {                                    // INC/DEC r
            uint8_t v = reg8(y);
            if (y == 6) idle(m_prefix ? m_dispAddr : hlReg(), 1);
            setReg8(y, z == 4 ? inc8(v) : dec8(v));
            return y == 6 ? 11 + ext : 4;
        }
        case 6: {                                            // LD r,n
            if (y == 6 && m_prefix) {                        // d comes before n
                m_dispAddr = (uint16_t)(idx() + (int8_t)fetch());
                m_wz = m_dispAddr;
                m_dispFetched = true;
                uint8_t n = fetch();
                idle((uint16_t)(m_pc - 1), 2);
                wr(m_dispAddr, n);
                return 15;
            }
            setReg8(y, fetch());
            return y == 6 ? 10 : 7;
        }
        default:
            switch (y) {
            case 0: {                                        // RLCA
                uint8_t c = m_a >> 7;
                m_a = (uint8_t)(m_a << 1 | c);
                setF((uint8_t)((m_f & (FS | FZ | FP)) | (m_a & (FX | FY)) | c));
                return 4;
            }
            case 1: {                                        // RRCA
                uint8_t c = m_a & 1;
                m_a = (uint8_t)(m_a >> 1 | c << 7);
                setF((uint8_t)((m_f & (FS | FZ | FP)) | (m_a & (FX | FY)) | c));
                return 4;
            }
            case 2: {                                        // RLA
                uint8_t c = m_a >> 7;
                m_a = (uint8_t)(m_a << 1 | (m_f & FC));
                setF((uint8_t)((m_f & (FS | FZ | FP)) | (m_a & (FX | FY)) | c));
                return 4;
            }
            case 3: {                                        // RRA
                uint8_t c = m_a & 1;
                m_a = (uint8_t)(m_a >> 1 | (m_f & FC) << 7);
                setF((uint8_t)((m_f & (FS | FZ | FP)) | (m_a & (FX | FY)) | c));
                return 4;
            }
            case 4: {                                        // DAA
                uint8_t a = m_a, corr = 0, c = m_f & FC;
                if ((m_f & FH) || (a & 0x0F) > 9) corr |= 0x06;
                if (c || a > 0x99) { corr |= 0x60; c = FC; }
                uint8_t r = (m_f & FN) ? (uint8_t)(a - corr) : (uint8_t)(a + corr);
                uint8_t h = (uint8_t)((a ^ r) & FH);
                m_a = r;
                setF((uint8_t)(sz53p(r) | (m_f & FN) | h | c));
                return 4;
            }
            case 5:                                          // CPL
                m_a = (uint8_t)~m_a;
                setF((uint8_t)((m_f & (FS | FZ | FP | FC)) | FH | FN | (m_a & (FX | FY))));
                return 4;
            case 6:                                          // SCF
                setF((uint8_t)((m_f & (FS | FZ | FP)) | FC | (((m_q ^ m_f) | m_a) & (FX | FY))));
                return 4;
            default: {                                       // CCF
                uint8_t c = m_f & FC;
                setF((uint8_t)((m_f & (FS | FZ | FP)) | (c ? FH : FC) | (((m_q ^ m_f) | m_a) & (FX | FY))));
                return 4;
            }
            }
        }
    case 1:
        if (y == 6 && z == 6) { m_halted = true; m_pc--; return 4; }   // HALT: PC stays on it
        if (y == 6 || z == 6) {                              // LD (IX+d),r / LD r,(IX+d): plain H, L
            int save = m_prefix;
            if (y == 6) {
                uint16_t addr;
                addr = save ? dispAddr() : hlReg();
                m_prefix = 0;
                uint8_t v = reg8(z);
                m_prefix = save;
                wr(addr, v);
            } else {
                uint8_t v = reg8(6);
                m_prefix = 0;
                setReg8(y, v);
                m_prefix = save;
            }
            return 7 + ext;
        }
        setReg8(y, reg8(z));                                 // LD r,r' (IXH/IXL with a prefix)
        return 4;
    case 2:
        alu(y, reg8(z));
        return z == 6 ? 7 + ext : 4;
    default:
        switch (z) {
        case 0:                                              // RET cc
            idle(ir(), 1);
            if (cond(y)) { m_pc = pop(); m_wz = m_pc; return 11; }
            return 5;
        case 1:
            if (!q) { setRp2(p, pop()); return 10; }        // POP rp2
            switch (p) {
            case 0: m_pc = pop(); m_wz = m_pc; return 10;    // RET
            case 1: {                                        // EXX
                uint16_t t;
                t = bc(); m_b = m_bc2 >> 8; m_c = m_bc2 & 0xFF; m_bc2 = t;
                t = de(); m_d = m_de2 >> 8; m_e = m_de2 & 0xFF; m_de2 = t;
                t = hl(); m_h = m_hl2 >> 8; m_l = m_hl2 & 0xFF; m_hl2 = t;
                return 4;
            }
            case 2: m_pc = rp(2); return 4;                  // JP (HL)
            default: idle(ir(), 2); m_sp = rp(2); return 6;  // LD SP,HL
            }
        case 2: {                                            // JP cc,nn
            uint16_t a = fetch16();
            m_wz = a;
            if (cond(y)) m_pc = a;
            return 10;
        }
        case 3:
            switch (y) {
            case 0: m_pc = fetch16(); m_wz = m_pc; return 10;
            case 2: { uint8_t n = fetch(); out((uint16_t)(m_a << 8 | n), m_a);
                      m_wz = (uint16_t)(m_a << 8 | ((n + 1) & 0xFF)); return 11; }
            case 3: { uint8_t n = fetch(); uint16_t port = (uint16_t)(m_a << 8 | n);
                      m_a = in(port); m_wz = (uint16_t)(port + 1); return 11; }
            case 4: {                                        // EX (SP),HL
                const uint16_t sp1 = (uint16_t)(m_sp + 1), r = rp(2);
                uint8_t lo = rd(m_sp), hi = rd(sp1);
                idle(sp1, 1);
                wr(sp1, r >> 8);
                wr(m_sp, r & 0xFF);
                idle(m_sp, 2);
                uint16_t v = (uint16_t)(hi << 8 | lo);
                setRp(2, v);
                m_wz = v;
                return 19;
            }
            case 5: {                                        // EX DE,HL (never IX)
                uint16_t t = de(); m_d = m_h; m_e = m_l; setHl(t);
                return 4;
            }
            case 6: m_iff1 = m_iff2 = false; return 4;       // DI
            case 7: m_iff1 = m_iff2 = true; m_eiDelay = true; return 4;   // EI
            default: return 4;                               // CB: handled in step()
            }
        case 4:                                              // CALL cc,nn
            if (cond(y)) { call(); return 17; }
            m_wz = fetch16();
            return 10;
        case 5:
            if (!q) { idle(ir(), 1); push(rp2(p)); return 11; }   // PUSH rp2
            if (p == 0) { call(); return 17; }               // CALL
            return 4;                                        // DD/ED/FD: handled in step()
        case 6: alu(y, fetch()); return 7;
        default: idle(ir(), 1); push(m_pc); m_pc = (uint16_t)(y * 8); m_wz = m_pc; return 11;   // RST
        }
    }
}

int Z80::execCB() {
    uint8_t op = m1();
    incR();
    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    uint8_t v = reg8(z);
    if (z == 6) idle(hlReg(), 1);
    switch (x) {
        case 0: setReg8(z, rot(y, v)); return z == 6 ? 15 : 8;
        case 1: bit(y, v, z == 6 ? (uint8_t)(m_wz >> 8) : v); return z == 6 ? 12 : 8;
        case 2: setReg8(z, (uint8_t)(v & ~(1 << y))); return z == 6 ? 15 : 8;
        default: setReg8(z, (uint8_t)(v | (1 << y))); return z == 6 ? 15 : 8;
    }
}

// DDCB d op / FDCB d op: the operand is (IX+d); the result is also copied
// to the register z names (undocumented) unless z = 6
int Z80::execIndexCB() {
    uint16_t addr = (uint16_t)(idx() + (int8_t)fetch());
    m_wz = addr;
    uint8_t op = fetch();                                    // not an M1: R is not incremented
    idle((uint16_t)(m_pc - 1), 2);
    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    uint8_t v = rd(addr), r;
    idle(addr, 1);
    switch (x) {
        case 0: r = rot(y, v); break;
        case 1: bit(y, v, (uint8_t)(addr >> 8)); return 16;  // + 4 for the prefix = 20
        case 2: r = (uint8_t)(v & ~(1 << y)); break;
        default: r = (uint8_t)(v | (1 << y)); break;
    }
    wr(addr, r);
    if (z != 6) { m_prefix = 0; setReg8(z, r); }
    return 19;                                               // 23 with the prefix
}

int Z80::execED() {
    uint8_t op = m1();
    incR();
    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    if (x == 1) {
        switch (z) {
        case 0: {                                            // IN r,(C) / IN (C)
            uint8_t v = in(bc());
            m_wz = (uint16_t)(bc() + 1);
            if (y != 6) setReg8(y, v);
            setF((uint8_t)((m_f & FC) | sz53p(v)));
            return 12;
        }
        case 1:                                              // OUT (C),r / OUT (C),0
            out(bc(), y == 6 ? 0 : reg8(y));
            m_wz = (uint16_t)(bc() + 1);
            return 12;
        case 2: idle(ir(), 7); if (q) adc16(rp(p)); else sbc16(rp(p)); return 15;
        case 3: {
            uint16_t a = fetch16();
            if (q) setRp(p, rd16(a)); else wr16(a, rp(p));
            m_wz = (uint16_t)(a + 1);
            return 20;
        }
        case 4: { uint8_t v = m_a; m_a = 0; alu(2, v); return 8; }          // NEG
        case 5: m_iff1 = m_iff2; m_pc = pop(); m_wz = m_pc; return 14;      // RETN / RETI
        case 6: { static const uint8_t IM[8] = {0, 0, 1, 2, 0, 0, 1, 2}; m_im = IM[y]; return 8; }
        default:
            switch (y) {
            case 0: idle(ir(), 1); m_i = m_a; return 9;      // LD I,A
            case 1: idle(ir(), 1); m_r = m_a; return 9;      // LD R,A
            case 2: idle(ir(), 1); m_a = m_i; setF((uint8_t)((m_f & FC) | sz53(m_a) | (m_iff2 ? FP : 0))); return 9;
            case 3: idle(ir(), 1); m_a = m_r; setF((uint8_t)((m_f & FC) | sz53(m_a) | (m_iff2 ? FP : 0))); return 9;
            case 4: {                                        // RRD
                uint16_t h = hlReg();
                uint8_t v = rd(h);
                idle(h, 4);
                wr(h, (uint8_t)((m_a << 4) | (v >> 4)));
                m_a = (uint8_t)((m_a & 0xF0) | (v & 0x0F));
                setF((uint8_t)((m_f & FC) | sz53p(m_a)));
                m_wz = (uint16_t)(h + 1);
                return 18;
            }
            case 5: {                                        // RLD
                uint16_t h = hlReg();
                uint8_t v = rd(h);
                idle(h, 4);
                wr(h, (uint8_t)((v << 4) | (m_a & 0x0F)));
                m_a = (uint8_t)((m_a & 0xF0) | (v >> 4));
                setF((uint8_t)((m_f & FC) | sz53p(m_a)));
                m_wz = (uint16_t)(h + 1);
                return 18;
            }
            default: return 8;                               // NOP
            }
        }
    }
    if (x == 2 && y >= 4 && z <= 3) {                        // block instructions
        const bool dec = y & 1, rep = y >= 6;
        const int dir = dec ? -1 : 1;
        uint16_t h = hlReg();
        uint16_t b16 = bc();
        switch (z) {
        case 0: {                                            // LDI LDD LDIR LDDR
            uint8_t v = rd(h);
            const uint16_t d0 = de();
            wr(d0, v);
            idle(d0, 2);
            setHl((uint16_t)(h + dir));
            uint16_t d = (uint16_t)(de() + dir); m_d = d >> 8; m_e = d & 0xFF;
            --b16; m_b = b16 >> 8; m_c = b16 & 0xFF;
            uint8_t n = (uint8_t)(v + m_a);
            setF((uint8_t)((m_f & (FS | FZ | FC)) | (b16 ? FP : 0) | (n & FX) | ((n << 4) & FY)));
            if (rep && b16) { idle(d0, 5); m_pc = (uint16_t)(m_pc - 2); m_wz = (uint16_t)(m_pc + 1); return 21; }
            return 16;
        }
        case 1: {                                            // CPI CPD CPIR CPDR
            uint8_t v = rd(h);
            idle(h, 5);
            uint8_t r = (uint8_t)(m_a - v);
            setHl((uint16_t)(h + dir));
            --b16; m_b = b16 >> 8; m_c = b16 & 0xFF;
            uint8_t hf = (uint8_t)((m_a ^ v ^ r) & FH);
            uint8_t n = (uint8_t)(r - (hf ? 1 : 0));
            setF((uint8_t)((m_f & FC) | (r & FS) | (r ? 0 : FZ) | hf | FN | (b16 ? FP : 0) |
                           (n & FX) | ((n << 4) & FY)));
            m_wz = (uint16_t)(m_wz + dir);
            if (rep && b16 && r) { idle(h, 5); m_pc = (uint16_t)(m_pc - 2); m_wz = (uint16_t)(m_pc + 1); return 21; }
            return 16;
        }
        case 2: {                                            // INI IND INIR INDR
            idle(ir(), 1);
            uint8_t v = in(b16);
            m_wz = (uint16_t)(b16 + dir);
            wr(h, v);
            setHl((uint16_t)(h + dir));
            m_b = (uint8_t)(m_b - 1);
            uint16_t k = (uint16_t)(v + (uint8_t)(m_c + dir));
            setF((uint8_t)(sz53(m_b) | ((v & 0x80) ? FN : 0) | (k > 0xFF ? (FH | FC) : 0) |
                           parity((uint8_t)((k & 7) ^ m_b))));
            if (rep && m_b) { idle(h, 5); m_pc = (uint16_t)(m_pc - 2); return 21; }
            return 16;
        }
        default: {                                           // OUTI OUTD OTIR OTDR
            idle(ir(), 1);
            uint8_t v = rd(h);
            m_b = (uint8_t)(m_b - 1);
            out(bc(), v);
            m_wz = (uint16_t)(bc() + dir);
            setHl((uint16_t)(h + dir));
            uint16_t k = (uint16_t)(v + m_l);
            setF((uint8_t)(sz53(m_b) | ((v & 0x80) ? FN : 0) | (k > 0xFF ? (FH | FC) : 0) |
                           parity((uint8_t)((k & 7) ^ m_b))));
            if (rep && m_b) { idle(bc(), 5); m_pc = (uint16_t)(m_pc - 2); return 21; }
            return 16;
        }
        }
    }
    return 8;                                                // undefined: NOP
}

// ---------------------------------------------------------------------------
// Debugger support
// ---------------------------------------------------------------------------

int Z80::disassembleEntry(IBus* bus, uint32_t addr, void* entryOut) {
    auto* e = static_cast<DisasmEntry*>(entryOut);
    char buf[64];
    e->addr = addr;
    e->bytes = disassembleOne(bus, addr, buf, sizeof buf);
    std::string s(buf);
    auto sp = s.find(' ');
    e->mnemonic = sp == std::string::npos ? s : s.substr(0, sp);
    e->operands = sp == std::string::npos ? "" : s.substr(sp + 1);
    e->complete = s;
    e->isCall = isCallAt(bus, addr) > 0;
    e->isReturn = isReturnAt(bus, addr);
    e->isBranch = e->mnemonic == "JP" || e->mnemonic == "JR" || e->mnemonic == "DJNZ";
    e->isIllegal = false;
    e->targetAddr = 0;
    e->rawLen = (uint8_t)std::min(e->bytes, (int)DisasmEntry::MAX_RAW);
    for (int i = 0; i < e->rawLen; ++i) e->raw[i] = bus ? bus->peek8((addr + i) & 0xFFFF) : 0;
    return e->bytes;
}

int Z80::isCallAt(IBus* bus, uint32_t addr) {
    if (!bus) return 0;
    uint8_t op = bus->peek8(addr & 0xFFFF);
    if (op == 0xCD || (op & 0xC7) == 0xC4) return 3;         // CALL, CALL cc
    if ((op & 0xC7) == 0xC7) return 1;                       // RST
    return 0;
}

bool Z80::isReturnAt(IBus* bus, uint32_t addr) {
    if (!bus) return false;
    uint8_t op = bus->peek8(addr & 0xFFFF);
    if (op == 0xC9 || (op & 0xC7) == 0xC0) return true;
    if (op == 0xED) {
        uint8_t o2 = bus->peek8((addr + 1) & 0xFFFF);
        return (o2 & 0xC7) == 0x45;                          // RETN / RETI
    }
    return false;
}

bool Z80::isProgramEnd(IBus* bus) { (void)bus; return m_halted && !m_iff1; }

int Z80::writeCallHarness(IBus* bus, uint32_t scratch, uint32_t routineAddr) {
    // CALL routine ; HALT
    bus->write8(scratch, 0xCD);
    bus->write8(scratch + 1, routineAddr & 0xFF);
    bus->write8(scratch + 2, (routineAddr >> 8) & 0xFF);
    bus->write8(scratch + 3, 0x76);
    return 4;
}
