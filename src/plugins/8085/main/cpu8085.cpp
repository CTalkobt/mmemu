// Intel 8085A core. Decoding by the opcode's fields: op = x(7-6) y(5-3)
// z(2-0), y = p(5-4) q(3).
//
// Sources: the Intel 8085AH data sheet and the MCS-80/85 Family User's Manual
// (instruction set, T-states, interrupts, RIM/SIM, the AC difference from
// the 8080); Dehnhardt and Sorensen, "Unspecified 8085 op codes enhance
// programming" (Electronics, 1979) for the undocumented instructions; Ken
// Shirriff's die-level analysis of the flag circuits (righto.com, 2013) for V
// and K; MAME's i8085.cpp (DSUB as revised in 2025, the RIM-after-TRAP IE),
// VirtualT's and Alan Cox's V85 8085 cores for comparison. The code is our own.

#include "cpu8085.h"
#include "opcodes8085.h"
#include "libdebug/main/execution_observer.h"
#include "libcore/main/intercept.h"
#include "libtoolchain/main/idisasm.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

inline uint8_t szp(uint8_t v) {
    uint8_t p = v;
    p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
    return (uint8_t)((v & I8085::FS) | (v ? 0 : I8085::FZ) | ((p & 1) ? 0 : I8085::FP));
}

/** V (signed overflow) and K (V xor the sign) for the 8-bit sum a + b + c. */
inline uint8_t vk(uint8_t a, uint8_t b, int c, uint8_t res) {
    const bool c6 = (((a & 0x7F) + (b & 0x7F) + c) & 0x80) != 0;
    const bool c7 = (a + b + c) > 0xFF;
    const bool v = c6 != c7;
    const bool k = v != ((res & 0x80) != 0);
    return (uint8_t)((v ? I8085::FV : 0) | (k ? I8085::FK : 0));
}

const RegDescriptor REGS[I8085::R_COUNT] = {
    {"AF", RegWidth::R16, REGFLAG_STATUS | REGFLAG_ACCUMULATOR, nullptr, "SZKA-PVC"},
    {"BC", RegWidth::R16, 0, nullptr, nullptr},
    {"DE", RegWidth::R16, 0, nullptr, nullptr},
    {"HL", RegWidth::R16, 0, nullptr, nullptr},
    {"SP", RegWidth::R16, REGFLAG_SP, nullptr, nullptr},
    {"PC", RegWidth::R16, REGFLAG_PC, nullptr, nullptr},
    // The interrupt masks, IE and pending interrupts as RIM reads them
    {"IM", RegWidth::R8, 0, nullptr, nullptr},
};
}  // namespace

I8085::I8085() { reset(); }

void I8085::reset() {
    // RESET IN: PC 0, IE off, the RST masks set, the 7.5 latch and SOD clear
    m_pc = 0;
    m_ie = false;
    m_eiDelay = false;
    m_halted = false;
    m_mask = IM_M55 | IM_M65 | IM_M75;
    m_i75 = false;
    m_trapPending = false;
    m_trapIe = -1;
    setSod(false);
}

const RegDescriptor* I8085::regDescriptor(int idx) const {
    return idx >= 0 && idx < R_COUNT ? &REGS[idx] : nullptr;
}

uint8_t I8085::rimValue() const {
    uint8_t v = m_mask;
    if (m_ie) v |= IM_IE;
    if (m_rst55) v |= IM_I55;
    if (m_rst65) v |= IM_I65;
    if (m_i75) v |= IM_I75;
    if (m_sidFn ? m_sidFn() : m_sid) v |= IM_SID;
    return v;
}

uint32_t I8085::regRead(int idx) const {
    switch (idx) {
        case R_AF: return (uint32_t)(m_a << 8 | m_f);
        case R_BC: return bc();
        case R_DE: return de();
        case R_HL: return hl();
        case R_SP: return m_sp;
        case R_PC: return m_pc;
        case R_IM: return rimValue();
    }
    return 0;
}

void I8085::regWrite(int idx, uint32_t v) {
    switch (idx) {
        case R_AF: m_a = (uint8_t)(v >> 8); m_f = (uint8_t)(v & 0xF7); break;
        case R_BC: m_b = (uint8_t)(v >> 8); m_c = (uint8_t)v; break;
        case R_DE: m_d = (uint8_t)(v >> 8); m_e = (uint8_t)v; break;
        case R_HL: m_h = (uint8_t)(v >> 8); m_l = (uint8_t)v; break;
        case R_SP: m_sp = (uint16_t)v; break;
        case R_PC: m_pc = (uint16_t)v; m_halted = false; break;
        case R_IM:                              // the masks, IE and the 7.5 latch
            m_mask = (uint8_t)(v & 7);
            m_ie = (v & IM_IE) != 0;
            m_i75 = (v & IM_I75) != 0;
            break;
    }
}

void I8085::saveState(uint8_t* b) const {
    uint16_t w[] = {(uint16_t)(m_a << 8 | m_f), bc(), de(), hl(), m_sp, m_pc};
    std::memcpy(b, w, sizeof w);
    b += sizeof w;
    b[0] = m_mask;
    b[1] = (uint8_t)((m_ie ? 1 : 0) | (m_eiDelay ? 2 : 0) | (m_halted ? 4 : 0) | (m_i75 ? 8 : 0) |
                     (m_trapPending ? 16 : 0) | (m_sod ? 32 : 0) | (m_trapIe >= 0 ? 64 : 0) | (m_trapIe > 0 ? 128 : 0));
    b[2] = (uint8_t)((m_intr ? 1 : 0) | (m_rst55 ? 2 : 0) | (m_rst65 ? 4 : 0) | (m_rst75 ? 8 : 0) |
                     (m_trap ? 16 : 0) | (m_sid ? 32 : 0));
    std::memcpy(b + 4, &m_cycles, sizeof m_cycles);
}

void I8085::loadState(const uint8_t* b) {
    uint16_t w[6];
    std::memcpy(w, b, sizeof w);
    b += sizeof w;
    regWrite(R_AF, w[0]); regWrite(R_BC, w[1]); regWrite(R_DE, w[2]); regWrite(R_HL, w[3]);
    m_sp = w[4]; m_pc = w[5];
    m_mask = b[0] & 7;
    m_ie = b[1] & 1; m_eiDelay = b[1] & 2; m_halted = b[1] & 4; m_i75 = b[1] & 8;
    m_trapPending = b[1] & 16; m_sod = b[1] & 32;
    m_trapIe = (b[1] & 64) ? ((b[1] & 128) ? 1 : 0) : -1;
    m_intr = b[2] & 1; m_rst55 = b[2] & 2; m_rst65 = b[2] & 4; m_rst75 = b[2] & 8;
    m_trap = b[2] & 16; m_sid = b[2] & 32;
    std::memcpy(&m_cycles, b + 4, sizeof m_cycles);
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

uint8_t I8085::reg8(int r) {
    switch (r) {
        case 0: return m_b;
        case 1: return m_c;
        case 2: return m_d;
        case 3: return m_e;
        case 4: return m_h;
        case 5: return m_l;
        case 6: return rd(hl());
        default: return m_a;
    }
}

void I8085::setReg8(int r, uint8_t v) {
    switch (r) {
        case 0: m_b = v; break;
        case 1: m_c = v; break;
        case 2: m_d = v; break;
        case 3: m_e = v; break;
        case 4: m_h = v; break;
        case 5: m_l = v; break;
        case 6: wr(hl(), v); break;
        default: m_a = v; break;
    }
}

uint16_t I8085::rp(int p) const {
    switch (p) {
        case 0: return bc();
        case 1: return de();
        case 2: return hl();
        default: return m_sp;
    }
}

void I8085::setRp(int p, uint16_t v) {
    switch (p) {
        case 0: m_b = v >> 8; m_c = v & 0xFF; break;
        case 1: setDe(v); break;
        case 2: setHl(v); break;
        default: m_sp = v; break;
    }
}

bool I8085::cond(int y) const {
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

// ADD, ADC: AC is the carry out of bit 3
void I8085::add(uint8_t v, int carry) {
    const int r = m_a + v + carry;
    const uint8_t res = (uint8_t)r;
    m_f = (uint8_t)(szp(res) | (r > 0xFF ? FC : 0) | ((m_a ^ v ^ res) & FA) | vk(m_a, v, carry, res));
    m_a = res;
}

// SUB, SBB, CMP: the ALU adds the complement, A + ~v + !borrow; CY is the
// borrow (no carry out), AC the carry out of bit 3 of that addition (as on
// the 8080), V and K from it (K: A < v as signed numbers)
uint8_t I8085::sub(uint8_t v, int borrow) {
    const uint8_t nv = (uint8_t)~v;
    const int cin = borrow ? 0 : 1;
    const int r = m_a + nv + cin;
    const uint8_t res = (uint8_t)r;
    m_f = (uint8_t)(szp(res) | (r > 0xFF ? 0 : FC) | ((m_a ^ nv ^ res) & FA) | vk(m_a, nv, cin, res));
    return res;
}

// ANA ORA XRA: CY and V clear, K the sign; AC set for ANA (the 8085's), else clear
void I8085::logic(uint8_t res, bool ac) {
    m_a = res;
    m_f = (uint8_t)(szp(res) | (ac ? FA : 0) | ((res & 0x80) ? FK : 0));
}

uint8_t I8085::inr(uint8_t v) {
    const uint8_t r = (uint8_t)(v + 1);
    const bool ov = v == 0x7F;
    m_f = (uint8_t)((m_f & FC) | szp(r) | ((r & 0x0F) == 0 ? FA : 0) | (ov ? FV : 0) |
                    ((ov != ((r & 0x80) != 0)) ? FK : 0));
    return r;
}

// DCR adds $FF: AC is that addition's carry out of bit 3 (set unless the low nibble was 0)
uint8_t I8085::dcr(uint8_t v) {
    const uint8_t r = (uint8_t)(v - 1);
    const bool ov = v == 0x80;
    m_f = (uint8_t)((m_f & FC) | szp(r) | ((v & 0x0F) != 0 ? FA : 0) | (ov ? FV : 0) |
                    ((ov != ((r & 0x80) != 0)) ? FK : 0));
    return r;
}

// DAA: adds 6 and/or $60; flags as for that addition, CY only ever set
void I8085::daa() {
    uint8_t corr = 0;
    bool cy = m_f & FC;
    if ((m_f & FA) || (m_a & 0x0F) > 9) corr |= 0x06;
    if (cy || m_a > 0x99) { corr |= 0x60; cy = true; }
    const uint8_t res = (uint8_t)(m_a + corr);
    m_f = (uint8_t)(szp(res) | (cy ? FC : 0) | ((m_a ^ corr ^ res) & FA) | vk(m_a, corr, 0, res));
    m_a = res;
}

// DAD: CY from bit 15; V the 16-bit signed overflow, K V xor bit 15 (S Z AC P kept)
void I8085::dad(uint16_t v) {
    const uint16_t h = hl();
    const uint32_t s = (uint32_t)h + v;
    const uint16_t res = (uint16_t)s;
    const bool c14 = (((h & 0x7FFF) + (v & 0x7FFF)) & 0x8000) != 0;
    const bool ov = c14 != (s > 0xFFFF);
    const bool k = ov != ((res & 0x8000) != 0);
    m_f = (uint8_t)((m_f & (FS | FZ | FA | FP)) | (s > 0xFFFF ? FC : 0) | (ov ? FV : 0) | (k ? FK : 0));
    setHl(res);
}

// DSUB: HL - BC as SUB L,C then SBB H,B; the flags from the high byte's
// subtraction, except Z for the whole 16-bit result (MAME, 2025)
void I8085::dsub() {
    const int lo = m_l - m_c;
    const uint8_t a = m_a;
    m_a = m_h;
    const uint8_t hi = sub(m_b, lo < 0 ? 1 : 0);
    m_a = a;
    m_l = (uint8_t)lo;
    m_h = hi;
    m_f = (uint8_t)((m_f & ~FZ) | ((m_h | m_l) ? 0 : FZ));
}

// ---------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------

// TRAP and RST n.5 take as long as an RST (a 6-state M1 and two writes)
int I8085::takeInterrupt(Irq src, uint16_t vector) {
    m_halted = false;
    m_ie = false;
    if (m_ack) m_ack(src);
    push(m_pc);
    m_pc = vector;
    return 12;
}

int I8085::checkInterrupts() {
    if (m_trapPending) {
        m_trapPending = false;
        m_trapIe = m_ie ? 1 : 0;
        return takeInterrupt(Irq::Trap, 0x0024);
    }
    if (!m_ie || m_eiDelay) return 0;
    if (m_i75 && !(m_mask & IM_M75)) {
        m_i75 = false;
        return takeInterrupt(Irq::Rst75, 0x003C);
    }
    if (m_rst65 && !(m_mask & IM_M65)) return takeInterrupt(Irq::Rst65, 0x0034);
    if (m_rst55 && !(m_mask & IM_M55)) return takeInterrupt(Irq::Rst55, 0x002C);
    if (!m_intr) return 0;

    // INTR: the instruction comes from the data bus in INTA cycles
    m_halted = false;
    m_ie = false;
    if (m_ack) m_ack(Irq::Intr);
    auto inta = [this]() -> uint8_t { return m_inta ? m_inta() : 0xFF; };
    const uint8_t op = inta();
    if ((op & 0xC7) == 0xC7) {                               // RST n: 12
        push(m_pc);
        m_pc = (uint16_t)(op & 0x38);
        return 12;
    }
    if (op == 0xCD) {                                        // CALL nn: 18
        const uint8_t lo = inta();
        const uint8_t hi = inta();
        push(m_pc);
        m_pc = (uint16_t)(hi << 8 | lo);
        return 18;
    }
    // Any other one-byte instruction runs as it is (its operands, if any, from memory)
    return exec(op);
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

int I8085::step() {
    if (m_pcHook && m_pcHook->armed(m_pc) && m_pcHook->fire(this, m_pc)) {
        m_cycles += 4;
        return 4;
    }
    // Interrupts are taken between instructions (the maskable ones not right after EI)
    if (int t = checkInterrupts()) {
        m_eiDelay = false;
        m_cycles += t;
        return t;
    }
    m_eiDelay = false;
    if (m_halted) {
        m_cycles += 4;
        return 4;
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

    const int t = exec(fetch());
    m_cycles += t;
    return t;
}

// One instruction (the opcode fetched); its T-states (8085 data sheet)
int I8085::exec(uint8_t op) {
    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    switch (x) {
    case 0:
        switch (z) {
        case 0:
            switch (y) {
            case 0: return 4;                                         // NOP
            case 1: dsub(); return 10;                                // DSUB
            case 2:                                                   // ARHL: HL >> 1 arithmetic, CY = bit 0
                m_f = (uint8_t)((m_f & ~FC) | (m_l & 1));
                setHl((uint16_t)((hl() & 0x8000) | (hl() >> 1)));
                return 7;
            case 3: {                                                 // RDEL: DE << 1 through CY; V = DE's overflow
                const uint16_t d = de();
                const bool ov = ((d >> 15) ^ (d >> 14)) & 1;
                setDe((uint16_t)(d << 1 | (m_f & FC)));
                m_f = (uint8_t)((m_f & ~(FC | FV)) | (d >> 15) | (ov ? FV : 0));
                return 10;
            }
            case 4: {                                                 // RIM
                uint8_t v = rimValue();
                if (m_trapIe >= 0) v = (uint8_t)((v & ~IM_IE) | (m_trapIe ? IM_IE : 0));
                m_trapIe = -1;
                m_a = v;
                return 4;
            }
            case 5: setDe((uint16_t)(hl() + fetch())); return 10;     // LDHI n
            case 6:                                                   // SIM
                if (m_a & 0x08) m_mask = m_a & 7;                     // MSE: new masks
                if (m_a & 0x10) m_i75 = false;                        // R7.5
                if (m_a & 0x40) setSod((m_a & 0x80) != 0);            // SOE: SOD
                return 4;
            default: setDe((uint16_t)(m_sp + fetch())); return 10;    // LDSI n
            }
        case 1:
            if (!q) { setRp(p, fetch16()); return 10; }               // LXI rp,nn
            dad(rp(p));                                               // DAD rp
            return 10;
        case 2:
            switch (y) {
            case 0: wr(bc(), m_a); return 7;                          // STAX B
            case 1: m_a = rd(bc()); return 7;                         // LDAX B
            case 2: wr(de(), m_a); return 7;                          // STAX D
            case 3: m_a = rd(de()); return 7;                         // LDAX D
            case 4: wr16(fetch16(), hl()); return 16;                 // SHLD nn
            case 5: setHl(rd16(fetch16())); return 16;                // LHLD nn
            case 6: wr(fetch16(), m_a); return 13;                    // STA nn
            default: m_a = rd(fetch16()); return 13;                  // LDA nn
            }
        case 3: {                                                     // INX / DCX: K = the incrementer's carry
            const uint16_t v = (uint16_t)(rp(p) + (q ? -1 : 1));
            setRp(p, v);
            const bool k = q ? v == 0xFFFF : v == 0x0000;
            m_f = (uint8_t)((m_f & ~FK) | (k ? FK : 0));
            return 6;
        }
        case 4: setReg8(y, inr(reg8(y))); return y == 6 ? 10 : 4;     // INR r
        case 5: setReg8(y, dcr(reg8(y))); return y == 6 ? 10 : 4;     // DCR r
        case 6: setReg8(y, fetch()); return y == 6 ? 10 : 7;          // MVI r,n
        default:
            switch (y) {
            case 0: {                                                 // RLC: V, K as A + A
                const uint8_t a = m_a;
                m_a = (uint8_t)(a << 1 | a >> 7);
                m_f = (uint8_t)((m_f & ~(FC | FV | FK)) | (a >> 7) | vk(a, a, 0, m_a));
                return 4;
            }
            case 1:                                                   // RRC: V clear
                m_f = (uint8_t)((m_f & ~(FC | FV)) | (m_a & 1));
                m_a = (uint8_t)(m_a >> 1 | m_a << 7);
                return 4;
            case 2: {                                                 // RAL
                const uint8_t a = m_a;
                const int c = m_f & FC;
                m_a = (uint8_t)(a << 1 | c);
                m_f = (uint8_t)((m_f & ~(FC | FV | FK)) | (a >> 7) | vk(a, a, c, m_a));
                return 4;
            }
            case 3: {                                                 // RAR
                const int c = m_f & FC;
                m_f = (uint8_t)((m_f & ~(FC | FV)) | (m_a & 1));
                m_a = (uint8_t)(m_a >> 1 | c << 7);
                return 4;
            }
            case 4: daa(); return 4;
            case 5: m_a = (uint8_t)~m_a; return 4;                    // CMA (no flags)
            case 6: m_f |= FC; return 4;                              // STC
            default: m_f ^= FC; return 4;                             // CMC
            }
        }
    case 1:
        if (op == 0x76) { m_halted = true; return 5; }               // HLT
        setReg8(y, reg8(z));                                          // MOV r,r
        return (y == 6 || z == 6) ? 7 : 4;
    case 2: {
        const uint8_t v = reg8(z);
        switch (y) {
        case 0: add(v, 0); break;
        case 1: add(v, m_f & FC); break;
        case 2: m_a = sub(v, 0); break;
        case 3: m_a = sub(v, m_f & FC); break;
        case 4: logic(m_a & v, true); break;
        case 5: logic(m_a ^ v, false); break;
        case 6: logic(m_a | v, false); break;
        default: sub(v, 0); break;
        }
        return z == 6 ? 7 : 4;
    }
    default:
        switch (z) {
        case 0:                                                       // Rcc: 6 / 12
            if (!cond(y)) return 6;
            m_pc = pop();
            return 12;
        case 1:
            if (!q) {                                                 // POP rp (PSW: bit 3 stays 0)
                const uint16_t v = pop();
                if (p == 3) { m_a = v >> 8; m_f = (uint8_t)(v & 0xF7); }
                else setRp(p, v);
                return 10;
            }
            switch (p) {
            case 0: m_pc = pop(); return 10;                          // RET
            case 1: wr16(de(), hl()); return 10;                      // SHLX
            case 2: m_pc = hl(); return 6;                            // PCHL
            default: m_sp = hl(); return 6;                           // SPHL
            }
        case 2: {                                                     // Jcc nn: 7 / 10
            if (!cond(y)) { m_pc = (uint16_t)(m_pc + 2); return 7; }
            m_pc = fetch16();
            return 10;
        }
        case 3:
            switch (y) {
            case 0: m_pc = fetch16(); return 10;                      // JMP nn
            case 1:                                                   // RSTV: RST 8 ($40) when V
                if (!(m_f & FV)) return 6;
                push(m_pc);
                m_pc = 0x0040;
                return 12;
            case 2: {                                                 // OUT n
                const uint8_t port = fetch();
                if (m_io) m_io->write8(port, m_a);
                return 10;
            }
            case 3: {                                                 // IN n
                const uint8_t port = fetch();
                m_a = m_io ? m_io->read8(port) : 0xFF;
                return 10;
            }
            case 4: {                                                 // XTHL
                const uint16_t v = rd16(m_sp);
                wr16(m_sp, hl());
                setHl(v);
                return 16;
            }
            case 5: {                                                 // XCHG
                const uint16_t d = de();
                setDe(hl());
                setHl(d);
                return 4;
            }
            case 6: m_ie = false; return 4;                           // DI
            default: m_ie = true; m_eiDelay = true; return 4;         // EI
            }
        case 4: {                                                     // Ccc nn: 9 / 18
            if (!cond(y)) { m_pc = (uint16_t)(m_pc + 2); return 9; }
            const uint16_t t = fetch16();
            push(m_pc);
            m_pc = t;
            return 18;
        }
        case 5:
            if (!q) {                                                 // PUSH rp
                push(p == 3 ? (uint16_t)(m_a << 8 | m_f) : rp(p));
                return 12;
            }
            switch (p) {
            case 0: {                                                 // CALL nn
                const uint16_t t = fetch16();
                push(m_pc);
                m_pc = t;
                return 18;
            }
            case 2: setHl(rd16(de())); return 10;                     // LHLX
            default: {                                                // JNK / JK nn: 7 / 10
                const bool k = (m_f & FK) != 0;
                if (k != (p == 3)) { m_pc = (uint16_t)(m_pc + 2); return 7; }
                m_pc = fetch16();
                return 10;
            }
            }
        case 6: {                                                     // ALU n
            const uint8_t v = fetch();
            switch (y) {
            case 0: add(v, 0); break;
            case 1: add(v, m_f & FC); break;
            case 2: m_a = sub(v, 0); break;
            case 3: m_a = sub(v, m_f & FC); break;
            case 4: logic(m_a & v, true); break;
            case 5: logic(m_a ^ v, false); break;
            case 6: logic(m_a | v, false); break;
            default: sub(v, 0); break;
            }
            return 7;
        }
        default:                                                      // RST n
            push(m_pc);
            m_pc = (uint16_t)(y * 8);
            return 12;
        }
    }
}

// ---------------------------------------------------------------------------
// Debugger support
// ---------------------------------------------------------------------------

int I8085::disassembleOne(IBus* bus, uint32_t addr, char* buf, int bufsz) {
    const auto peek = [bus](uint32_t a) -> uint8_t { return bus ? bus->peek8(a & 0xFFFF) : 0; };
    const I8085Opcode& o = i8085Opcode(peek(addr));
    std::string text = o.mnemonic;
    if (!o.operands.empty()) {
        text += ' ';
        size_t at = 0;
        while (true) {
            const size_t comma = o.operands.find(',', at);
            const std::string part = o.operands.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            char num[8];
            if (part == "n") { std::snprintf(num, sizeof num, "$%02X", peek(addr + 1)); text += num; }
            else if (part == "nn") { std::snprintf(num, sizeof num, "$%04X", peek(addr + 1) | peek(addr + 2) << 8); text += num; }
            else text += part;
            if (comma == std::string::npos) break;
            text += ',';
            at = comma + 1;
        }
    }
    if (buf && bufsz > 0) std::snprintf(buf, (size_t)bufsz, "%s", text.c_str());
    return o.length;
}

int I8085::disassembleEntry(IBus* bus, uint32_t addr, void* entryOut) {
    auto* e = static_cast<DisasmEntry*>(entryOut);
    char buf[32];
    e->addr = addr;
    e->bytes = disassembleOne(bus, addr, buf, sizeof buf);
    const std::string s(buf);
    const auto sp = s.find(' ');
    e->mnemonic = sp == std::string::npos ? s : s.substr(0, sp);
    e->operands = sp == std::string::npos ? "" : s.substr(sp + 1);
    e->complete = s;
    const uint8_t op = bus ? bus->peek8(addr & 0xFFFF) : 0;
    const I8085Opcode& o = i8085Opcode(op);
    e->isCall = isCallAt(bus, addr) > 0;
    e->isReturn = isReturnAt(bus, addr);
    e->isBranch = op == 0xC3 || (op & 0xC7) == 0xC2 || op == 0xDD || op == 0xFD || op == 0xE9;
    e->isIllegal = o.undocumented;
    e->targetAddr = 0;
    if (o.length == 3 && (e->isBranch || e->isCall) && bus)
        e->targetAddr = (uint32_t)(bus->peek8((addr + 1) & 0xFFFF) | bus->peek8((addr + 2) & 0xFFFF) << 8);
    else if ((op & 0xC7) == 0xC7) e->targetAddr = op & 0x38;
    e->rawLen = (uint8_t)std::min(e->bytes, (int)DisasmEntry::MAX_RAW);
    for (int i = 0; i < e->rawLen; ++i) e->raw[i] = bus ? bus->peek8((addr + i) & 0xFFFF) : 0;
    return e->bytes;
}

int I8085::isCallAt(IBus* bus, uint32_t addr) {
    if (!bus) return 0;
    const uint8_t op = bus->peek8(addr & 0xFFFF);
    if (op == 0xCD || (op & 0xC7) == 0xC4) return 3;          // CALL, Ccc
    if ((op & 0xC7) == 0xC7 || op == 0xCB) return 1;          // RST, RSTV
    return 0;
}

bool I8085::isReturnAt(IBus* bus, uint32_t addr) {
    if (!bus) return false;
    const uint8_t op = bus->peek8(addr & 0xFFFF);
    return op == 0xC9 || (op & 0xC7) == 0xC0;
}

bool I8085::isProgramEnd(IBus* bus) { (void)bus; return m_halted && !m_ie; }

int I8085::writeCallHarness(IBus* bus, uint32_t scratch, uint32_t routineAddr) {
    // CALL routine ; HLT
    bus->write8(scratch, 0xCD);
    bus->write8(scratch + 1, routineAddr & 0xFF);
    bus->write8(scratch + 2, (routineAddr >> 8) & 0xFF);
    bus->write8(scratch + 3, 0x76);
    return 4;
}
