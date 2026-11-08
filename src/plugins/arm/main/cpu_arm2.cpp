// Acorn ARM1/ARM2 core: the 26-bit architecture (ARM2 datasheet, VLSI
// VL86C010; the ARM1 manual): instructions, the barrel shifter, the PSR in
// R15, banked registers and exceptions.

#include "cpu_arm2.h"
#include "libdebug/main/execution_observer.h"
#include "libcore/main/intercept.h"
#include "libtoolchain/main/idisasm.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace {
const char* const REGNAMES[17] = {"R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9",
                                  "R10", "R11", "R12", "R13", "R14", "R15", "PC"};
RegDescriptor makeReg(int i) {
    if (i < 16) return {REGNAMES[i], RegWidth::R32, (uint8_t)(i == 13 ? REGFLAG_SP : 0), nullptr, nullptr};
    if (i == 16) return {"PC", RegWidth::R32, REGFLAG_PC, nullptr, nullptr};
    return {"PSR", RegWidth::R32, REGFLAG_STATUS, nullptr, nullptr};
}
const RegDescriptor* regTable() {
    static RegDescriptor t[Arm2::R_COUNT];
    static bool made = false;
    if (!made) { for (int i = 0; i < Arm2::R_COUNT; ++i) t[i] = makeReg(i); made = true; }
    return t;
}
inline uint32_t ror32(uint32_t v, int n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
}  // namespace

Arm2::Arm2(bool arm1) : m_arm1(arm1) { reset(); }

void Arm2::reset() {
    setMode(SVC);
    m_psr = FLAG_I | FLAG_F | SVC;
    m_pc = 0;
}

const RegDescriptor* Arm2::regDescriptor(int idx) const {
    return idx >= 0 && idx < R_COUNT ? &regTable()[idx] : nullptr;
}

uint32_t Arm2::regRead(int idx) const {
    if (idx >= 0 && idx < 15) return m_r[idx];
    if (idx == 15) return (m_pc & PC_MASK) | m_psr;
    if (idx == R_PC) return m_pc;
    if (idx == R_PSR) return m_psr;
    return 0;
}

void Arm2::regWrite(int idx, uint32_t v) {
    if (idx >= 0 && idx < 15) m_r[idx] = v;
    else if (idx == 15) { m_pc = v & PC_MASK; setPsr(v, true); }
    else if (idx == R_PC) m_pc = v & PC_MASK;
    else if (idx == R_PSR) setPsr(v, true);
}

void Arm2::saveState(uint8_t* b) const {
    uint32_t w[32];
    int n = 0;
    for (int i = 0; i < 16; ++i) w[n++] = m_r[i];
    w[n++] = m_pc; w[n++] = m_psr;
    for (int i = 0; i < 7; ++i) w[n++] = m_usr[i];
    for (int i = 0; i < 7; ++i) w[n++] = m_fiqBank[i];
    std::memcpy(b, w, (size_t)n * 4);
    uint32_t x[6] = {m_irqBank[0], m_irqBank[1], m_svcBank[0], m_svcBank[1], (uint32_t)(m_irq | m_fiq << 1), 0};
    std::memcpy(b + n * 4, x, 4 * 4);
    (void)x;
}

void Arm2::loadState(const uint8_t* b) {
    uint32_t w[32];
    std::memcpy(w, b, 32 * 4);
    int n = 0;
    for (int i = 0; i < 16; ++i) m_r[i] = w[n++];
    m_pc = w[n++]; m_psr = w[n++];
    for (int i = 0; i < 7; ++i) m_usr[i] = w[n++];
    for (int i = 0; i < 7; ++i) m_fiqBank[i] = w[n++];
    uint32_t x[4];
    std::memcpy(x, b + n * 4, sizeof x);
    m_irqBank[0] = x[0]; m_irqBank[1] = x[1]; m_svcBank[0] = x[2]; m_svcBank[1] = x[3];
}

// ---------------------------------------------------------------------------
// Memory, modes, the PSR
// ---------------------------------------------------------------------------
// Words go to the bus as words (little-endian), so a board can treat word
// accesses to its devices differently from byte ones
uint32_t Arm2::rd32(uint32_t a) { return m_bus ? m_bus->read32(a & 0x03FFFFFC) : 0xFFFFFFFF; }
void Arm2::wr32(uint32_t a, uint32_t v) { if (m_bus) m_bus->write32(a & 0x03FFFFFC, v); }

// Bank the registers of the mode being left, bring in the new mode's
void Arm2::setMode(Mode m) {
    const Mode old = (Mode)(m_psr & 3);
    if (old == m) return;
    if (old == FIQ) { for (int i = 0; i < 7; ++i) m_fiqBank[i] = m_r[8 + i]; }
    else {
        for (int i = 0; i < 5; ++i) m_usr[i] = m_r[8 + i];
        uint32_t* b13 = old == USR ? &m_usr[5] : old == IRQ ? m_irqBank : m_svcBank;
        b13[0] = m_r[13]; b13[1] = m_r[14];
    }
    if (m == FIQ) { for (int i = 0; i < 7; ++i) m_r[8 + i] = m_fiqBank[i]; }
    else {
        for (int i = 0; i < 5; ++i) m_r[8 + i] = m_usr[i];
        const uint32_t* b13 = m == USR ? &m_usr[5] : m == IRQ ? m_irqBank : m_svcBank;
        m_r[13] = b13[0]; m_r[14] = b13[1];
    }
    m_psr = (m_psr & ~3u) | m;
}

// The PSR bits of a value for R15: all of them in a privileged mode; only
// N Z C V in user mode
void Arm2::setPsr(uint32_t v, bool privileged) {
    if (privileged) {
        const uint32_t keep = v & 0xFC000003;
        setMode((Mode)(keep & 3));
        m_psr = keep;
    } else {
        m_psr = (m_psr & 0x0FFFFFFF) | (v & 0xF0000000);
    }
}

void Arm2::exception(uint32_t vector, Mode m, bool maskFiq, uint32_t link) {
    setMode(m);
    m_r[14] = link;
    m_psr |= FLAG_I | (maskFiq ? FLAG_F : 0);
    m_pc = vector;
}

bool Arm2::cond(uint32_t c) const {
    const bool n = m_psr & FLAG_N, z = m_psr & FLAG_Z, cf = m_psr & FLAG_C, v = m_psr & FLAG_V;
    switch (c) {
        case 0x0: return z;            case 0x1: return !z;
        case 0x2: return cf;           case 0x3: return !cf;
        case 0x4: return n;            case 0x5: return !n;
        case 0x6: return v;            case 0x7: return !v;
        case 0x8: return cf && !z;     case 0x9: return !cf || z;
        case 0xA: return n == v;       case 0xB: return n != v;
        case 0xC: return !z && n == v; case 0xD: return z || n != v;
        case 0xE: return true;         default: return false;     // NV
    }
}

// The barrel shifter: operand 2 of a data processing instruction (or the
// shifted-register offset of LDR/STR), its carry out
uint32_t Arm2::shifter(uint32_t op, bool& carry, int& cycles) {
    carry = m_psr & FLAG_C;
    if (op & (1u << 25)) {                                  // immediate, rotated by twice the field
        const int rot = (int)((op >> 8) & 15) * 2;
        const uint32_t v = ror32(op & 0xFF, rot);
        if (rot) carry = v >> 31;
        return v;
    }
    const int type = (op >> 5) & 3;
    int amount;
    uint32_t v;
    if (op & 0x10) {                                        // by a register: R15 reads 12 ahead
        amount = (int)(m_r[(op >> 8) & 15] & 0xFF);
        if (((op >> 8) & 15) == 15) amount = (int)(r15(4) & 0xFF);
        v = regOp(op & 15, 4);
        cycles += 1;
        if (amount == 0) return v;
        switch (type) {
            case 0:                                         // LSL
                if (amount < 32) { carry = (v >> (32 - amount)) & 1; return v << amount; }
                carry = amount == 32 ? (v & 1) : 0; return 0;
            case 1:                                         // LSR
                if (amount < 32) { carry = (v >> (amount - 1)) & 1; return v >> amount; }
                carry = amount == 32 ? (v >> 31) : 0; return 0;
            case 2:                                         // ASR
                if (amount < 32) { carry = (v >> (amount - 1)) & 1; return (uint32_t)((int32_t)v >> amount); }
                carry = v >> 31; return carry ? 0xFFFFFFFF : 0;
            default:                                        // ROR
                if ((amount & 31) == 0) { carry = v >> 31; return v; }
                carry = (v >> ((amount & 31) - 1)) & 1;
                return ror32(v, amount);
        }
    }
    amount = (int)((op >> 7) & 31);
    v = regOp(op & 15, 0);
    switch (type) {
        case 0:                                             // LSL #n (#0: none)
            if (!amount) return v;
            carry = (v >> (32 - amount)) & 1;
            return v << amount;
        case 1:                                             // LSR #n (#0 is #32)
            if (!amount) { carry = v >> 31; return 0; }
            carry = (v >> (amount - 1)) & 1;
            return v >> amount;
        case 2:                                             // ASR #n (#0 is #32)
            if (!amount) { carry = v >> 31; return carry ? 0xFFFFFFFF : 0; }
            carry = (v >> (amount - 1)) & 1;
            return (uint32_t)((int32_t)v >> amount);
        default:                                            // ROR #n (#0 is RRX)
            if (!amount) {
                carry = v & 1;
                return (v >> 1) | ((m_psr & FLAG_C) ? 0x80000000u : 0);
            }
            carry = (v >> (amount - 1)) & 1;
            return ror32(v, amount);
    }
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------
int Arm2::step() {
    if (m_pcHook && m_pcHook->armed(m_pc) && m_pcHook->fire(this, m_pc)) {
        m_cycles += 1;
        return 1;
    }
    // Interrupts between instructions: the link is the next instruction + 4
    if (m_fiq && !(m_psr & FLAG_F)) {
        exception(0x1C, FIQ, true, ((m_pc + 4) & PC_MASK) | m_psr);
        m_cycles += 3;
        return 3;
    }
    if (m_irq && !(m_psr & FLAG_I)) {
        exception(0x18, IRQ, false, ((m_pc + 4) & PC_MASK) | m_psr);
        m_cycles += 3;
        return 3;
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
    const uint32_t op = rd32(m_pc);
    m_pc = (m_pc + 4) & PC_MASK;
    int c = 1;
    if (cond(op >> 28)) {
        switch ((op >> 25) & 7) {
            case 0:
                if ((op & 0x0FC000F0) == 0x00000090 && !m_arm1) { c = multiply(op); break; }
                if ((op & 0x90) == 0x90) {                          // the multiply space otherwise: undefined
                    exception(0x04, SVC, false, m_pc | m_psr);
                    c = 3;
                    break;
                }
                c = dataProcessing(op);
                break;
            case 1: c = dataProcessing(op); break;
            case 2: c = singleTransfer(op); break;
            case 3:
                if (op & 0x10) { exception(0x04, SVC, false, m_pc | m_psr); c = 3; break; }   // undefined
                c = singleTransfer(op);
                break;
            case 4: c = blockTransfer(op); break;
            case 5: {                                               // B, BL
                int32_t off = (int32_t)(op << 8) >> 6;
                if (op & (1u << 24)) m_r[14] = (m_pc & PC_MASK) | m_psr;
                m_pc = (uint32_t)(m_pc + 4 + off) & PC_MASK;
                c = 3;
                break;
            }
            case 6: exception(0x04, SVC, false, m_pc | m_psr); c = 3; break;   // coprocessor: none fitted
            default:
                if (op & (1u << 24)) exception(0x08, SVC, false, m_pc | m_psr);   // SWI
                else exception(0x04, SVC, false, m_pc | m_psr);                    // coprocessor
                c = 3;
                break;
        }
    }
    m_cycles += (uint64_t)c;
    return c;
}

int Arm2::dataProcessing(uint32_t op) {
    int cycles = 1;
    bool sc;
    const uint32_t b = shifter(op, sc, cycles);
    const int opc = (op >> 21) & 15, rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    const bool s = op & (1u << 20);
    // Rn: R15 is the PC alone (8 ahead, 12 with a register shift)
    const uint32_t a = rn == 15 ? ((m_pc + 4 + ((op & 0x02000010) == 0x10 ? 4 : 0)) & PC_MASK) : m_r[rn];
    uint64_t r = 0;
    bool logical = true, carry = sc, overflow = (m_psr & FLAG_V) != 0;
    auto addc = [&](uint32_t x, uint32_t y, uint32_t cin) {
        const uint64_t t = (uint64_t)x + y + cin;
        carry = t >> 32;
        overflow = ((x ^ ~y) & (x ^ (uint32_t)t)) >> 31;
        logical = false;
        return (uint32_t)t;
    };
    const uint32_t cin = (m_psr & FLAG_C) ? 1 : 0;
    switch (opc) {
        case 0x0: case 0x8: r = a & b; break;                           // AND, TST
        case 0x1: case 0x9: r = a ^ b; break;                           // EOR, TEQ
        case 0x2: case 0xA: r = addc(a, ~b, 1); break;                  // SUB, CMP
        case 0x3: r = addc(b, ~a, 1); break;                            // RSB
        case 0x4: case 0xB: r = addc(a, b, 0); break;                   // ADD, CMN
        case 0x5: r = addc(a, b, cin); break;                           // ADC
        case 0x6: r = addc(a, ~b, cin); break;                          // SBC
        case 0x7: r = addc(b, ~a, cin); break;                          // RSC
        case 0xC: r = a | b; break;                                     // ORR
        case 0xD: r = b; break;                                         // MOV
        case 0xE: r = a & ~b; break;                                    // BIC
        default: r = ~b; break;                                         // MVN
    }
    const uint32_t res = (uint32_t)r;
    const bool test = opc >= 0x8 && opc <= 0xB;
    const bool priv = (m_psr & 3) != USR;
    if (rd == 15) {
        if (test) {                                                     // TEQP etc.: the PSR from the result
            if (s) setPsr(res, priv);
            return cycles;
        }
        m_pc = res & PC_MASK;
        if (s) setPsr(res, priv);                                       // MOVS PC: PC and PSR
        return cycles + 2;
    }
    if (s) {
        uint32_t f = m_psr & 0x0FFFFFFF;
        if (res & 0x80000000) f |= FLAG_N;
        if (!res) f |= FLAG_Z;
        if (carry) f |= FLAG_C;
        if (logical ? (m_psr & FLAG_V) : overflow) f |= FLAG_V;
        m_psr = f;
    }
    if (!test) m_r[rd] = res;
    return cycles;
}

int Arm2::multiply(uint32_t op) {
    const int rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
    const uint32_t vs = rs == 15 ? r15(0) : m_r[rs];
    uint32_t res = (rm == 15 ? r15(0) : m_r[rm]) * vs;
    if (op & (1u << 21)) res += rn == 15 ? r15(0) : m_r[rn];          // MLA
    if (rd == rm) res = 0;                                             // Rd = Rm (unpredictable): 0, as b-em/Arculator
    if (rd != 15) m_r[rd] = res;
    if (op & (1u << 20)) {
        m_psr &= ~(FLAG_N | FLAG_Z);
        if (res & 0x80000000) m_psr |= FLAG_N;
        if (!res) m_psr |= FLAG_Z;
    }
    // Two bits of Rs a cycle (Booth's algorithm), up to 16
    int c = 1;
    for (uint32_t v = vs; v && c < 17; v >>= 2) ++c;
    return c;
}

int Arm2::singleTransfer(uint32_t op) {
    const bool pre = op & (1u << 24), up = op & (1u << 23), byte = op & (1u << 22);
    const bool wb = op & (1u << 21), load = op & (1u << 20);
    const int rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    uint32_t off;
    if (op & (1u << 25)) { bool c; int cy = 0; off = shifter(op & ~(1u << 25) & ~0x10u, c, cy); }   // shifted register
    else off = op & 0xFFF;
    const uint32_t base = rn == 15 ? ((m_pc + 4) & PC_MASK) : m_r[rn];
    const uint32_t moved = up ? base + off : base - off;
    const uint32_t addr = pre ? moved : base;
    if (addr > 0x03FFFFFF) {                                            // address exception
        exception(0x14, SVC, false, m_pc | m_psr);
        return 3;
    }
    const uint32_t store = rd == 15 ? r15(4) : m_r[rd];                // STR R15: 12 ahead, with the PSR
    if (!pre || wb) { if (rn != 15) m_r[rn] = moved; }                  // writeback (post-indexed: always)
    if (load) {
        uint32_t v;
        if (byte) v = rd8(addr);
        else v = ror32(rd32(addr), (int)(addr & 3) * 8);                // an unaligned word rotates
        if (rd == 15) { m_pc = v & PC_MASK; return 5; }
        m_r[rd] = v;
        return 3;
    }
    if (byte) wr8(addr, store & 0xFF);
    else wr32(addr, store);
    return 2;
}

int Arm2::blockTransfer(uint32_t op) {
    const bool pre = op & (1u << 24), up = op & (1u << 23), psrForce = op & (1u << 22);
    const bool wb = op & (1u << 21), load = op & (1u << 20);
    const int rn = (op >> 16) & 15;
    const uint16_t list = op & 0xFFFF;
    int n = 0;
    for (int i = 0; i < 16; ++i) if (list & (1 << i)) ++n;
    const uint32_t base = rn == 15 ? ((m_pc + 4) & PC_MASK) : m_r[rn];
    uint32_t addr = up ? base + (pre ? 4 : 0) : base - 4u * (uint32_t)n + (pre ? 0 : 4);
    const uint32_t newBase = up ? base + 4u * (uint32_t)n : base - 4u * (uint32_t)n;
    const bool priv = (m_psr & 3) != USR;
    // ^ without R15 in an LDM, or in any STM: the user bank (in a privileged mode)
    const bool userBank = psrForce && priv && (!load || !(list & 0x8000));
    const Mode was = (Mode)(m_psr & 3);
    if (n == 0) return 2;
    if (addr > 0x03FFFFFF) { exception(0x14, SVC, false, m_pc | m_psr); return 3; }
    // The base is written back in the current mode's bank (after the first
    // transfer: an STM stores the original base only if it comes first);
    // ^ transfers the user bank's registers; a stored R15 carries the real PSR
    const uint32_t pcWithPsr = r15(4);
    bool first = true;
    for (int i = 0; i < 16; ++i) {
        if (!(list & (1 << i))) continue;
        if (load) {
            const uint32_t v = rd32(addr);
            if (first && wb && rn != 15) m_r[rn] = newBase;
            if (i == 15) {
                m_pc = v & PC_MASK;
                if (psrForce) setPsr(v, (was != USR));                  // LDM ^ with R15: the PSR too
            } else if (userBank && i >= 8 && i <= 14 && was != USR) {
                // the user bank: R8-R12 are shared outside FIQ; R13-R14 always banked
                if (was == FIQ) m_usr[i - 8] = v;
                else if (i >= 13) m_usr[i - 8] = v;
                else m_r[i] = v;
            } else {
                m_r[i] = v;
            }
        } else {
            uint32_t v;
            if (i == 15) v = pcWithPsr;
            else if (userBank && i >= 8 && i <= 14 && was != USR && (was == FIQ || i >= 13)) v = m_usr[i - 8];
            else v = m_r[i];
            wr32(addr, v);
            if (first && wb && rn != 15) m_r[rn] = newBase;
        }
        first = false;
        addr += 4;
    }
    if (load) return n + 2 + ((list & 0x8000) ? 2 : 0);
    return n + 1;
}

// ---------------------------------------------------------------------------
// Disassembler (Acorn assembler syntax)
// ---------------------------------------------------------------------------
namespace {
const char* const COND[16] = {"EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC",
                              "HI", "LS", "GE", "LT", "GT", "LE", "", "NV"};
const char* const DP[16] = {"AND", "EOR", "SUB", "RSB", "ADD", "ADC", "SBC", "RSC",
                            "TST", "TEQ", "CMP", "CMN", "ORR", "MOV", "BIC", "MVN"};
const char* const SHIFT[4] = {"LSL", "LSR", "ASR", "ROR"};
std::string rn(int r) { return r == 15 ? "PC" : r == 14 ? "R14" : r == 13 ? "R13" : "R" + std::to_string(r); }
std::string hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "&%X", v); return b; }
std::string op2(uint32_t op) {
    if (op & (1u << 25)) return "#" + hex(ror32(op & 0xFF, (int)((op >> 8) & 15) * 2));
    std::string s = rn(op & 15);
    const int type = (op >> 5) & 3;
    if (op & 0x10) return s + "," + SHIFT[type] + " " + rn((op >> 8) & 15);
    const int amt = (op >> 7) & 31;
    if (!amt && type == 0) return s;
    if (!amt && type == 3) return s + ",RRX";
    return s + "," + SHIFT[type] + " #" + std::to_string(amt ? amt : 32);
}
}  // namespace

int Arm2::disassembleOne(IBus* bus, uint32_t addr, char* buf, int bufsz) {
    uint32_t op = 0;
    for (int i = 3; i >= 0; --i) op = op << 8 | (bus ? bus->peek8((addr + (uint32_t)i) & 0x03FFFFFF) : 0);
    const std::string c = COND[op >> 28];
    std::string s;
    const int cls = (op >> 25) & 7;
    if (cls <= 1) {
        if ((op & 0x0FC000F0) == 0x90 && !m_arm1) {
            const bool acc = op & (1u << 21);
            s = std::string(acc ? "MLA" : "MUL") + c + ((op & (1u << 20)) ? "S" : "") + " " + rn((op >> 16) & 15) + "," +
                rn(op & 15) + "," + rn((op >> 8) & 15) + (acc ? "," + rn((op >> 12) & 15) : "");
        } else if (cls == 0 && (op & 0x90) == 0x90) {
            s = "UNDEF";
        } else {
            const int o = (op >> 21) & 15, d = (op >> 12) & 15, n = (op >> 16) & 15;
            const bool test = o >= 8 && o <= 11, st = op & (1u << 20);
            std::string m = std::string(DP[o]) + c + (st && !test ? "S" : "") + (test && d == 15 ? "P" : "");
            if (test) s = m + " " + rn(n) + "," + op2(op);
            else if (o == 0xD || o == 0xF) s = m + " " + rn(d) + "," + op2(op);
            else s = m + " " + rn(d) + "," + rn(n) + "," + op2(op);
        }
    } else if (cls == 2 || cls == 3) {
        if (cls == 3 && (op & 0x10)) s = "UNDEF";
        else {
            const bool pre = op & (1u << 24), up = op & (1u << 23), wb = op & (1u << 21);
            std::string m = std::string((op & (1u << 20)) ? "LDR" : "STR") + c + ((op & (1u << 22)) ? "B" : "");
            std::string o;
            if (op & (1u << 25)) o = std::string(up ? "" : "-") + op2(op & ~(1u << 25) & ~0x10u);
            else o = "#" + std::string(up ? "" : "-") + hex(op & 0xFFF);
            const std::string b = rn((op >> 16) & 15);
            s = m + " " + rn((op >> 12) & 15) + "," + (pre ? "[" + b + "," + o + "]" + (wb ? "!" : "") : "[" + b + "]," + o);
        }
    } else if (cls == 4) {
        static const char* M[4] = {"DA", "IA", "DB", "IB"};
        std::string regs;
        for (int i = 0; i < 16; ++i) if (op & (1u << i)) regs += (regs.empty() ? "" : ",") + rn(i);
        s = std::string((op & (1u << 20)) ? "LDM" : "STM") + c + M[(op >> 23) & 3] + " " + rn((op >> 16) & 15) +
            ((op & (1u << 21)) ? "!" : "") + ",{" + regs + "}" + ((op & (1u << 22)) ? "^" : "");
    } else if (cls == 5) {
        const int32_t off = (int32_t)(op << 8) >> 6;
        s = std::string((op & (1u << 24)) ? "BL" : "B") + c + " " + hex((addr + 8 + (uint32_t)off) & PC_MASK);
    } else if (cls == 7 && (op & (1u << 24))) {
        s = "SWI" + c + " " + hex(op & 0xFFFFFF);
    } else {
        s = "CDP/LDC/STC (undefined)";
    }
    std::snprintf(buf, (size_t)bufsz, "%s", s.c_str());
    return 4;
}

int Arm2::disassembleEntry(IBus* bus, uint32_t addr, void* entryOut) {
    auto* e = static_cast<DisasmEntry*>(entryOut);
    char buf[96];
    e->addr = addr;
    e->bytes = disassembleOne(bus, addr, buf, sizeof buf);
    std::string s(buf);
    const auto sp = s.find(' ');
    e->mnemonic = sp == std::string::npos ? s : s.substr(0, sp);
    e->operands = sp == std::string::npos ? "" : s.substr(sp + 1);
    e->complete = s;
    e->isCall = isCallAt(bus, addr) > 0;
    e->isReturn = isReturnAt(bus, addr);
    e->isBranch = !e->mnemonic.empty() && e->mnemonic[0] == 'B' && e->mnemonic.rfind("BIC", 0) != 0 && !e->isCall;
    e->isIllegal = s.rfind("UNDEF", 0) == 0;
    e->targetAddr = 0;
    e->rawLen = 4;
    for (int i = 0; i < 4; ++i) e->raw[i] = bus ? bus->peek8((addr + (uint32_t)i) & 0x03FFFFFF) : 0;
    return e->bytes;
}

int Arm2::isCallAt(IBus* bus, uint32_t addr) {
    if (!bus) return 0;
    uint32_t op = 0;
    for (int i = 3; i >= 0; --i) op = op << 8 | bus->peek8((addr + (uint32_t)i) & 0x03FFFFFF);
    if ((op & 0x0F000000) == 0x0B000000) return 4;          // BL
    if ((op & 0x0F000000) == 0x0F000000) return 4;          // SWI
    return 0;
}

bool Arm2::isReturnAt(IBus* bus, uint32_t addr) {
    if (!bus) return false;
    uint32_t op = 0;
    for (int i = 3; i >= 0; --i) op = op << 8 | bus->peek8((addr + (uint32_t)i) & 0x03FFFFFF);
    if ((op & 0x0FEFFFFF) == 0x01A0F00E) return true;       // MOV(S) PC,R14
    if ((op & 0x0E108000) == 0x08108000) return true;       // LDM with PC
    return false;
}

bool Arm2::isProgramEnd(IBus* bus) {
    if (!bus) return false;
    uint32_t op = 0;
    for (int i = 3; i >= 0; --i) op = op << 8 | bus->peek8((m_pc + (uint32_t)i) & 0x03FFFFFF);
    return op == 0xEAFFFFFE;                                // B . (a loop on itself)
}

int Arm2::writeCallHarness(IBus* bus, uint32_t scratch, uint32_t routineAddr) {
    // BL routine ; B .
    const int32_t off = ((int32_t)routineAddr - (int32_t)(scratch + 8)) >> 2;
    const uint32_t bl = 0xEB000000 | ((uint32_t)off & 0xFFFFFF);
    const uint32_t loop = 0xEAFFFFFE;
    for (int i = 0; i < 4; ++i) { bus->write8(scratch + (uint32_t)i, (bl >> (8 * i)) & 0xFF); bus->write8(scratch + 4 + (uint32_t)i, (loop >> (8 * i)) & 0xFF); }
    return 8;
}
