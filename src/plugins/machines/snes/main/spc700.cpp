// Sony SPC700 core (#15) after the SPC700 documentation (Anomie, fullsnes)
// and bsnes/Mesen2 (Core/SNES/Spc.Instructions.cpp, GPL-3.0); checked
// against Tom Harte's SingleStepTests.

#include "spc700.h"
#include "libmem/main/ibus.h"
#include "libdebug/main/execution_observer.h"
#include "libtoolchain/main/idisasm.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

const RegDescriptor REGS[Spc700::R_COUNT] = {
    {"A", RegWidth::R8, REGFLAG_ACCUMULATOR, nullptr, nullptr},
    {"X", RegWidth::R8, 0, nullptr, nullptr},
    {"Y", RegWidth::R8, 0, nullptr, nullptr},
    {"SP", RegWidth::R8, REGFLAG_SP, nullptr, nullptr},
    {"PC", RegWidth::R16, REGFLAG_PC, nullptr, nullptr},
    {"PSW", RegWidth::R8, REGFLAG_STATUS, nullptr, "NVPBHIZC"},
    {"YA", RegWidth::R16, REGFLAG_INTERNAL, nullptr, nullptr},
};

// Cycles per opcode (conditional branches not taken; +2 when taken)
const uint8_t CYCLES[256] = {
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 6, 8,
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 4, 6,
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 5, 4,
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 3, 8,
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 6, 6,
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 4, 5, 2, 2, 4, 3,
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 5, 5,
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 6,
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 2, 4, 5,
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 12, 5,
    3, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 2, 4, 4,
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 4,
    3, 8, 4, 5, 4, 5, 4, 7, 2, 5, 6, 4, 5, 2, 4, 9,
    2, 8, 4, 5, 5, 6, 6, 7, 4, 5, 5, 5, 2, 2, 6, 3,
    2, 8, 4, 5, 3, 4, 3, 6, 2, 4, 5, 3, 4, 3, 4, 3,
    2, 8, 4, 5, 4, 5, 5, 6, 3, 4, 5, 4, 2, 2, 4, 3,
};

// Disassembly: $1/$2 a direct-page operand byte, #1 an immediate byte, @1 an
// absolute word, r1/r2 a relative branch byte, m1 a 13-bit address with a
// bit number, u1 a PCALL page offset
const char* const OPS[256] = {
    "NOP", "TCALL 0", "SET1 $1.0", "BBS $1.0,r2", "OR A,$1", "OR A,@1", "OR A,(X)", "OR A,[$1+X]", "OR A,#1", "OR $2,$1", "OR1 C,m1", "ASL $1", "ASL @1", "PUSH PSW", "TSET1 @1", "BRK",
    "BPL r1", "TCALL 1", "CLR1 $1.0", "BBC $1.0,r2", "OR A,$1+X", "OR A,@1+X", "OR A,@1+Y", "OR A,[$1]+Y", "OR $2,#1", "OR (X),(Y)", "DECW $1", "ASL $1+X", "ASL A", "DEC X", "CMP X,@1", "JMP [@1+X]",
    "CLRP", "TCALL 2", "SET1 $1.1", "BBS $1.1,r2", "AND A,$1", "AND A,@1", "AND A,(X)", "AND A,[$1+X]", "AND A,#1", "AND $2,$1", "OR1 C,/m1", "ROL $1", "ROL @1", "PUSH A", "CBNE $1,r2", "BRA r1",
    "BMI r1", "TCALL 3", "CLR1 $1.1", "BBC $1.1,r2", "AND A,$1+X", "AND A,@1+X", "AND A,@1+Y", "AND A,[$1]+Y", "AND $2,#1", "AND (X),(Y)", "INCW $1", "ROL $1+X", "ROL A", "INC X", "CMP X,$1", "CALL @1",
    "SETP", "TCALL 4", "SET1 $1.2", "BBS $1.2,r2", "EOR A,$1", "EOR A,@1", "EOR A,(X)", "EOR A,[$1+X]", "EOR A,#1", "EOR $2,$1", "AND1 C,m1", "LSR $1", "LSR @1", "PUSH X", "TCLR1 @1", "PCALL u1",
    "BVC r1", "TCALL 5", "CLR1 $1.2", "BBC $1.2,r2", "EOR A,$1+X", "EOR A,@1+X", "EOR A,@1+Y", "EOR A,[$1]+Y", "EOR $2,#1", "EOR (X),(Y)", "CMPW YA,$1", "LSR $1+X", "LSR A", "MOV X,A", "CMP Y,@1", "JMP @1",
    "CLRC", "TCALL 6", "SET1 $1.3", "BBS $1.3,r2", "CMP A,$1", "CMP A,@1", "CMP A,(X)", "CMP A,[$1+X]", "CMP A,#1", "CMP $2,$1", "AND1 C,/m1", "ROR $1", "ROR @1", "PUSH Y", "DBNZ $1,r2", "RET",
    "BVS r1", "TCALL 7", "CLR1 $1.3", "BBC $1.3,r2", "CMP A,$1+X", "CMP A,@1+X", "CMP A,@1+Y", "CMP A,[$1]+Y", "CMP $2,#1", "CMP (X),(Y)", "ADDW YA,$1", "ROR $1+X", "ROR A", "MOV A,X", "CMP Y,$1", "RETI",
    "SETC", "TCALL 8", "SET1 $1.4", "BBS $1.4,r2", "ADC A,$1", "ADC A,@1", "ADC A,(X)", "ADC A,[$1+X]", "ADC A,#1", "ADC $2,$1", "EOR1 C,m1", "DEC $1", "DEC @1", "MOV Y,#1", "POP PSW", "MOV $2,#1",
    "BCC r1", "TCALL 9", "CLR1 $1.4", "BBC $1.4,r2", "ADC A,$1+X", "ADC A,@1+X", "ADC A,@1+Y", "ADC A,[$1]+Y", "ADC $2,#1", "ADC (X),(Y)", "SUBW YA,$1", "DEC $1+X", "DEC A", "MOV X,SP", "DIV YA,X", "XCN A",
    "EI", "TCALL 10", "SET1 $1.5", "BBS $1.5,r2", "SBC A,$1", "SBC A,@1", "SBC A,(X)", "SBC A,[$1+X]", "SBC A,#1", "SBC $2,$1", "MOV1 C,m1", "INC $1", "INC @1", "CMP Y,#1", "POP A", "MOV (X)+,A",
    "BCS r1", "TCALL 11", "CLR1 $1.5", "BBC $1.5,r2", "SBC A,$1+X", "SBC A,@1+X", "SBC A,@1+Y", "SBC A,[$1]+Y", "SBC $2,#1", "SBC (X),(Y)", "MOVW YA,$1", "INC $1+X", "INC A", "MOV SP,X", "DAS A", "MOV A,(X)+",
    "DI", "TCALL 12", "SET1 $1.6", "BBS $1.6,r2", "MOV $1,A", "MOV @1,A", "MOV (X),A", "MOV [$1+X],A", "CMP X,#1", "MOV @1,X", "MOV1 m1,C", "MOV $1,Y", "MOV @1,Y", "MOV X,#1", "POP X", "MUL YA",
    "BNE r1", "TCALL 13", "CLR1 $1.6", "BBC $1.6,r2", "MOV $1+X,A", "MOV @1+X,A", "MOV @1+Y,A", "MOV [$1]+Y,A", "MOV $1,X", "MOV $1+Y,X", "MOVW $1,YA", "MOV $1+X,Y", "DEC Y", "MOV A,Y", "CBNE $1+X,r2", "DAA A",
    "CLRV", "TCALL 14", "SET1 $1.7", "BBS $1.7,r2", "MOV A,$1", "MOV A,@1", "MOV A,(X)", "MOV A,[$1+X]", "MOV A,#1", "MOV X,@1", "NOT1 m1", "MOV Y,$1", "MOV Y,@1", "NOTC", "POP Y", "SLEEP",
    "BEQ r1", "TCALL 15", "CLR1 $1.7", "BBC $1.7,r2", "MOV A,$1+X", "MOV A,@1+X", "MOV A,@1+Y", "MOV A,[$1]+Y", "MOV X,$1", "MOV X,$1+Y", "MOV $2,$1", "MOV Y,$1+X", "INC Y", "MOV Y,A", "DBNZ Y,r1", "STOP",
};

}  // namespace

Spc700::Spc700() { reset(); }

const RegDescriptor* Spc700::regDescriptor(int idx) const { return idx >= 0 && idx < R_COUNT ? &REGS[idx] : nullptr; }

uint32_t Spc700::regRead(int idx) const {
    switch (idx) {
    case R_A: return m_a; case R_X: return m_x; case R_Y: return m_y; case R_SP: return m_sp;
    case R_PC: return m_pc; case R_PSW: return m_psw; case R_YA: return (uint32_t)(m_y << 8 | m_a);
    }
    return 0;
}

void Spc700::regWrite(int idx, uint32_t v) {
    switch (idx) {
    case R_A: m_a = (uint8_t)v; break; case R_X: m_x = (uint8_t)v; break; case R_Y: m_y = (uint8_t)v; break;
    case R_SP: m_sp = (uint8_t)v; break; case R_PC: m_pc = (uint16_t)v; m_stopped = false; break;
    case R_PSW: m_psw = (uint8_t)v; break; case R_YA: m_a = (uint8_t)v; m_y = (uint8_t)(v >> 8); break;
    }
}

void Spc700::reset() {
    m_a = m_x = m_y = 0;
    m_sp = 0xEF;
    m_psw = 0x02;
    m_stopped = false;
    m_pc = m_bus ? (uint16_t)(m_bus->read8(0xFFFE) | m_bus->read8(0xFFFF) << 8) : 0xFFC0;
}

uint8_t Spc700::rd(uint16_t a) { return m_bus ? m_bus->read8(a) : 0; }
void Spc700::wr(uint16_t a, uint8_t v) { if (m_bus) m_bus->write8(a, v); }

uint8_t Spc700::adc(uint8_t a, uint8_t b) {
    const int r = a + b + (m_psw & FC);
    flag(FC, r > 0xFF);
    flag(FV, ~(a ^ b) & (a ^ r) & 0x80);
    flag(FH, (a ^ b ^ r) & 0x10);
    nz((uint8_t)r);
    return (uint8_t)r;
}

uint8_t Spc700::alu(int op, uint8_t a, uint8_t b) {
    switch (op) {
    case 0: a |= b; nz(a); return a;
    case 1: a &= b; nz(a); return a;
    case 2: a ^= b; nz(a); return a;
    case 3: flag(FC, a >= b); nz((uint8_t)(a - b)); return a;
    case 4: return adc(a, b);
    default: return adc(a, (uint8_t)~b);
    }
}

uint8_t Spc700::shift(int op, uint8_t v) {
    const uint8_t c = m_psw & FC;
    switch (op) {
    case 0: flag(FC, v & 0x80); v = (uint8_t)(v << 1); break;
    case 1: flag(FC, v & 0x80); v = (uint8_t)(v << 1 | c); break;
    case 2: flag(FC, v & 1); v >>= 1; break;
    default: flag(FC, v & 1); v = (uint8_t)(v >> 1 | c << 7); break;
    }
    nz(v);
    return v;
}

bool Spc700::branch(bool take) {
    const int8_t rel = (int8_t)fetch();
    if (take) { m_pc = (uint16_t)(m_pc + rel); m_extra += 2; }
    return take;
}

int Spc700::step() {
    if (m_stopped) { m_cycles += 2; return 2; }
    if (m_observer) {
        if (m_observer->needsDisasm()) {
            DisasmEntry entry;
            disassembleEntry(m_bus, m_pc, &entry);
            if (!m_observer->onStep(this, m_bus, entry)) return 1;
        } else {
            m_observer->onStepLite(this, m_pc);
        }
    }
    m_extra = 0;
    const uint8_t op = fetch();
    const int hi = op >> 4, lo = op & 0x0F;

    // The ALU block: OR AND EOR CMP ADC SBC, rows 0-B, columns 4-9
    if (hi <= 0xB && lo >= 4 && lo <= 9) {
        const int f = hi >> 1;
        if (!(hi & 1)) {
            uint8_t b;
            switch (lo) {
            case 4: b = rd(dp(fetch())); break;
            case 5: b = rd(fetch16()); break;
            case 6: b = rd(dp(m_x)); break;
            case 7: { const uint8_t d = (uint8_t)(fetch() + m_x); b = rd((uint16_t)(rd(dp(d)) | rd(dp((uint8_t)(d + 1))) << 8)); break; }
            case 8: b = fetch(); break;
            default: {                                   // dd, ds
                const uint8_t s = fetch(), d = fetch();
                b = rd(dp(s));
                const uint8_t r = alu(f, rd(dp(d)), b);
                if (f != 3) wr(dp(d), r);
                goto done;
            }
            }
            m_a = alu(f, m_a, b);
        } else {
            uint8_t b;
            switch (lo) {
            case 4: b = rd(dp((uint8_t)(fetch() + m_x))); break;
            case 5: b = rd((uint16_t)(fetch16() + m_x)); break;
            case 6: b = rd((uint16_t)(fetch16() + m_y)); break;
            case 7: { const uint8_t d = fetch(); b = rd((uint16_t)(rdDp16(d) + m_y)); break; }
            case 8: {                                    // d, #i
                const uint8_t i = fetch(), d = fetch();
                const uint8_t r = alu(f, rd(dp(d)), i);
                if (f != 3) wr(dp(d), r);
                goto done;
            }
            default: {                                   // (X), (Y)
                b = rd(dp(m_y));
                const uint8_t r = alu(f, rd(dp(m_x)), b);
                if (f != 3) wr(dp(m_x), r);
                goto done;
            }
            }
            m_a = alu(f, m_a, b);
        }
        goto done;
    }
    // Shifts (rows 0-7) and DEC/INC (rows 8-B), columns B and C
    if (hi <= 0xB && (lo == 0xB || lo == 0xC)) {
        const int f = hi >> 1;
        auto apply = [&](uint8_t v) -> uint8_t {
            if (f < 4) return shift(f, v);
            v = f == 4 ? (uint8_t)(v - 1) : (uint8_t)(v + 1);
            nz(v);
            return v;
        };
        if (!(hi & 1)) {
            const uint16_t a = lo == 0xB ? dp(fetch()) : fetch16();
            wr(a, apply(rd(a)));
        } else if (lo == 0xB) {
            const uint16_t a = dp((uint8_t)(fetch() + m_x));
            wr(a, apply(rd(a)));
        } else {
            m_a = apply(m_a);
        }
        goto done;
    }
    // TCALL n, SET1/CLR1 d.n, BBS/BBC d.n
    if (lo == 1) { const uint16_t v = (uint16_t)(0xFFDE - 2 * hi); call((uint16_t)(rd(v) | rd((uint16_t)(v + 1)) << 8)); goto done; }
    if (lo == 2) { const uint16_t a = dp(fetch()); const uint8_t m = (uint8_t)(1 << (hi >> 1)); const uint8_t v = rd(a); wr(a, (hi & 1) ? (uint8_t)(v & ~m) : (uint8_t)(v | m)); goto done; }
    if (lo == 3) { const uint8_t v = rd(dp(fetch())); const bool set = v & (1 << (hi >> 1)); branch((hi & 1) ? !set : set); goto done; }

    switch (op) {
    // Branches
    case 0x10: branch(!(m_psw & FN)); break;
    case 0x30: branch(m_psw & FN); break;
    case 0x50: branch(!(m_psw & FV)); break;
    case 0x70: branch(m_psw & FV); break;
    case 0x90: branch(!(m_psw & FC)); break;
    case 0xB0: branch(m_psw & FC); break;
    case 0xD0: branch(!(m_psw & FZ)); break;
    case 0xF0: branch(m_psw & FZ); break;
    case 0x2F: { const int8_t rel = (int8_t)fetch(); m_pc = (uint16_t)(m_pc + rel); break; }
    case 0x2E: { const uint8_t v = rd(dp(fetch())); branch(m_a != v); break; }
    case 0xDE: { const uint8_t v = rd(dp((uint8_t)(fetch() + m_x))); branch(m_a != v); break; }
    case 0x6E: { const uint16_t a = dp(fetch()); const uint8_t v = (uint8_t)(rd(a) - 1); wr(a, v); branch(v != 0); break; }
    case 0xFE: --m_y; branch(m_y != 0); break;

    // Flags
    case 0x00: break;
    case 0x20: m_psw &= ~FP; break;
    case 0x40: m_psw |= FP; break;
    case 0x60: m_psw &= ~FC; break;
    case 0x80: m_psw |= FC; break;
    case 0xA0: m_psw |= FI; break;
    case 0xC0: m_psw &= ~FI; break;
    case 0xE0: m_psw &= ~(FV | FH); break;
    case 0xED: m_psw ^= FC; break;

    // Bit instructions on a 13-bit address
    case 0x0A: case 0x2A: case 0x4A: case 0x6A: case 0x8A: case 0xAA: case 0xCA: case 0xEA: {
        const uint16_t w = fetch16();
        const uint16_t a = w & 0x1FFF;
        const int bit = w >> 13;
        const uint8_t v = rd(a);
        const bool b = (v >> bit) & 1, c = m_psw & FC;
        switch (op) {
        case 0x0A: flag(FC, c || b); break;
        case 0x2A: flag(FC, c || !b); break;
        case 0x4A: flag(FC, c && b); break;
        case 0x6A: flag(FC, c && !b); break;
        case 0x8A: flag(FC, c != b); break;
        case 0xAA: flag(FC, b); break;
        case 0xCA: wr(a, (uint8_t)((v & ~(1 << bit)) | (c << bit))); break;
        case 0xEA: wr(a, (uint8_t)(v ^ (1 << bit))); break;
        }
        break;
    }

    // Stack, calls, returns
    case 0x0D: push(m_psw); break;
    case 0x2D: push(m_a); break;
    case 0x4D: push(m_x); break;
    case 0x6D: push(m_y); break;
    case 0x8E: m_psw = pull(); break;
    case 0xAE: m_a = pull(); break;
    case 0xCE: m_x = pull(); break;
    case 0xEE: m_y = pull(); break;
    case 0x3F: call(fetch16()); break;
    case 0x4F: call((uint16_t)(0xFF00 | fetch())); break;
    case 0x0F: {
        push((uint8_t)(m_pc >> 8)); push((uint8_t)m_pc); push(m_psw);
        m_psw = (uint8_t)((m_psw | FB) & ~FI);
        m_pc = (uint16_t)(rd(0xFFDE) | rd(0xFFDF) << 8);
        break;
    }
    case 0x6F: { const uint8_t l = pull(); m_pc = (uint16_t)(l | pull() << 8); break; }
    case 0x7F: { m_psw = pull(); const uint8_t l = pull(); m_pc = (uint16_t)(l | pull() << 8); break; }
    case 0x5F: m_pc = fetch16(); break;
    case 0x1F: { const uint16_t p = (uint16_t)(fetch16() + m_x); m_pc = (uint16_t)(rd(p) | rd((uint16_t)(p + 1)) << 8); break; }

    // Compares with X and Y
    case 0x1E: alu(3, m_x, rd(fetch16())); break;
    case 0x3E: alu(3, m_x, rd(dp(fetch()))); break;
    case 0xC8: alu(3, m_x, fetch()); break;
    case 0x5E: alu(3, m_y, rd(fetch16())); break;
    case 0x7E: alu(3, m_y, rd(dp(fetch()))); break;
    case 0xAD: alu(3, m_y, fetch()); break;

    // Register moves and register INC/DEC
    case 0x1D: nz(--m_x); break;
    case 0x3D: nz(++m_x); break;
    case 0xDC: nz(--m_y); break;
    case 0xFC: nz(++m_y); break;
    case 0x5D: m_x = m_a; nz(m_x); break;
    case 0x7D: m_a = m_x; nz(m_a); break;
    case 0xDD: m_a = m_y; nz(m_a); break;
    case 0xFD: m_y = m_a; nz(m_y); break;
    case 0x9D: m_x = m_sp; nz(m_x); break;
    case 0xBD: m_sp = m_x; break;

    // Loads
    case 0xE4: m_a = rd(dp(fetch())); nz(m_a); break;
    case 0xE5: m_a = rd(fetch16()); nz(m_a); break;
    case 0xE6: m_a = rd(dp(m_x)); nz(m_a); break;
    case 0xE7: { const uint8_t d = (uint8_t)(fetch() + m_x); m_a = rd((uint16_t)(rd(dp(d)) | rd(dp((uint8_t)(d + 1))) << 8)); nz(m_a); break; }
    case 0xE8: m_a = fetch(); nz(m_a); break;
    case 0xF4: m_a = rd(dp((uint8_t)(fetch() + m_x))); nz(m_a); break;
    case 0xF5: m_a = rd((uint16_t)(fetch16() + m_x)); nz(m_a); break;
    case 0xF6: m_a = rd((uint16_t)(fetch16() + m_y)); nz(m_a); break;
    case 0xF7: { const uint8_t d = fetch(); m_a = rd((uint16_t)(rdDp16(d) + m_y)); nz(m_a); break; }
    case 0xBF: m_a = rd(dp(m_x)); ++m_x; nz(m_a); break;
    case 0xCD: m_x = fetch(); nz(m_x); break;
    case 0xE9: m_x = rd(fetch16()); nz(m_x); break;
    case 0xF8: m_x = rd(dp(fetch())); nz(m_x); break;
    case 0xF9: m_x = rd(dp((uint8_t)(fetch() + m_y))); nz(m_x); break;
    case 0x8D: m_y = fetch(); nz(m_y); break;
    case 0xEB: m_y = rd(dp(fetch())); nz(m_y); break;
    case 0xEC: m_y = rd(fetch16()); nz(m_y); break;
    case 0xFB: m_y = rd(dp((uint8_t)(fetch() + m_x))); nz(m_y); break;

    // Stores (no flags)
    case 0xC4: wr(dp(fetch()), m_a); break;
    case 0xC5: wr(fetch16(), m_a); break;
    case 0xC6: wr(dp(m_x), m_a); break;
    case 0xC7: { const uint8_t d = (uint8_t)(fetch() + m_x); wr((uint16_t)(rd(dp(d)) | rd(dp((uint8_t)(d + 1))) << 8), m_a); break; }
    case 0xD4: wr(dp((uint8_t)(fetch() + m_x)), m_a); break;
    case 0xD5: wr((uint16_t)(fetch16() + m_x), m_a); break;
    case 0xD6: wr((uint16_t)(fetch16() + m_y), m_a); break;
    case 0xD7: { const uint8_t d = fetch(); wr((uint16_t)(rdDp16(d) + m_y), m_a); break; }
    case 0xAF: wr(dp(m_x), m_a); ++m_x; break;
    case 0xC9: wr(fetch16(), m_x); break;
    case 0xD8: wr(dp(fetch()), m_x); break;
    case 0xD9: wr(dp((uint8_t)(fetch() + m_y)), m_x); break;
    case 0xCB: wr(dp(fetch()), m_y); break;
    case 0xCC: wr(fetch16(), m_y); break;
    case 0xDB: wr(dp((uint8_t)(fetch() + m_x)), m_y); break;
    case 0x8F: { const uint8_t i = fetch(); wr(dp(fetch()), i); break; }
    case 0xFA: { const uint8_t s = fetch(); const uint8_t v = rd(dp(s)); wr(dp(fetch()), v); break; }

    // Test-and-set/clear
    case 0x0E: case 0x4E: {
        const uint16_t a = fetch16();
        const uint8_t v = rd(a);
        nz((uint8_t)(m_a - v));
        wr(a, op == 0x0E ? (uint8_t)(v | m_a) : (uint8_t)(v & ~m_a));
        break;
    }

    // 16-bit
    case 0x1A: case 0x3A: {
        const uint8_t d = fetch();
        uint16_t w = rdDp16(d);
        w = op == 0x1A ? (uint16_t)(w - 1) : (uint16_t)(w + 1);
        wr(dp(d), (uint8_t)w);
        wr(dp((uint8_t)(d + 1)), (uint8_t)(w >> 8));
        nz16(w);
        break;
    }
    case 0x5A: { const uint16_t w = rdDp16(fetch()), ya = (uint16_t)(m_y << 8 | m_a); flag(FC, ya >= w); nz16((uint16_t)(ya - w)); break; }
    case 0x7A: case 0x9A: {
        const uint16_t w = rdDp16(fetch()), ya = (uint16_t)(m_y << 8 | m_a);
        uint16_t r;
        if (op == 0x7A) {
            const uint32_t s = (uint32_t)ya + w;
            r = (uint16_t)s;
            flag(FC, s > 0xFFFF);
            flag(FV, ~(ya ^ w) & (ya ^ r) & 0x8000);
            flag(FH, (ya ^ w ^ r) & 0x1000);
        } else {
            r = (uint16_t)(ya - w);
            flag(FC, ya >= w);
            flag(FV, (ya ^ w) & (ya ^ r) & 0x8000);
            flag(FH, !((ya ^ w ^ r) & 0x1000));
        }
        m_a = (uint8_t)r; m_y = (uint8_t)(r >> 8);
        nz16(r);
        break;
    }
    case 0xBA: { const uint8_t d = fetch(); m_a = rd(dp(d)); m_y = rd(dp((uint8_t)(d + 1))); nz16((uint16_t)(m_y << 8 | m_a)); break; }
    case 0xDA: { const uint8_t d = fetch(); wr(dp(d), m_a); wr(dp((uint8_t)(d + 1)), m_y); break; }

    // Multiply, divide, decimal adjust, nibble swap
    case 0xCF: { const uint16_t r = (uint16_t)(m_y * m_a); m_a = (uint8_t)r; m_y = (uint8_t)(r >> 8); nz(m_y); break; }
    case 0x9E: {
        // bsnes: the S-SMP's divide, including its results when the quotient overflows 9 bits
        const uint16_t ya = (uint16_t)(m_y << 8 | m_a);
        flag(FV, m_y >= m_x);
        flag(FH, (m_y & 15) >= (m_x & 15));
        if (m_y < (m_x << 1)) {
            m_a = (uint8_t)(ya / m_x);
            m_y = (uint8_t)(ya % m_x);
        } else {
            m_a = (uint8_t)(255 - (ya - (m_x << 9)) / (256 - m_x));
            m_y = (uint8_t)(m_x + (ya - (m_x << 9)) % (256 - m_x));
        }
        nz(m_a);
        break;
    }
    case 0xDF:
        if ((m_psw & FC) || m_a > 0x99) { m_a = (uint8_t)(m_a + 0x60); m_psw |= FC; }
        if ((m_psw & FH) || (m_a & 15) > 9) m_a = (uint8_t)(m_a + 6);
        nz(m_a);
        break;
    case 0xBE:
        if (!(m_psw & FC) || m_a > 0x99) { m_a = (uint8_t)(m_a - 0x60); m_psw &= ~FC; }
        if (!(m_psw & FH) || (m_a & 15) > 9) m_a = (uint8_t)(m_a - 6);
        nz(m_a);
        break;
    case 0x9F: m_a = (uint8_t)(m_a >> 4 | m_a << 4); nz(m_a); break;

    case 0xEF: case 0xFF: m_stopped = true; break;
    default: break;
    }
done:
    const int c = CYCLES[op] + m_extra;
    m_cycles += (uint64_t)c;
    return c;
}

int Spc700::disassembleOne(IBus* bus, uint32_t addr, char* buf, int bufsz) {
    auto b = [&](int i) { return bus ? bus->peek8((addr + (uint32_t)i) & 0xFFFF) : 0; };
    const char* f = OPS[b(0)];
    int len = 1;
    std::string out;
    char t[24];
    for (const char* p = f; *p; ++p) {
        const char c = *p, n = p[1];
        if ((c == '$' || c == '#' || c == '@' || c == 'r' || c == 'm' || c == 'u') && (n == '1' || n == '2')) {
            const int i = n - '0';
            switch (c) {
            case '$': std::snprintf(t, sizeof t, "$%02X", b(i)); len = std::max(len, i + 1); break;
            case '#': std::snprintf(t, sizeof t, "#$%02X", b(i)); len = std::max(len, i + 1); break;
            case '@': std::snprintf(t, sizeof t, "$%04X", b(i) | b(i + 1) << 8); len = std::max(len, i + 2); break;
            case 'm': { const int w = b(i) | b(i + 1) << 8; std::snprintf(t, sizeof t, "$%04X.%d", w & 0x1FFF, w >> 13); len = std::max(len, i + 2); break; }
            case 'u': std::snprintf(t, sizeof t, "$FF%02X", b(i)); len = std::max(len, i + 1); break;
            default: len = std::max(len, i + 1); t[0] = 0; break;
            }
            if (c == 'r') {
                // the target is relative to the end of the instruction (computed below)
                out += '\x01';
                out += (char)i;
            } else {
                out += t;
            }
            ++p;
            continue;
        }
        out += c;
    }
    std::string fin;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\x01') {
            const int bi = out[++i];
            std::snprintf(t, sizeof t, "$%04X", (unsigned)((addr + (uint32_t)len + (uint32_t)(int8_t)b(bi)) & 0xFFFF));
            fin += t;
        } else {
            fin += out[i];
        }
    }
    std::snprintf(buf, (size_t)bufsz, "%s", fin.c_str());
    return len;
}

int Spc700::disassembleEntry(IBus* bus, uint32_t addr, void* entryOut) {
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
    const std::string& m = e->mnemonic;
    e->isBranch = (m.size() == 3 && m[0] == 'B' && m != "BRK") || m == "JMP" || m == "CBNE" || m == "DBNZ";
    e->isIllegal = false;
    e->targetAddr = 0;
    e->rawLen = (uint8_t)std::min(e->bytes, (int)DisasmEntry::MAX_RAW);
    for (int i = 0; i < e->rawLen; ++i) e->raw[i] = bus ? bus->peek8((addr + (uint32_t)i) & 0xFFFF) : 0;
    return e->bytes;
}

int Spc700::isCallAt(IBus* bus, uint32_t addr) {
    if (!bus) return 0;
    const uint8_t op = bus->peek8(addr & 0xFFFF);
    if (op == 0x3F) return 3;
    if (op == 0x4F) return 2;
    if ((op & 0x0F) == 0x01 || op == 0x0F) return 1;
    return 0;
}

bool Spc700::isReturnAt(IBus* bus, uint32_t addr) {
    if (!bus) return false;
    const uint8_t op = bus->peek8(addr & 0xFFFF);
    return op == 0x6F || op == 0x7F;
}

int Spc700::writeCallHarness(IBus* bus, uint32_t scratch, uint32_t routineAddr) {
    // CALL routine ; STOP
    bus->write8(scratch, 0x3F);
    bus->write8(scratch + 1, routineAddr & 0xFF);
    bus->write8(scratch + 2, (routineAddr >> 8) & 0xFF);
    bus->write8(scratch + 3, 0xFF);
    return 4;
}

void Spc700::saveState(uint8_t* b) const {
    b[0] = m_a; b[1] = m_x; b[2] = m_y; b[3] = m_sp; b[4] = m_psw;
    b[5] = (uint8_t)m_pc; b[6] = (uint8_t)(m_pc >> 8); b[7] = m_stopped;
    std::memcpy(b + 8, &m_cycles, 8);
}

void Spc700::loadState(const uint8_t* b) {
    m_a = b[0]; m_x = b[1]; m_y = b[2]; m_sp = b[3]; m_psw = b[4];
    m_pc = (uint16_t)(b[5] | b[6] << 8); m_stopped = b[7];
    std::memcpy(&m_cycles, b + 8, 8);
}
