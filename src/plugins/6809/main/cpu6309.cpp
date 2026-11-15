// Hitachi HD6309 core (#332): the 6809's superset, counted bus cycle by bus
// cycle in emulation and native mode.
//
// Sources: Darren Atkinson, "Motorola 6809 and Hitachi 6309 Programmers
// Reference" (2009); David Banks, "Undocumented 6309 Behaviours"; XRoar's
// HD6309 (Ciaran Anscomb) as the cycle-sequence reference; MAME's hd6309 as a
// second opinion. Where they disagree, see the notes at each instruction.

#include "cpu6309.h"
#include "libdebug/main/execution_observer.h"
#include "libcore/main/intercept.h"
#include "libtoolchain/main/idisasm.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
const RegDescriptor REGS[HD6309::R6309_COUNT] = {
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
    {"E", RegWidth::R8, 0, nullptr, nullptr},
    {"F", RegWidth::R8, 0, nullptr, nullptr},
    {"W", RegWidth::R16, 0, nullptr, nullptr},
    {"Q", RegWidth::R32, 0, nullptr, nullptr},
    {"V", RegWidth::R16, 0, nullptr, nullptr},
    {"MD", RegWidth::R8, 0, nullptr, nullptr},
};
inline uint16_t sext8(uint8_t v) { return (uint16_t)(int16_t)(int8_t)v; }

// The branch condition of a Bcc / LBcc by its low nibble
bool branchTaken(uint8_t cc, int lo) {
    const bool n = cc & M6809::CC_N, v = cc & M6809::CC_V, z = cc & M6809::CC_Z, c = cc & M6809::CC_C;
    switch (lo & 15) {
        case 0x0: return true;           case 0x1: return false;
        case 0x2: return !(c || z);      case 0x3: return c || z;
        case 0x4: return !c;             case 0x5: return c;
        case 0x6: return !z;             case 0x7: return z;
        case 0x8: return !v;             case 0x9: return v;
        case 0xA: return !n;             case 0xB: return n;
        case 0xC: return n == v;         case 0xD: return n != v;
        case 0xE: return !z && n == v;   default: return z || n != v;
    }
}

// The seven postbytes the 6309 leaves undefined: [,-R] ($92 $B2 $D2 $F2)
// and [n16] with R bits other than 00 ($BF $DF $FF). Every other postbyte
// means something; $90 $B0 $D0 $F0 ([,R+] on the 6809) are the [W] modes.
// From hoglet67's 6809Decoder (postbyte_cycles_6309_*: "on the 6309 there
// are 7"), measured on real parts.
bool illegalIndexPostbyte(uint8_t pb) {
    return (pb & 0x9F) == 0x92 || pb == 0xBF || pb == 0xDF || pb == 0xFF;
}
}  // namespace

HD6309::HD6309() { reset(); }

void HD6309::reset() {
    M6809::reset();
    m_md = 0;
    m_tfmActive = false;
}

// ---------------------------------------------------------------------------
// Registers and state
// ---------------------------------------------------------------------------
const RegDescriptor* HD6309::regDescriptor(int idx) const {
    return idx >= 0 && idx < R6309_COUNT ? &REGS[idx] : nullptr;
}

uint32_t HD6309::regRead(int idx) const {
    switch (idx) {
        case R_E: return m_e;
        case R_F: return m_f;
        case R_W: return w();
        case R_Q: return q();
        case R_V: return m_v;
        case R_MD: return m_md;
    }
    return M6809::regRead(idx);
}

void HD6309::regWrite(int idx, uint32_t v) {
    switch (idx) {
        case R_E: m_e = (uint8_t)v; return;
        case R_F: m_f = (uint8_t)v; return;
        case R_W: setW((uint16_t)v); return;
        case R_Q: setD((uint16_t)(v >> 16)); setW((uint16_t)v); return;
        case R_V: m_v = (uint16_t)v; return;
        case R_MD: m_md = (uint8_t)v; return;
    }
    M6809::regWrite(idx, v);
}

void HD6309::saveState(uint8_t* b) const {
    std::memset(b, 0, stateSize());
    M6809::saveState(b);
    b[16] = m_e; b[17] = m_f;
    b[18] = (uint8_t)(m_v >> 8); b[19] = (uint8_t)m_v;
    b[20] = m_md; b[21] = m_m;
    b[22] = m_tfmActive ? 1 : 0; b[23] = m_tfmOp; b[24] = m_tfmPost;
}

void HD6309::loadState(const uint8_t* b) {
    M6809::loadState(b);
    m_e = b[16]; m_f = b[17];
    m_v = (uint16_t)(b[18] << 8 | b[19]);
    m_md = b[20]; m_m = b[21];
    m_tfmActive = b[22] & 1; m_tfmOp = b[23]; m_tfmPost = b[24];
}

// TFR/EXG: an 8-bit register read as 16 bits appears in both halves; a 16-bit
// value written to A, DP or E gives its high byte, to B, CC or F its low
// byte (Atkinson, TFR 6309 implementation). Codes $C and $D are the zero
// register.
uint16_t HD6309::tfrRead(int code) const {
    switch (code & 15) {
        case 0x0: return d();
        case 0x1: return m_x;
        case 0x2: return m_y;
        case 0x3: return m_u;
        case 0x4: return m_s;
        case 0x5: return m_pc;
        case 0x6: return w();
        case 0x7: return m_v;
        case 0x8: return (uint16_t)(m_a << 8 | m_a);
        case 0x9: return (uint16_t)(m_b << 8 | m_b);
        case 0xA: return (uint16_t)(m_cc << 8 | m_cc);
        case 0xB: return (uint16_t)(m_dp << 8 | m_dp);
        case 0xE: return (uint16_t)(m_e << 8 | m_e);
        case 0xF: return (uint16_t)(m_f << 8 | m_f);
    }
    return 0;
}

void HD6309::tfrWrite(int code, uint16_t v) {
    switch (code & 15) {
        case 0x0: setD(v); break;
        case 0x1: m_x = v; break;
        case 0x2: m_y = v; break;
        case 0x3: m_u = v; break;
        case 0x4: m_s = v; m_nmiArmed = true; break;
        case 0x5: m_pc = v; break;
        case 0x6: setW(v); break;
        case 0x7: m_v = v; break;
        case 0x8: m_a = (uint8_t)(v >> 8); break;
        case 0x9: m_b = (uint8_t)v; break;
        case 0xA: m_cc = (uint8_t)v; break;
        case 0xB: m_dp = (uint8_t)(v >> 8); break;
        case 0xE: m_e = (uint8_t)(v >> 8); break;
        case 0xF: m_f = (uint8_t)v; break;
    }
}

uint16_t HD6309::reg16(int code) const {
    switch (code) {
        case 0: return d();
        case 1: return m_x;
        case 2: return m_y;
        case 3: return m_u;
        default: return m_s;
    }
}

void HD6309::setReg16(int code, uint16_t v) {
    switch (code) {
        case 0: setD(v); break;
        case 1: m_x = v; break;
        case 2: m_y = v; break;
        case 3: m_u = v; break;
        default: m_s = v; m_nmiArmed = true; break;
    }
}

// ---------------------------------------------------------------------------
// Addressing. Dead cycles the 6309 drops in native mode go through emu().
// ---------------------------------------------------------------------------
uint16_t HD6309::eaDirect() {
    const uint16_t a = (uint16_t)(m_dp << 8 | imm8());
    emu();
    return a;
}

uint16_t HD6309::eaExtended() {
    const uint16_t a = imm16();
    emu();
    return a;
}

// The indexed modes, with Atkinson's indexed-mode table of extra cycles
// (6809 / native): the 6809's, plus ,W ,W++ ,--W n16,W (postbytes $8F $CF
// $EF $AF and their indirect forms $90 $D0 $F0 $B0), and E,R F,R W,R.
uint16_t HD6309::eaIndexed() {
    const uint8_t pb = imm8();
    if (illegalIndexPostbyte(pb)) illegalPostbyte();
    uint16_t& r = indexReg(pb >> 5);
    if (!(pb & 0x80)) {                                                       // 5-bit offset
        idle(2);
        return (uint16_t)(r + ((pb & 0x10) ? (int)(pb & 0x1F) - 32 : (pb & 0x1F)));
    }
    uint16_t ea;
    switch (pb) {
        case 0x8F: case 0x90: ea = w(); idle(); break;                        // ,W   [,W]
        case 0xAF: case 0xB0: ea = (uint16_t)(imm16() + w()); idle(); break;  // n,W  [n,W]
        case 0xCF: case 0xD0: ea = w(); setW((uint16_t)(w() + 2)); idle(2); break;   // ,W++ [,W++]
        case 0xEF: case 0xF0: setW((uint16_t)(w() - 2)); ea = w(); idle(2); break;   // ,--W [,--W]
        default:
            switch (pb & 0x0F) {
                case 0x0: ea = r; r = (uint16_t)(r + 1); idle(2); emu(); break;            // ,R+
                case 0x1: ea = r; r = (uint16_t)(r + 2); idle(3); emu(); break;            // ,R++
                case 0x2: r = (uint16_t)(r - 1); ea = r; idle(2); emu(); break;            // ,-R
                case 0x3: r = (uint16_t)(r - 2); ea = r; idle(3); emu(); break;            // ,--R
                case 0x4: ea = r; idle(); break;                                          // ,R
                case 0x5: ea = (uint16_t)(r + sext8(m_b)); idle(2); break;                // B,R
                case 0x6: ea = (uint16_t)(r + sext8(m_a)); idle(2); break;                // A,R
                case 0x7: ea = (uint16_t)(r + sext8(m_e)); idle(2); break;                // E,R
                case 0x8: ea = (uint16_t)(r + sext8(imm8())); idle(); break;              // n8,R
                case 0x9: ea = (uint16_t)(r + imm16()); idle(2); emu(); break;            // n16,R
                case 0xA: ea = (uint16_t)(r + sext8(m_f)); idle(2); break;                // F,R
                case 0xB: ea = (uint16_t)(r + d()); idle(3); emu(2); break;               // D,R
                case 0xC: { const uint16_t o = sext8(imm8()); ea = (uint16_t)(m_pc + o); idle(); break; }   // n8,PCR
                case 0xD: { const uint16_t o = imm16(); ea = (uint16_t)(m_pc + o); idle(2); emu(2); break; } // n16,PCR
                case 0xE: ea = (uint16_t)(r + w()); idle(2); break;                       // W,R
                default: ea = imm16(); emu(); break;                                      // [n16]
            }
    }
    if (pb & 0x10) { ea = r16(ea); idle(); }                                  // indirect
    return ea;
}

uint16_t HD6309::ea(int mode) {
    if (mode == 1) return eaDirect();
    if (mode == 2) return eaIndexed();
    return eaExtended();
}

uint8_t HD6309::operand8(int mode) { return mode == 0 ? imm8() : r8(ea(mode)); }
uint16_t HD6309::operand16(int mode) { return mode == 0 ? imm16() : r16(ea(mode)); }

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------
uint16_t HD6309::adc16(uint16_t a, uint16_t b, int carry) {
    const uint32_t r = (uint32_t)a + b + (uint32_t)carry;
    flag(CC_V, (a ^ r) & (b ^ r) & 0x8000);
    flag(CC_C, r & 0x10000);
    nz16((uint16_t)r);
    return (uint16_t)r;
}

uint16_t HD6309::sbc16(uint16_t a, uint16_t b, int borrow) {
    const uint32_t r = (uint32_t)a - b - (uint32_t)borrow;
    flag(CC_V, (a ^ b) & (a ^ r) & 0x8000);
    flag(CC_C, r & 0x10000);
    nz16((uint16_t)r);
    return (uint16_t)r;
}

// The D and W inherent group by the opcode's low nibble
uint16_t HD6309::rmw16(int op, uint16_t v) {
    uint16_t r = v;
    switch (op) {
        case 0x0: r = sub16(0, v); break;                                                     // NEG
        case 0x3: r = (uint16_t)~v; nz16(r); flag(CC_V, false); flag(CC_C, true); break;      // COM
        case 0x4: flag(CC_C, v & 1); r = v >> 1; nz16(r); break;                              // LSR
        case 0x6: r = (uint16_t)(v >> 1 | ((m_cc & CC_C) ? 0x8000 : 0)); flag(CC_C, v & 1); nz16(r); break;  // ROR
        case 0x7: r = (uint16_t)(v >> 1 | (v & 0x8000)); flag(CC_C, v & 1); nz16(r); break;   // ASR
        case 0x8: r = (uint16_t)(v << 1); flag(CC_C, v & 0x8000); flag(CC_V, (v ^ r) & 0x8000); nz16(r); break;  // ASL
        case 0x9:                                                                             // ROL
            r = (uint16_t)(v << 1 | (m_cc & CC_C));
            flag(CC_C, v & 0x8000); flag(CC_V, (v ^ r) & 0x8000); nz16(r);
            break;
        case 0xA: r = (uint16_t)(v - 1); flag(CC_V, v == 0x8000); nz16(r); break;            // DEC
        case 0xC: r = (uint16_t)(v + 1); flag(CC_V, v == 0x7FFF); nz16(r); break;            // INC
        case 0xD: nz16(v); flag(CC_V, false); break;                                          // TST
        case 0xF: r = 0; m_cc = (uint8_t)((m_cc & ~(CC_N | CC_V | CC_C)) | CC_Z); break;     // CLR
    }
    return r;
}

// ---------------------------------------------------------------------------
// Interrupts and traps
// ---------------------------------------------------------------------------
// The entire state; in native mode E and F go between B and DP, so the frame
// is 14 bytes rather than 12
void HD6309::stackEntire() {
    m_cc |= CC_E;
    idle();
    psh16(m_s, m_pc);
    psh16(m_s, m_u);
    psh16(m_s, m_y);
    psh16(m_s, m_x);
    psh8(m_s, m_dp);
    if (nativeMode()) { psh8(m_s, m_f); psh8(m_s, m_e); }
    psh8(m_s, m_b);
    psh8(m_s, m_a);
    psh8(m_s, m_cc);
}

void HD6309::stackFast() {
    m_cc &= (uint8_t)~CC_E;
    idle();
    psh16(m_s, m_pc);
    psh8(m_s, m_cc);
}

void HD6309::vectorTo(uint8_t mask, int vec) {
    m_cc |= mask;
    idle();
    m_pc = r16((uint16_t)(m_vectors | vec));
    idle();
}

// An undefined opcode: MD.IL set, the entire state stacked, the vector at $FFF0
void HD6309::illegal() {
    idle(2);
    m_md |= MD_IL;
    stackEntire();
    vectorTo(CC_I | CC_F, 0x0);
}

// An undefined indexed postbyte (hoglet67): the trap's stacking starts one
// cycle after the postbyte's own, three dead cycles after it, so LDA [,-X]
// takes 21 cycles (23 native) as an undefined page-2 opcode does. The index
// register is left alone and the stacked PC is the byte after the postbyte
// (neither is visible in the decoder's captures: unverified).
void HD6309::illegalPostbyte() {
    illegal();
    throw Abandon{};
}

bool HD6309::serviceInterrupt() {
    const bool nmi = m_nmiPending && m_nmiArmed;
    const bool firq = m_firq && !(m_cc & CC_F);
    const bool irq = m_irq && !(m_cc & CC_I);
    if (!nmi && !firq && !irq) return false;
    if (!m_cwaiStacked) {
        idle(2);
        if (!nmi && firq && !(m_md & MD_FM)) stackFast();   // FM: FIRQ stacks everything, E set
        else stackEntire();
    }
    if (nmi) { m_nmiPending = false; vectorTo(CC_I | CC_F, 0xC); }
    else if (firq) vectorTo(CC_I | CC_F, 0x6);
    else vectorTo(CC_I, 0x8);
    m_cwaiStacked = false;
    m_wait = WAIT_NONE;
    return true;
}

// One byte of a TFM: read, then (unless an interrupt is taken, which leaves
// the pointers and W alone so the TFM re-runs after RTI) a dead cycle and
// the write. 3 cycles a byte, as Atkinson's 6 + 3n.
void HD6309::tfmStep() {
    const int src = m_tfmPost >> 4, dst = m_tfmPost & 15;
    const uint8_t v = r8(reg16(src));
    m_m = v;
    if (serviceInterrupt()) { m_tfmActive = false; return; }
    idle();
    w8(reg16(dst), v);
    static const int SRC_STEP[4] = {1, -1, 1, 0}, DST_STEP[4] = {1, -1, 0, 1};
    setReg16(src, (uint16_t)(reg16(src) + SRC_STEP[m_tfmOp & 3]));
    setReg16(dst, (uint16_t)(reg16(dst) + DST_STEP[m_tfmOp & 3]));
    setW((uint16_t)(w() - 1));
    if (w() == 0) endTfm();
}

// TFM leaves Z set when it completes (hoglet67)
void HD6309::endTfm() {
    m_pc = (uint16_t)(m_pc + 3);
    m_cc |= CC_Z;
    m_tfmActive = false;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------
int HD6309::step() {
    m_cyc = 0;
    if (m_nmiPending && !m_nmiArmed) m_nmiPending = false;   // NMI waits until S has been loaded
    if (m_tfmActive) {
        tfmStep();
        m_cycles += (uint64_t)m_cyc;
        return m_cyc;
    }
    if (m_pcHook && m_pcHook->armed(m_pc) && m_pcHook->fire(this, m_pc)) {
        m_cycles += 2;
        return 2;
    }
    if (m_wait == WAIT_SYNC) {
        if (m_nmiPending || m_firq || m_irq) {
            m_wait = WAIT_NONE;
        } else {
            m_cycles += 1;
            return 1;
        }
    }
    if (serviceInterrupt()) { m_cycles += (uint64_t)m_cyc; return m_cyc; }
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
    try {
        uint8_t op = imm8();
        if (op == 0x10 || op == 0x11) {
            // Unlike the 6809's, the 6309's page prefixes do not chain: $10
            // or $11 after a prefix is an undefined opcode and traps, 21
            // cycles (23 native), as hoglet67's measured opcode table has it.
            // (XRoar lets them chain as the 6809's do.)
            const uint8_t page = op;
            op = imm8();
            if (op == 0x10 || op == 0x11) illegal();
            else if (page == 0x10) execPage2(op);
            else execPage3(op);
        } else {
            execPage0(op);
        }
    } catch (const Abandon&) {
        // an undefined indexed postbyte: the trap has been taken
    }
    m_cycles += (uint64_t)m_cyc;
    return m_cyc;
}

void HD6309::execPage0(uint8_t op) {
    const int hi = op >> 4, lo = op & 0x0F;
    if (hi == 0x0 || hi == 0x6 || hi == 0x7) {                                // memory read-modify-write
        const int mode = hi == 0x0 ? 1 : hi == 0x6 ? 2 : 3;
        if (lo == 0x1 || lo == 0x2 || lo == 0x5 || lo == 0xB) {               // OIM AIM EIM TIM
            const uint8_t im = imm8();
            m_m = im;                                                         // M is the immediate (hoglet67)
            if (lo == 0xB) {
                // TIM: 4 direct, 5+ indexed, 5 extended in both modes, no
                // dead cycles (hoglet67's measured table; a capture shows
                // TIM #$08,S+ in 7). Atkinson has 6 / 7+ / 7.
                const uint16_t a = mode == 1 ? (uint16_t)(m_dp << 8 | imm8()) : mode == 2 ? eaIndexed() : imm16();
                logic8((uint8_t)(r8(a) & im));
                return;
            }
            const uint16_t a = ea(mode);
            const uint8_t v = r8(a);
            const uint8_t r = logic8(lo == 0x1 ? (uint8_t)(v | im) : lo == 0x5 ? (uint8_t)(v ^ im) : (uint8_t)(v & im));
            w8(a, r);
            // Atkinson and hoglet67: 6 direct, 7+ indexed, 7 extended in both
            // modes. XRoar and MAME each guess differently.
            if (mode == 2 || nativeMode()) idle();
            return;
        }
        if (lo == 0xE) { m_pc = ea(mode); return; }                           // JMP
        const uint16_t a = ea(mode);
        const uint8_t r = rmw(lo, r8(a));
        m_m = r;
        idle();
        if (lo == 0xD) emu(); else w8(a, r);                                  // TST only reads
        return;
    }
    if (hi == 0x4 || hi == 0x5) {                                             // A / B inherent
        if (lo == 0x1 || lo == 0x2 || lo == 0x5 || lo == 0xB || lo == 0xE) { illegal(); return; }
        uint8_t& reg = hi == 0x4 ? m_a : m_b;
        reg = rmw(lo, reg);
        emu();
        return;
    }
    if (hi == 0x2) {                                                          // short branches
        const uint16_t o = sext8(imm8());
        idle();
        if (branchTaken(m_cc, lo)) m_pc = (uint16_t)(m_pc + o);
        return;
    }
    if (hi == 0x1) {
        switch (lo) {
            case 0x2: emu(); return;                                          // NOP
            case 0x3: emu(); idle(2); m_wait = WAIT_SYNC; return;             // SYNC
            case 0x4:                                                         // SEXW
                setD((m_e & 0x80) ? 0xFFFF : 0x0000);
                flag(CC_N, m_a & 0x80);
                flag(CC_Z, q() == 0);
                idle(3);
                return;
            case 0x6: { const uint16_t o = imm16(); idle(); emu(); m_pc = (uint16_t)(m_pc + o); return; }   // LBRA
            case 0x7: {                                                       // LBSR
                const uint16_t o = imm16();
                idle(2); emu(2);
                psh16(m_s, m_pc);
                m_pc = (uint16_t)(m_pc + o);
                return;
            }
            case 0x9: {                                                       // DAA
                // The 6809's correction, except that with C set and A in
                // $80-$99 the 6309 adds a different one ($80-$8F +$20 more,
                // $90-$97 +$10, $98 $68, $99 $68 - H), so C=1 A=$80 gives $00
                // where the 6809 gives $E0. V is bit 7 of the result XOR the
                // new C, on both CPUs. hoglet67's 6809Decoder (op_fn_DAA),
                // checked there exhaustively against a real 6309.
                const uint8_t lsn = m_a & 0x0F, msn = m_a >> 4;
                const int h = (m_cc & CC_H) ? 1 : 0;
                unsigned cf = 0;
                if (h || lsn > 9) cf |= 0x06;
                if ((m_cc & CC_C) || msn > 9 || (msn > 8 && lsn > 9)) cf |= 0x60;
                if ((m_cc & CC_C) && m_a >= 0x80 && m_a <= 0x99) {
                    if (m_a == 0x99) cf = (unsigned)(0x68 - h);
                    else if (m_a == 0x98) cf = 0x68;
                    else if (m_a >= 0x90) cf += 0x10;
                    else cf += 0x20;
                }
                const unsigned r = m_a + cf;
                if (r & 0x100) m_cc |= CC_C;
                m_a = (uint8_t)r;
                nz8(m_a);
                flag(CC_V, ((r >> 7) & 1) ^ ((m_cc & CC_C) ? 1 : 0));
                emu();
                return;
            }
            // ORCC/ANDCC: 3 cycles in both modes (Atkinson, and hoglet67's
            // measured table); XRoar and MAME have 2 in native mode
            case 0xA: m_cc |= imm8(); idle(); return;                         // ORCC
            case 0xC: m_cc &= imm8(); idle(); return;                         // ANDCC
            case 0xD: m_a = (m_b & 0x80) ? 0xFF : 0x00; nz16(d()); emu(); return;   // SEX
            case 0xE: {                                                       // EXG
                const uint8_t pb = imm8();
                const uint16_t s = tfrRead(pb >> 4), t = tfrRead(pb & 15);
                tfrWrite(pb & 15, s);
                tfrWrite(pb >> 4, t);
                idle(3); emu(3);
                return;
            }
            case 0xF: {                                                       // TFR
                const uint8_t pb = imm8();
                tfrWrite(pb & 15, tfrRead(pb >> 4));
                idle(2); emu(2);
                return;
            }
        }
        illegal();                                                            // $15 $18 $1B
        return;
    }
    if (hi == 0x3) {
        switch (lo) {
            case 0x0: m_x = eaIndexed(); idle(); flag(CC_Z, !m_x); return;    // LEAX
            case 0x1: m_y = eaIndexed(); idle(); flag(CC_Z, !m_y); return;    // LEAY
            case 0x2: { const uint16_t a = eaIndexed(); idle(); m_s = a; m_nmiArmed = true; return; }   // LEAS
            case 0x3: { const uint16_t a = eaIndexed(); idle(); m_u = a; return; }                       // LEAU
            case 0x4: case 0x6: {                                             // PSHS, PSHU
                const uint8_t pb = imm8();
                idle(); emu(); idle();
                uint16_t& sp = lo == 0x4 ? m_s : m_u;
                const uint16_t other = lo == 0x4 ? m_u : m_s;
                if (pb & 0x80) psh16(sp, m_pc);
                if (pb & 0x40) psh16(sp, other);
                if (pb & 0x20) psh16(sp, m_y);
                if (pb & 0x10) psh16(sp, m_x);
                if (pb & 0x08) psh8(sp, m_dp);
                if (pb & 0x04) psh8(sp, m_b);
                if (pb & 0x02) psh8(sp, m_a);
                if (pb & 0x01) psh8(sp, m_cc);
                return;
            }
            case 0x5: case 0x7: {                                             // PULS, PULU
                const uint8_t pb = imm8();
                idle(); emu();
                uint16_t& sp = lo == 0x5 ? m_s : m_u;
                if (pb & 0x01) m_cc = pul8(sp);
                if (pb & 0x02) m_a = pul8(sp);
                if (pb & 0x04) m_b = pul8(sp);
                if (pb & 0x08) m_dp = pul8(sp);
                if (pb & 0x10) m_x = pul16(sp);
                if (pb & 0x20) m_y = pul16(sp);
                if (pb & 0x40) {
                    const uint16_t v = pul16(sp);
                    if (lo == 0x5) m_u = v; else { m_s = v; m_nmiArmed = true; }
                }
                if (pb & 0x80) m_pc = pul16(sp);
                idle();
                return;
            }
            case 0x9: emu(); m_pc = pul16(m_s); idle(); return;               // RTS
            case 0xA: m_x = (uint16_t)(m_x + m_b); emu(2); return;            // ABX
            case 0xB:                                                         // RTI (E and F too in native mode)
                idle();
                m_cc = pul8(m_s);
                if (m_cc & CC_E) {
                    m_a = pul8(m_s);
                    m_b = pul8(m_s);
                    if (nativeMode()) { m_e = pul8(m_s); m_f = pul8(m_s); }
                    m_dp = pul8(m_s);
                    m_x = pul16(m_s);
                    m_y = pul16(m_s);
                    m_u = pul16(m_s);
                }
                m_pc = pul16(m_s);
                idle();
                return;
            case 0xC:                                                         // CWAI
                m_cc &= imm8();
                idle(2);
                stackEntire();
                idle();
                m_wait = WAIT_CWAI;
                m_cwaiStacked = true;
                return;
            case 0xD: {                                                       // MUL
                m_m = m_b;
                const uint16_t r = (uint16_t)(m_a * m_b);
                setD(r);
                flag(CC_Z, !r);
                flag(CC_C, r & 0x80);
                idle(9); emu();
                return;
            }
            case 0xF:                                                         // SWI
                idle();
                stackEntire();
                vectorTo(CC_I | CC_F, 0xA);
                return;
        }
        illegal();                                                            // $38 $3E
        return;
    }
    // $80-$FF: the A / B / D / X / U group; mode by bits 4-5
    const int mode = (op >> 4) & 3;
    const bool bSide = op & 0x40;
    uint8_t& acc = bSide ? m_b : m_a;
    switch (lo) {
        case 0x0: acc = sub8(acc, operand8(mode), 0); return;                 // SUB
        case 0x1: sub8(acc, operand8(mode), 0); return;                       // CMP
        case 0x2: { const uint8_t v = operand8(mode); acc = sub8(acc, v, m_cc & CC_C); return; }   // SBC
        case 0x3: {                                                           // SUBD / ADDD
            const uint16_t v = operand16(mode);
            setD(bSide ? add16(d(), v) : sub16(d(), v));
            emu();
            return;
        }
        case 0x4: acc = logic8((uint8_t)(acc & operand8(mode))); return;      // AND
        case 0x5: logic8((uint8_t)(acc & operand8(mode))); return;            // BIT
        case 0x6: acc = logic8(operand8(mode)); return;                       // LD
        case 0x7:                                                             // ST
            if (mode == 0) { illegal(); return; }
            w8(ea(mode), acc);
            logic8(acc);
            return;
        case 0x8: acc = logic8((uint8_t)(acc ^ operand8(mode))); return;      // EOR
        case 0x9: { const uint8_t v = operand8(mode); acc = add8(acc, v, m_cc & CC_C); return; }   // ADC
        case 0xA: acc = logic8((uint8_t)(acc | operand8(mode))); return;      // OR
        case 0xB: acc = add8(acc, operand8(mode), 0); return;                 // ADD
        case 0xC:
            if (!bSide) { sub16(m_x, operand16(mode)); emu(); return; }       // CMPX
            setD(logic16(operand16(mode)));                                   // LDD
            return;
        case 0xD:
            if (!bSide) {
                if (mode == 0) {                                              // BSR
                    const uint16_t o = sext8(imm8());
                    idle(2); emu();
                    psh16(m_s, m_pc);
                    m_pc = (uint16_t)(m_pc + o);
                    return;
                }
                const uint16_t a = ea(mode);                                  // JSR
                // indexed: 7+ / 6+ (hoglet67's table); direct and extended
                // already lose their dead cycle in eaDirect / eaExtended
                if (mode == 2) { idle(); emu(); } else idle(2);
                psh16(m_s, m_pc);
                m_pc = a;
                return;
            }
            if (mode == 0) {                                                  // LDQ #i32
                setD(imm16());
                setW(imm16());
                // Z from D alone and V left alone (hoglet67)
                flag(CC_N, m_a & 0x80);
                flag(CC_Z, d() == 0);
                return;
            }
            { const uint16_t a = ea(mode); w16(a, d()); logic16(d()); return; }   // STD
        case 0xE: {                                                           // LDX / LDU
            uint16_t& r = bSide ? m_u : m_x;
            r = logic16(operand16(mode));
            return;
        }
        default: {                                                            // STX / STU
            if (mode == 0) { illegal(); return; }
            const uint16_t r = bSide ? m_u : m_x;
            w16(ea(mode), r);
            logic16(r);
            return;
        }
    }
}

// ADDR ADCR SUBR SBCR ANDR ORR EORR CMPR: the size is the destination's. A
// 16-bit destination takes A/B as D, E/F as W, CC as $00:CC and DP as DP:M
// (M the internal latch, hoglet67; Atkinson says DP:$00); an 8-bit one takes
// a 16-bit source's low byte. With CC as the destination the result is
// stored before the flags are set over it (Atkinson, inter-register ops).
//
// PC as an operand (hoglet67's decoder, get_r_pc): it reads one ahead of the
// next instruction's address, through pipelining, in both modes; as a
// destination it reads the same way and the result is written to PC at once.
// On a real part the write lands late, on the next instruction's second byte
// ("cannot safely write to the PC"); that is not modelled. ADDR and ADCR
// leave H alone (hoglet67), as the other six do.
void HD6309::interRegister(uint8_t op) {
    const uint8_t pb = imm8();
    idle();
    const int src = pb >> 4, dst = pb & 15, k = op & 7;
    const int carry = m_cc & CC_C;
    const uint8_t h = m_cc & CC_H;
    const uint8_t mask = (k >= 4 && k <= 6) ? (CC_N | CC_Z | CC_V) : (CC_N | CC_Z | CC_V | CC_C);
    const uint16_t pcSeen = (uint16_t)(m_pc + 1);
    auto rd16r = [&](int code) { return code == 0x5 ? pcSeen : tfrRead(code); };
    if (!(dst & 8)) {
        const uint16_t a = rd16r(dst);
        uint16_t b;
        switch (src) {
            case 0x8: case 0x9: b = d(); break;
            case 0xA: b = m_cc; break;
            case 0xB: b = (uint16_t)(m_dp << 8 | m_m); break;
            case 0xC: case 0xD: b = 0; break;
            case 0xE: case 0xF: b = w(); break;
            default: b = rd16r(src); break;
        }
        uint16_t r;
        switch (k) {
            case 0: r = add16(a, b); break;
            case 1: r = adc16(a, b, carry); break;
            case 2: r = sub16(a, b); break;
            case 3: r = sbc16(a, b, carry); break;
            case 4: r = logic16(a & b); break;
            case 5: r = logic16(a | b); break;
            case 6: r = logic16(a ^ b); break;
            default: sub16(a, b); return;                                     // CMPR
        }
        tfrWrite(dst, r);
        return;
    }
    uint8_t* dreg = nullptr;
    switch (dst) {
        case 0x8: dreg = &m_a; break;
        case 0x9: dreg = &m_b; break;
        case 0xA: dreg = &m_cc; break;
        case 0xB: dreg = &m_dp; break;
        case 0xE: dreg = &m_e; break;
        case 0xF: dreg = &m_f; break;
    }
    const uint8_t a = dreg ? *dreg : 0;
    const uint8_t b = (uint8_t)rd16r(src);                                    // 8-bit: both halves; 16-bit: low byte
    uint8_t r;
    switch (k) {
        case 0: r = add8(a, b, 0); m_cc = (uint8_t)((m_cc & ~CC_H) | h); break;
        case 1: r = add8(a, b, carry); m_cc = (uint8_t)((m_cc & ~CC_H) | h); break;
        case 2: r = sub8(a, b, 0); break;
        case 3: r = sub8(a, b, carry); break;
        case 4: r = logic8(a & b); break;
        case 5: r = logic8(a | b); break;
        case 6: r = logic8(a ^ b); break;
        default: sub8(a, b, 0); return;                                       // CMPR
    }
    if (dreg == &m_cc) m_cc = (uint8_t)(r | (m_cc & mask));                  // flags set over the result
    else if (dreg) *dreg = r;
}

// BAND BIAND BOR BIOR BEOR BIEOR LDBT STBT: postbyte rr sss ddd, register
// rr (CC A B; 3 reads as 0 and is not written), memory bit sss, register
// bit ddd (STBT: register bit sss into memory bit ddd)
void HD6309::bitOp(uint8_t op) {
    const uint8_t pb = imm8();
    const uint16_t a = eaDirect();
    uint8_t mem = r8(a);
    idle();
    const int dBit = pb & 7, sBit = (pb >> 3) & 7, kind = op & 7;
    uint8_t* reg = (pb >> 6) == 0 ? &m_cc : (pb >> 6) == 1 ? &m_a : (pb >> 6) == 2 ? &m_b : nullptr;
    const uint8_t rv = reg ? *reg : 0;
    const int mb = (mem >> sBit) & 1, nb = (~mem >> sBit) & 1, rb = (rv >> dBit) & 1;
    int out;
    switch (kind) {
        case 0: out = mb & rb; break;
        case 1: out = nb & rb; break;
        case 2: out = mb | rb; break;
        case 3: out = nb | rb; break;
        case 4: out = mb ^ rb; break;
        case 5: out = nb ^ rb; break;
        case 6: out = mb; break;
        default: out = (rv >> sBit) & 1; break;
    }
    if (kind == 7) {
        mem = (uint8_t)((mem & ~(1 << dBit)) | (out << dBit));
        w8(a, mem);
    } else if (reg) {
        *reg = (uint8_t)((*reg & ~(1 << dBit)) | (out << dBit));
    }
}

// DIVD: D / M8, signed; quotient in B, remainder in A. The sequence (sign
// handling cycles, range and two's-complement overflow) after hoglet67 via
// XRoar. Atkinson says a range overflow clears N; the measured part sets N
// for a negative dividend, which is followed here.
void HD6309::divd(int mode) {
    const uint8_t divisor = operand8(mode);
    idle(3);
    if (divisor == 0) {
        m_md |= MD_DZ;
        m_cc = (uint8_t)((m_cc & ~(CC_N | CC_V)) | CC_Z);
        stackEntire();
        vectorTo(CC_I | CC_F, 0x0);
        return;
    }
    uint16_t num = d();
    uint8_t den = divisor;
    const bool numNeg = num & 0x8000, denNeg = den & 0x80;
    if (numNeg) { num = (uint16_t)-num; setD(num); idle(); }
    if (denNeg) { den = (uint8_t)-den; idle(); }
    idle(6);
    uint16_t quo = (uint16_t)(num / den);
    uint8_t rem = (uint8_t)(num % den);
    m_cc &= (uint8_t)~(CC_N | CC_Z | CC_V | CC_C);
    if (quo >> 8) {                                                           // range overflow: D keeps |D|
        m_cc |= CC_V;
        if (numNeg) m_cc |= CC_N;
        return;
    }
    if (numNeg) rem = (uint8_t)-rem;
    if (numNeg != denNeg) {
        const uint16_t neg = (uint16_t)-quo;
        if ((neg & 0x80) && !(quo & 0x80)) quo = neg;
        m_m = 0xFF;
    } else {
        m_m = 0;
    }
    idle(12);
    m_a = rem;
    m_b = (uint8_t)quo;
    if (m_b & 1) m_cc |= CC_C;
    if ((((quo >> 15) ^ (m_b >> 7)) & 1) == 0) { nz8(m_b); idle(); }
    else m_cc |= CC_N | CC_V;                                                 // two's-complement overflow
}

// DIVQ: Q / M16, signed; quotient in W, remainder in D
void HD6309::divq(int mode) {
    const uint16_t divisor = operand16(mode);
    idle(3);
    if (divisor == 0) {
        m_md |= MD_DZ;
        m_cc = (uint8_t)((m_cc & ~(CC_N | CC_V)) | CC_Z);
        stackEntire();
        vectorTo(CC_I | CC_F, 0x0);
        return;
    }
    uint32_t num = q();
    uint16_t den = divisor;
    const bool numNeg = num & 0x80000000u, denNeg = den & 0x8000;
    if (numNeg) { num = (uint32_t)-num; setD((uint16_t)(num >> 16)); setW((uint16_t)num); idle(); }
    if (denNeg) { den = (uint16_t)-den; idle(); }
    idle(6);
    uint32_t quo = num / den;
    uint16_t rem = (uint16_t)(num % den);
    m_cc &= (uint8_t)~(CC_N | CC_Z | CC_V | CC_C);
    if (quo >> 16) {
        m_cc |= CC_V;
        if (numNeg) m_cc |= CC_N;
        return;
    }
    if (numNeg) rem = (uint16_t)-rem;
    if (numNeg != denNeg) {
        const uint32_t neg = (uint32_t)-quo;
        if ((neg & 0x8000) && !(quo & 0x8000)) quo = neg;
        m_m = 0xFF;
    } else {
        m_m = 0;
    }
    idle(21);
    setD(rem);
    setW((uint16_t)quo);
    if (m_f & 1) m_cc |= CC_C;
    if ((((quo >> 31) ^ (m_e >> 7)) & 1) == 0) nz16(w());
    else m_cc |= CC_N | CC_V;
}

void HD6309::execPage2(uint8_t op) {
    if (op >= 0x21 && op <= 0x2F) {                                           // long branches
        const uint16_t o = imm16();
        if (branchTaken(m_cc, op)) { m_pc = (uint16_t)(m_pc + o); emu(); }
        idle();
        return;
    }
    if (op >= 0x30 && op <= 0x37) { interRegister(op); return; }
    switch (op) {
        case 0x38: idle(2); psh8(m_s, m_f); psh8(m_s, m_e); return;          // PSHSW
        case 0x39: idle(2); m_e = pul8(m_s); m_f = pul8(m_s); return;        // PULSW
        case 0x3A: idle(2); psh8(m_u, m_f); psh8(m_u, m_e); return;          // PSHUW
        case 0x3B: idle(2); m_e = pul8(m_u); m_f = pul8(m_u); return;        // PULUW
        case 0x3F: idle(); stackEntire(); vectorTo(0, 0x4); return;           // SWI2
    }
    if (op >= 0x40 && op <= 0x5F) {                                           // D / W inherent
        const int lo = op & 15;
        // D: NEG COM LSR ROR ASR ASL ROL DEC INC TST CLR; W: no NEG ASR ASL
        // (MAME also decodes $1050 as NEGW; XRoar and Atkinson do not)
        static const uint16_t D_OPS = 0xB7D9, W_OPS = 0xB658;                 // bit n: low nibble n valid
        if (!(((op & 0x10) ? W_OPS : D_OPS) >> lo & 1)) { illegal(); return; }
        const uint16_t r = rmw16(lo, (op & 0x10) ? w() : d());
        if (lo != 0xD) { if (op & 0x10) setW(r); else setD(r); }
        emu();
        return;
    }
    if (op < 0x80) { illegal(); return; }
    const int mode = (op >> 4) & 3, lo = op & 15;
    if (op < 0xC0) {
        switch (lo) {
            case 0x0: setW(sub16(w(), operand16(mode))); emu(); return;       // SUBW
            case 0x1: sub16(w(), operand16(mode)); emu(); return;             // CMPW
            case 0x2: { const uint16_t v = operand16(mode); setD(sbc16(d(), v, m_cc & CC_C)); emu(); return; }   // SBCD
            case 0x3: sub16(d(), operand16(mode)); emu(); return;             // CMPD
            case 0x4: setD(logic16(d() & operand16(mode))); emu(); return;    // ANDD
            case 0x5: logic16(d() & operand16(mode)); emu(); return;          // BITD
            case 0x6: setW(logic16(operand16(mode))); return;                 // LDW
            case 0x7:                                                         // STW
                if (mode == 0) break;
                w16(ea(mode), w()); logic16(w());
                return;
            case 0x8: setD(logic16(d() ^ operand16(mode))); emu(); return;    // EORD
            case 0x9: { const uint16_t v = operand16(mode); setD(adc16(d(), v, m_cc & CC_C)); emu(); return; }   // ADCD
            case 0xA: setD(logic16(d() | operand16(mode))); emu(); return;    // ORD
            case 0xB: setW(add16(w(), operand16(mode))); emu(); return;       // ADDW
            case 0xC: sub16(m_y, operand16(mode)); emu(); return;             // CMPY
            case 0xE: m_y = logic16(operand16(mode)); return;                 // LDY
            case 0xF:                                                         // STY
                if (mode == 0) break;
                w16(ea(mode), m_y); logic16(m_y);
                return;
        }
        illegal();
        return;
    }
    switch (lo) {
        case 0xC: case 0xD: {                                                 // LDQ / STQ
            if (mode == 0) break;
            const uint16_t a = ea(mode);
            if (lo == 0xC) { setD(r16(a)); setW(r16((uint16_t)(a + 2))); }
            else { w16(a, d()); w16((uint16_t)(a + 2), w()); }
            flag(CC_N, m_a & 0x80);
            flag(CC_Z, q() == 0);
            flag(CC_V, false);
            return;
        }
        case 0xE: m_s = logic16(operand16(mode)); m_nmiArmed = true; return;  // LDS
        case 0xF:                                                             // STS
            if (mode == 0) break;
            w16(ea(mode), m_s); logic16(m_s);
            return;
    }
    illegal();
}

void HD6309::execPage3(uint8_t op) {
    if (op >= 0x30 && op <= 0x37) { bitOp(op); return; }
    switch (op) {
        case 0x38: case 0x39: case 0x3A: case 0x3B: {                         // TFM
            const uint8_t pb = imm8();
            if ((pb >> 4) > 4 || (pb & 15) > 4) {                             // only D X Y U S (Atkinson)
                // 23 cycles (25 native), Z from W (hoglet67's decoder)
                flag(CC_Z, w() == 0);
                idle();
                illegal();
                return;
            }
            idle(3);
            m_cc &= (uint8_t)~CC_Z;
            m_tfmOp = op;
            m_tfmPost = pb;
            m_pc = (uint16_t)(m_pc - 3);                                      // PC stays on the TFM while it runs
            if (w() == 0) endTfm(); else m_tfmActive = true;
            return;
        }
        case 0x3C: {                                                          // BITMD: tests and clears /0 and IL
            const uint8_t im = imm8();
            m_m = im;
            const uint8_t bits = im & (MD_DZ | MD_IL);
            flag(CC_Z, !(m_md & bits));
            m_md &= (uint8_t)~bits;
            idle();
            return;
        }
        case 0x3D: m_md = (uint8_t)((m_md & ~(MD_NM | MD_FM)) | (imm8() & (MD_NM | MD_FM))); idle(2); return;  // LDMD
        case 0x3F: idle(); stackEntire(); vectorTo(0, 0x2); return;           // SWI3
    }
    if (op >= 0x40 && op <= 0x5F) {                                           // E / F inherent: COM DEC INC TST CLR
        const int lo = op & 15;
        if (lo != 0x3 && lo != 0xA && lo != 0xC && lo != 0xD && lo != 0xF) { illegal(); return; }
        uint8_t& reg = (op & 0x10) ? m_f : m_e;
        reg = rmw(lo, reg);
        emu();      // TSTE/TSTF 3/2 as Atkinson and MAME (XRoar adds a cycle)
        return;
    }
    if (op < 0x80) { illegal(); return; }
    const int mode = (op >> 4) & 3, lo = op & 15;
    uint8_t& acc = (op & 0x40) ? m_f : m_e;
    switch (lo) {
        case 0x0: acc = sub8(acc, operand8(mode), 0); return;                 // SUBE / SUBF
        case 0x1: sub8(acc, operand8(mode), 0); return;                       // CMPE / CMPF
        case 0x6: acc = logic8(operand8(mode)); return;                       // LDE / LDF
        case 0x7:                                                             // STE / STF
            if (mode == 0) break;
            w8(ea(mode), acc); logic8(acc);
            return;
        case 0xB: acc = add8(acc, operand8(mode), 0); return;                 // ADDE / ADDF
    }
    if (op < 0xC0) {
        switch (lo) {
            case 0x3: sub16(m_u, operand16(mode)); emu(); return;             // CMPU
            case 0xC: sub16(m_s, operand16(mode)); emu(); return;             // CMPS
            case 0xD: divd(mode); return;                                     // DIVD
            case 0xE: divq(mode); return;                                     // DIVQ
            case 0xF: {                                                       // MULD: Q = D * M16, signed
                const uint16_t v = operand16(mode);
                const int32_t r = (int32_t)(int16_t)d() * (int32_t)(int16_t)v;
                m_m = r < 0 ? 0xFF : 0x00;
                idle(24);
                setD((uint16_t)((uint32_t)r >> 16));
                setW((uint16_t)r);
                // N and Z from D alone (Atkinson note 6, hoglet67); C cleared
                // (Atkinson, MAME); V left alone (Atkinson, XRoar)
                flag(CC_N, m_a & 0x80);
                flag(CC_Z, d() == 0);
                flag(CC_C, false);
                return;
            }
        }
    }
    illegal();
}

// ---------------------------------------------------------------------------
// Disassembler (Motorola / Atkinson mnemonics)
// ---------------------------------------------------------------------------
namespace {
const char* const RMW[16] = {"NEG", "OIM", "AIM", "COM", "LSR", "EIM", "ROR", "ASR",
                             "ASL", "ROL", "DEC", "TIM", "INC", "TST", "JMP", "CLR"};
const char* const BR[16] = {"BRA", "BRN", "BHI", "BLS", "BCC", "BCS", "BNE", "BEQ",
                            "BVC", "BVS", "BPL", "BMI", "BGE", "BLT", "BGT", "BLE"};
const char* const ACCA[16] = {"SUBA", "CMPA", "SBCA", "SUBD", "ANDA", "BITA", "LDA", "STA",
                              "EORA", "ADCA", "ORA", "ADDA", "CMPX", "JSR", "LDX", "STX"};
const char* const ACCB[16] = {"SUBB", "CMPB", "SBCB", "ADDD", "ANDB", "BITB", "LDB", "STB",
                              "EORB", "ADCB", "ORB", "ADDB", "LDD", "STD", "LDU", "STU"};
const char* const P2[16] = {"SUBW", "CMPW", "SBCD", "CMPD", "ANDD", "BITD", "LDW", "STW",
                            "EORD", "ADCD", "ORD", "ADDW", "CMPY", nullptr, "LDY", "STY"};
const char* const P3E[16] = {"SUBE", "CMPE", nullptr, "CMPU", nullptr, nullptr, "LDE", "STE",
                             nullptr, nullptr, nullptr, "ADDE", "CMPS", "DIVD", "DIVQ", "MULD"};
const char* const P3F[16] = {"SUBF", "CMPF", nullptr, nullptr, nullptr, nullptr, "LDF", "STF",
                             nullptr, nullptr, nullptr, "ADDF", nullptr, nullptr, nullptr, nullptr};
const char* const INTER[8] = {"ADDR", "ADCR", "SUBR", "SBCR", "ANDR", "ORR", "EORR", "CMPR"};
const char* const BITOPS[8] = {"BAND", "BIAND", "BOR", "BIOR", "BEOR", "BIEOR", "LDBT", "STBT"};
const char* const TREG[16] = {"D", "X", "Y", "U", "S", "PC", "W", "V", "A", "B", "CC", "DP", "0", "0", "E", "F"};

struct Dis {
    IBus* bus;
    uint32_t pc;
    int len = 0;
    bool bad = false;                       // an undefined indexed postbyte was met
    uint8_t byte() { return bus ? bus->peek8((pc + len++) & 0xFFFF) : 0; }
    uint16_t word() { const uint8_t h = byte(); return (uint16_t)(h << 8 | byte()); }
    static std::string hex2(unsigned v) { char b[8]; std::snprintf(b, sizeof b, "$%02X", v & 0xFF); return b; }
    static std::string hex4(unsigned v) { char b[8]; std::snprintf(b, sizeof b, "$%04X", v & 0xFFFF); return b; }
    std::string idx() {
        static const char* R[4] = {"X", "Y", "U", "S"};
        const uint8_t pb = byte();
        const std::string r = R[(pb >> 5) & 3];
        if (illegalIndexPostbyte(pb)) { bad = true; return ""; }
        if (!(pb & 0x80)) {
            const int o = (pb & 0x10) ? (int)(pb & 0x1F) - 32 : (pb & 0x1F);
            return std::to_string(o) + "," + r;
        }
        std::string s;
        switch (pb) {
            case 0x8F: return ",W";
            case 0x90: return "[,W]";
            case 0xAF: return hex4(word()) + ",W";
            case 0xB0: return "[" + hex4(word()) + ",W]";
            case 0xCF: return ",W++";
            case 0xD0: return "[,W++]";
            case 0xEF: return ",--W";
            case 0xF0: return "[,--W]";
        }
        switch (pb & 0x0F) {
            case 0x0: s = "," + r + "+"; break;
            case 0x1: s = "," + r + "++"; break;
            case 0x2: s = ",-" + r; break;
            case 0x3: s = ",--" + r; break;
            case 0x4: s = "," + r; break;
            case 0x5: s = "B," + r; break;
            case 0x6: s = "A," + r; break;
            case 0x7: s = "E," + r; break;
            case 0x8: s = std::to_string((int8_t)byte()) + "," + r; break;
            case 0x9: s = hex4(word()) + "," + r; break;
            case 0xA: s = "F," + r; break;
            case 0xB: s = "D," + r; break;
            case 0xC: { const int8_t o = (int8_t)byte(); s = hex4(pc + len + o) + ",PCR"; break; }
            case 0xD: { const uint16_t o = word(); s = hex4(pc + len + o) + ",PCR"; break; }
            case 0xE: s = "W," + r; break;
            default: s = hex4(word()); break;
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
    static std::string regList(uint8_t pb, bool u) {
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

std::string disPage2(Dis& d, uint32_t addr) {
    const uint8_t op = d.byte();
    const int mode = (op >> 4) & 3, lo = op & 15;
    if (op >= 0x21 && op <= 0x2F) { const uint16_t o = d.word(); return std::string("L") + BR[lo] + " " + d.hex4(addr + d.len + o); }
    if (op >= 0x30 && op <= 0x37) { const uint8_t pb = d.byte(); return std::string(INTER[op & 7]) + " " + TREG[pb >> 4] + "," + TREG[pb & 15]; }
    switch (op) {
        case 0x38: return "PSHSW";
        case 0x39: return "PULSW";
        case 0x3A: return "PSHUW";
        case 0x3B: return "PULUW";
        case 0x3F: return "SWI2";
    }
    if (op >= 0x40 && op <= 0x5F) {
        static const uint16_t D_OPS = 0xB7D9, W_OPS = 0xB658;
        static const char* const N[16] = {"NEG", nullptr, nullptr, "COM", "LSR", nullptr, "ROR", "ASR",
                                          "ASL", "ROL", "DEC", nullptr, "INC", "TST", nullptr, "CLR"};
        if (!(((op & 0x10) ? W_OPS : D_OPS) >> lo & 1)) return "???";
        return std::string(N[lo]) + ((op & 0x10) ? "W" : "D");
    }
    if (op >= 0x80 && op < 0xC0 && P2[lo] && !(mode == 0 && (lo == 7 || lo == 0xF)))
        return std::string(P2[lo]) + " " + d.operand(mode, true);
    if (op >= 0xC0 && lo >= 0xC && !(mode == 0 && lo != 0xE)) {
        static const char* const N[4] = {"LDQ", "STQ", "LDS", "STS"};
        return std::string(N[lo - 0xC]) + " " + d.operand(mode, true);
    }
    return "???";
}

std::string disPage3(Dis& d) {
    const uint8_t op = d.byte();
    const int mode = (op >> 4) & 3, lo = op & 15;
    if (op >= 0x30 && op <= 0x37) {
        static const char* const R[4] = {"CC", "A", "B", "?"};
        const uint8_t pb = d.byte();
        return std::string(BITOPS[op & 7]) + " " + R[pb >> 6] + "," + std::to_string((pb >> 3) & 7) + "," +
               std::to_string(pb & 7) + ",<" + d.hex2(d.byte());
    }
    if (op >= 0x38 && op <= 0x3B) {
        static const char* const S[4] = {"+", "-", "+", ""}, *const T[4] = {"+", "-", "", "+"};
        const uint8_t pb = d.byte();
        return std::string("TFM ") + TREG[pb >> 4] + S[op & 3] + "," + TREG[pb & 15] + T[op & 3];
    }
    switch (op) {
        case 0x3C: return "BITMD #" + d.hex2(d.byte());
        case 0x3D: return "LDMD #" + d.hex2(d.byte());
        case 0x3F: return "SWI3";
    }
    if (op >= 0x40 && op <= 0x5F) {
        static const char* const N[16] = {nullptr, nullptr, nullptr, "COM", nullptr, nullptr, nullptr, nullptr,
                                          nullptr, nullptr, "DEC", nullptr, "INC", "TST", nullptr, "CLR"};
        return N[lo] ? std::string(N[lo]) + ((op & 0x10) ? "F" : "E") : "???";
    }
    if (op < 0x80) return "???";
    const char* n = (op & 0x40) ? P3F[lo] : P3E[lo];
    if (!n || (mode == 0 && lo == 7)) return "???";
    const bool wide = lo == 0x3 || lo == 0xC || lo == 0xE || lo == 0xF;
    return std::string(n) + " " + d.operand(mode, wide);
}
}  // namespace

int HD6309::disassembleOne(IBus* bus, uint32_t addr, char* buf, int bufsz) {
    Dis d{bus, addr};
    std::string s;
    uint8_t op = d.byte();
    const int hi = op >> 4, lo = op & 15;
    if (op == 0x10 || op == 0x11) {
        s = op == 0x10 ? disPage2(d, addr) : disPage3(d);
    } else if (hi == 0x0 || hi == 0x6 || hi == 0x7) {
        const int mode = hi == 0 ? 1 : hi == 6 ? 2 : 3;
        if (lo == 0x1 || lo == 0x2 || lo == 0x5 || lo == 0xB) {
            const uint8_t im = d.byte();
            s = std::string(RMW[lo]) + " #" + d.hex2(im) + ";" + d.operand(mode, false);
        } else {
            s = std::string(RMW[lo]) + " " + d.operand(mode, false);
        }
    } else if (hi == 0x4 || hi == 0x5) {
        s = (lo == 0x1 || lo == 0x2 || lo == 0x5 || lo == 0xB || lo == 0xE) ? "???" : std::string(RMW[lo]) + (hi == 4 ? "A" : "B");
    } else if (hi == 0x2) {
        const int8_t o = (int8_t)d.byte();
        s = std::string(BR[lo]) + " " + d.hex4(addr + d.len + o);
    } else if (hi == 0x1) {
        switch (lo) {
            case 0x2: s = "NOP"; break;
            case 0x3: s = "SYNC"; break;
            case 0x4: s = "SEXW"; break;
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
        } else if (bSide && lo == 0xD && mode == 0) {
            const uint16_t h = d.word(), l = d.word();
            char b[16];
            std::snprintf(b, sizeof b, "#$%04X%04X", h, l);
            s = std::string("LDQ ") + b;
        } else if (mode == 0 && (lo == 0x7 || lo == 0xF)) {
            s = "???";
        } else {
            const bool wide = lo == 3 || lo >= 0xC;
            s = std::string(bSide ? ACCB[lo] : ACCA[lo]) + " " + d.operand(mode, wide);
        }
    }
    if (d.bad) s = "???";
    std::snprintf(buf, (size_t)bufsz, "%s", s.c_str());
    return d.len;
}

int HD6309::disassembleEntry(IBus* bus, uint32_t addr, void* entryOut) {
    auto* e = static_cast<DisasmEntry*>(entryOut);
    char buf[64];
    e->addr = addr;
    e->bytes = disassembleOne(bus, addr, buf, sizeof buf);
    const std::string s(buf);
    const auto sp = s.find(' ');
    e->mnemonic = sp == std::string::npos ? s : s.substr(0, sp);
    e->operands = sp == std::string::npos ? "" : s.substr(sp + 1);
    e->complete = s;
    e->isCall = isCallAt(bus, addr) > 0;
    e->isReturn = isReturnAt(bus, addr);
    // A branch is Bcc / LBcc / LBRA / JMP (BAND, BITD, LDQ ... are not)
    const std::string& m = e->mnemonic;
    const std::string cond = m.size() == 4 && m[0] == 'L' ? m.substr(1) : m;
    bool br = m == "JMP" || m == "LBRA";
    for (const char* b : BR) br = br || cond == b;
    e->isBranch = br && !e->isCall;
    e->isIllegal = s == "???";
    e->targetAddr = 0;
    e->rawLen = (uint8_t)std::min(e->bytes, (int)DisasmEntry::MAX_RAW);
    for (int i = 0; i < e->rawLen; ++i) e->raw[i] = bus ? bus->peek8((addr + i) & 0xFFFF) : 0;
    return e->bytes;
}
