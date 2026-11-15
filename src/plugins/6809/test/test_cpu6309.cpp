// Hitachi HD6309 core (#332). Cycle counts are checked against Darren
// Atkinson's "6809 and 6309 Programming Reference" quick-reference table
// (emulation / native); emulation mode is also checked against the 6809 core
// instruction by instruction on random programs.

#include "test_harness.h"
#include "plugins/6809/main/cpu6309.h"
#include "libmem/main/memory_bus.h"
#include "libtoolchain/main/idisasm.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {
struct Rig6309 {
    FlatMemoryBus mem{"system", 16};
    HD6309 cpu;
    Rig6309() { cpu.setDataBus(&mem); }
    void poke16(uint16_t at, uint16_t v) { mem.write8(at, v >> 8); mem.write8((uint16_t)(at + 1), v & 0xFF); }
    void load(uint16_t at, std::vector<uint8_t> code) {
        for (size_t i = 0; i < code.size(); ++i) mem.write8((uint32_t)(at + i), code[i]);
        poke16(0xFFFE, at);
        cpu.reset();
    }
    int run(int n) { int t = 0; for (int i = 0; i < n; ++i) t += cpu.step(); return t; }
    void set(int reg, uint32_t v) { cpu.regWrite(reg, v); }
    uint32_t get(int reg) const { return cpu.regRead(reg); }
    std::string dis(uint16_t at) { char b[64]; cpu.disassembleOne(&mem, at, b, sizeof b); return b; }
};

// One instruction at $1000 with X=$3000 Y=$3100 U=$3200 S=$8000 W=$3300
// DP=$20, in emulation or native mode: its cycles
int cyclesOf(std::vector<uint8_t> code, bool native, uint8_t cc = 0, uint16_t dReg = 0) {
    Rig6309 r;
    for (uint32_t a = 0x2000; a < 0x4000; ++a) r.mem.write8(a, 0x02);   // operands: never a zero divisor
    r.load(0x1000, code);
    r.set(HD6309::R_X, 0x3000); r.set(HD6309::R_Y, 0x3100); r.set(HD6309::R_U, 0x3200);
    r.set(HD6309::R_S, 0x8000); r.set(HD6309::R_W, 0x3300); r.set(HD6309::R_DP, 0x20);
    r.set(HD6309::R_D, dReg);
    r.set(HD6309::R_CC, cc | M6809::CC_I | M6809::CC_F);
    r.set(HD6309::R_MD, native ? HD6309::MD_NM : 0);
    return r.cpu.step();
}

uint32_t s_rng = 1;
uint32_t rnd() { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }
}  // namespace

// Registers: the 6809's first, then E F W Q V MD; Q is D:W
TEST_CASE(hd6309_registers) {
    Rig6309 r;
    EXPECT_EQ(std::string(r.cpu.isaName()), std::string("6309"));
    EXPECT_EQ(r.cpu.regCount(), (int)HD6309::R6309_COUNT);
    const char* names[] = {"A", "B", "D", "X", "Y", "U", "S", "PC", "DP", "CC", "E", "F", "W", "Q", "V", "MD"};
    for (int i = 0; i < r.cpu.regCount(); ++i) EXPECT_EQ(std::string(r.cpu.regDescriptor(i)->name), std::string(names[i]));
    r.set(HD6309::R_Q, 0x12345678);
    EXPECT_EQ(r.cpu.d(), 0x1234);
    EXPECT_EQ(r.cpu.w(), 0x5678);
    EXPECT_EQ(r.get(HD6309::R_E), 0x56u);
    EXPECT_EQ(r.get(HD6309::R_F), 0x78u);
    r.set(HD6309::R_E, 0xAB);
    EXPECT_EQ(r.get(HD6309::R_Q), 0x1234AB78u);
    r.set(HD6309::R_V, 0xBEEF);
    EXPECT_EQ(r.get(HD6309::R_V), 0xBEEFu);
    EXPECT_EQ(r.cpu.regReadByName("W"), 0xAB78u);
    r.set(HD6309::R_MD, HD6309::MD_NM);
    EXPECT_TRUE(r.cpu.nativeMode());
    r.load(0x1000, {0x12});                          // /RESET returns to emulation mode
    EXPECT_TRUE(!r.cpu.nativeMode());
    EXPECT_EQ(r.get(HD6309::R_MD), 0u);
}

// Emulation mode runs 6809 code exactly as the 6809 core: random documented
// instructions (no SYNC/CWAI/DAA, no undefined opcodes or postbytes, TFR/EXG
// between same-size registers), registers, memory and cycles compared
TEST_CASE(hd6309_emulation_matches_6809) {
    auto valid0 = [](uint8_t op) {
        const int hi = op >> 4, lo = op & 15;
        if (hi == 0x0 || hi == 0x6 || hi == 0x7) return lo != 1 && lo != 2 && lo != 5 && lo != 0xB;
        if (hi == 0x4 || hi == 0x5) return lo != 1 && lo != 2 && lo != 5 && lo != 0xB && lo != 0xE;
        // (not DAA: the 6309's corrections differ, see hd6309_daa)
        if (hi == 0x1) return lo == 2 || lo == 6 || lo == 7 || lo == 0xA || lo == 0xC || lo == 0xD || lo == 0xE || lo == 0xF;
        if (hi == 0x2) return true;
        if (hi == 0x3) return lo != 8 && lo != 0xC && lo != 0xE;
        return op != 0x87 && op != 0x8F && op != 0xC7 && op != 0xCD && op != 0xCF;
    };
    auto valid2 = [](uint8_t op) {
        if (op >= 0x21 && op <= 0x2F) return true;
        switch (op) {
            case 0x3F: case 0x83: case 0x8C: case 0x8E: case 0x93: case 0x9C: case 0x9E: case 0x9F:
            case 0xA3: case 0xAC: case 0xAE: case 0xAF: case 0xB3: case 0xBC: case 0xBE: case 0xBF:
            case 0xCE: case 0xDE: case 0xDF: case 0xEE: case 0xEF: case 0xFE: case 0xFF: return true;
        }
        return false;
    };
    auto valid3 = [](uint8_t op) {
        switch (op) {
            case 0x3F: case 0x83: case 0x8C: case 0x93: case 0x9C: case 0xA3: case 0xAC: case 0xB3: case 0xBC: return true;
        }
        return false;
    };
    auto postbyte = []() -> uint8_t {
        for (;;) {
            const uint8_t pb = (uint8_t)rnd();
            if (!(pb & 0x80)) return pb;
            const int lo = pb & 15;
            if (lo == 7 || lo == 0xA || lo == 0xE) continue;
            if (lo == 0xF) { if (pb == 0x9F) return pb; continue; }   // $BF $DF $FF trap on the 6309
            if ((pb & 0x1F) == 0x10 || (pb & 0x1F) == 0x12) continue;
            return pb;
        }
    };
    for (uint32_t seed : {1u, 0xC0C03u, 0x6309u, 0xBEEFu}) {
        s_rng = seed;
        FlatMemoryBus m1{"a", 16}, m2{"b", 16};
        M6809 c1;
        HD6309 c2;
        c1.setDataBus(&m1); c2.setDataBus(&m2);
        for (uint32_t a = 0; a < 0x10000; ++a) { const uint8_t v = (uint8_t)rnd(); m1.write8(a, v); m2.write8(a, v); }
        for (int reg = 0; reg < M6809::R_COUNT; ++reg) {
            if (reg == M6809::R_D) continue;
            const uint32_t v = rnd() & (reg == M6809::R_CC ? 0x0F : 0xFFFF);
            c1.regWrite(reg, v); c2.regWrite(reg, v);
        }
        for (int i = 0; i < 6000; ++i) {
            uint8_t code[6];
            int n = 0, page = 0;
            uint8_t op;
            do { op = (uint8_t)rnd(); } while (!(op == 0x10 || op == 0x11 || valid0(op)));
            if (op == 0x10 || op == 0x11) {
                page = op;
                code[n++] = op;
                do { op = (uint8_t)rnd(); } while (!(page == 0x10 ? valid2(op) : valid3(op)));
            }
            code[n++] = op;
            const int hi = op >> 4;
            if (hi == 0x6 || hi == 0xA || hi == 0xE || (page == 0 && op >= 0x30 && op <= 0x33)) code[n++] = postbyte();
            else if (page == 0 && (op == 0x1E || op == 0x1F)) {
                const uint8_t r16[6] = {0, 1, 2, 3, 4, 5}, r8[4] = {8, 9, 10, 11};
                code[n++] = (rnd() & 1) ? (uint8_t)(r16[rnd() % 6] << 4 | r16[rnd() % 6])
                                        : (uint8_t)(r8[rnd() % 4] << 4 | r8[rnd() % 4]);
            }
            while (n < 6) code[n++] = (uint8_t)rnd();
            const uint16_t pc = (uint16_t)c1.pc();
            for (int k = 0; k < 6; ++k) { m1.write8((uint16_t)(pc + k), code[k]); m2.write8((uint16_t)(pc + k), code[k]); }
            const int t1 = c1.step(), t2 = c2.step();
            bool same = t1 == t2;
            for (int reg = 0; reg < M6809::R_COUNT; ++reg) same = same && c1.regRead(reg) == c2.regRead(reg);
            if (!same) {
                char b[64];
                c1.disassembleOne(&m1, pc, b, sizeof b);
                std::fprintf(stderr, "seed %u step %d at $%04X: %s  cycles %d/%d\n", seed, i, pc, b, t1, t2);
            }
            EXPECT_TRUE(same);
            if (i % 500 == 0) {
                bool memSame = true;
                for (uint32_t a = 0; a < 0x10000 && memSame; ++a) memSame = m1.peek8(a) == m2.peek8(a);
                EXPECT_TRUE(memSame);
            }
        }
    }
}

// Emulation / native cycle counts against Atkinson's table
TEST_CASE(hd6309_cycle_counts) {
    struct C { std::vector<uint8_t> code; int emu, nat; uint8_t cc; uint16_t d; };
    const std::vector<C> table = {
        {{0x96, 0x10}, 4, 3, 0, 0},                          // LDA <
        {{0xFC, 0x20, 0x00}, 6, 5, 0, 0},                    // LDD ext
        {{0x12}, 2, 1, 0, 0},                                // NOP
        {{0x34, 0x06}, 7, 6, 0, 0},                          // PSHS A,B
        {{0x35, 0x06}, 7, 6, 0, 0},                          // PULS A,B
        {{0x39}, 5, 4, 0, 0},                                // RTS
        {{0x16, 0x00, 0x00}, 5, 4, 0, 0},                    // LBRA
        {{0x17, 0x00, 0x00}, 9, 7, 0, 0},                    // LBSR
        {{0x3D}, 11, 10, 0, 0},                              // MUL
        {{0x1F, 0x12}, 6, 4, 0, 0},                          // TFR
        {{0x1E, 0x12}, 8, 5, 0, 0},                          // EXG
        {{0x3A}, 3, 1, 0, 0},                                // ABX
        {{0x1D}, 2, 1, 0, 0},                                // SEX
        {{0x19}, 2, 1, 0, 0},                                // DAA
        {{0x1A, 0x00}, 3, 3, 0, 0},                          // ORCC (Atkinson, hoglet67)
        {{0x1C, 0xFF}, 3, 3, 0, 0},                          // ANDCC
        {{0xAD, 0x84}, 7, 6, 0, 0},                          // JSR ,X (hoglet67)
        {{0xAD, 0x80}, 9, 7, 0, 0},                          // JSR ,X+
        {{0x6B, 0x0F, 0xE0}, 7, 6, 0, 0},                    // TIM ,S+ (hoglet67's capture: 7)
        {{0x83, 0x00, 0x01}, 4, 3, 0, 0},                    // SUBD #
        {{0x9C, 0x10}, 6, 4, 0, 0},                          // CMPX <
        {{0xA6, 0x84}, 4, 4, 0, 0},                          // LDA ,X
        {{0xA6, 0x80}, 6, 5, 0, 0},                          // LDA ,X+
        {{0xA6, 0x81}, 7, 6, 0, 0},                          // LDA ,X++
        {{0xA6, 0x05}, 5, 5, 0, 0},                          // LDA 5,X
        {{0xA6, 0x89, 0x00, 0x10}, 8, 7, 0, 0},              // LDA $10,X
        {{0xA6, 0x8B}, 8, 6, 0, 0},                          // LDA D,X
        {{0xA6, 0x86}, 5, 5, 0, 0},                          // LDA A,X
        {{0xA6, 0x9F, 0x20, 0x00}, 9, 8, 0, 0},              // LDA [$2000]
        {{0xA6, 0x8D, 0x00, 0x00}, 9, 7, 0, 0},              // LDA $0000,PCR
        {{0xA6, 0x94}, 7, 7, 0, 0},                          // LDA [,X]
        {{0xA6, 0x91}, 10, 9, 0, 0},                         // LDA [,X++]
        {{0x4F}, 2, 1, 0, 0},                                // CLRA
        {{0x00, 0x10}, 6, 5, 0, 0},                          // NEG <
        {{0x0D, 0x10}, 6, 4, 0, 0},                          // TST <
        {{0x7D, 0x20, 0x00}, 7, 5, 0, 0},                    // TST ext
        {{0x6D, 0x84}, 6, 5, 0, 0},                          // TST ,X
        {{0x0E, 0x10}, 3, 2, 0, 0},                          // JMP <
        {{0xBD, 0x20, 0x00}, 8, 7, 0, 0},                    // JSR ext
        {{0x9D, 0x10}, 7, 6, 0, 0},                          // JSR <
        {{0x8D, 0x00}, 7, 6, 0, 0},                          // BSR
        {{0x26, 0x00}, 3, 3, 0, 0},                          // BNE
        {{0x10, 0x27, 0x00, 0x00}, 6, 5, M6809::CC_Z, 0},    // LBEQ taken
        {{0x10, 0x27, 0x00, 0x00}, 5, 5, 0, 0},              // LBEQ not taken
        {{0x10, 0xCE, 0x80, 0x00}, 4, 4, 0, 0},              // LDS #
        {{0x10, 0x83, 0x00, 0x00}, 5, 4, 0, 0},              // CMPD #
        {{0x11, 0x93, 0x10}, 7, 5, 0, 0},                    // CMPU <
        {{0x3F}, 19, 21, 0, 0},                              // SWI
        {{0x10, 0x3F}, 20, 22, 0, 0},                        // SWI2
        {{0x11, 0x3F}, 20, 22, 0, 0},                        // SWI3
        // 6309 additions
        {{0x10, 0xDC, 0x10}, 8, 7, 0, 0},                    // LDQ <
        {{0x10, 0xED, 0x84}, 8, 8, 0, 0},                    // STQ ,X
        {{0xCD, 0x00, 0x00, 0x00, 0x00}, 5, 5, 0, 0},        // LDQ #
        {{0x10, 0x86, 0x00, 0x01}, 4, 4, 0, 0},              // LDW #
        {{0x10, 0x97, 0x10}, 6, 5, 0, 0},                    // STW <
        {{0x10, 0x8B, 0x00, 0x01}, 5, 4, 0, 0},              // ADDW #
        {{0x10, 0xB0, 0x20, 0x00}, 8, 6, 0, 0},              // SUBW ext
        {{0x10, 0x99, 0x10}, 7, 5, 0, 0},                    // ADCD <
        {{0x10, 0x4A}, 3, 2, 0, 0},                          // DECD
        {{0x10, 0x5D}, 3, 2, 0, 0},                          // TSTW
        {{0x11, 0x4D}, 3, 2, 0, 0},                          // TSTE (Atkinson, MAME)
        {{0x11, 0x5C}, 3, 2, 0, 0},                          // INCF
        {{0x11, 0x8B, 0x01}, 3, 3, 0, 0},                    // ADDE #
        {{0x11, 0xD6, 0x10}, 5, 4, 0, 0},                    // LDF <
        {{0x11, 0xB7, 0x20, 0x00}, 6, 5, 0, 0},              // STE ext
        {{0x11, 0xA0, 0x84}, 5, 5, 0, 0},                    // SUBE ,X
        {{0x14}, 4, 4, 0, 0},                                // SEXW
        {{0x10, 0x30, 0x12}, 4, 4, 0, 0},                    // ADDR X,Y
        {{0x10, 0x37, 0x89}, 4, 4, 0, 0},                    // CMPR A,B
        {{0x10, 0x38}, 6, 6, 0, 0},                          // PSHSW
        {{0x10, 0x3B}, 6, 6, 0, 0},                          // PULUW
        {{0x11, 0x3D, 0x00}, 5, 5, 0, 0},                    // LDMD
        {{0x11, 0x3C, 0x00}, 4, 4, 0, 0},                    // BITMD
        {{0x11, 0x8D, 0x02}, 25, 25, 0, 100},                // DIVD #
        {{0x11, 0x9D, 0x10}, 27, 26, 0, 100},                // DIVD <
        {{0x11, 0x8E, 0x00, 0x02}, 34, 34, 0, 0},            // DIVQ #
        {{0x11, 0x8F, 0x00, 0x02}, 28, 28, 0, 0},            // MULD #
        {{0x11, 0x9F, 0x10}, 30, 29, 0, 0},                  // MULD <
        {{0x02, 0x0F, 0x10}, 6, 6, 0, 0},                    // AIM <
        {{0x62, 0x0F, 0x84}, 7, 7, 0, 0},                    // AIM ,X
        {{0x72, 0x0F, 0x20, 0x00}, 7, 7, 0, 0},              // AIM ext
        {{0x01, 0x0F, 0x10}, 6, 6, 0, 0},                    // OIM <
        {{0x0B, 0x0F, 0x10}, 4, 4, 0, 0},                    // TIM < (hoglet67)
        {{0x7B, 0x0F, 0x20, 0x00}, 5, 5, 0, 0},              // TIM ext
        {{0x11, 0x30, 0x40, 0x10}, 7, 6, 0, 0},              // BAND
        {{0x11, 0x36, 0x40, 0x10}, 7, 6, 0, 0},              // LDBT
        {{0x11, 0x37, 0x40, 0x10}, 8, 7, 0, 0},              // STBT
        {{0x15}, 20, 22, 0, 0},                              // illegal
        {{0x11, 0x38, 0x12}, 6, 6, 0, 0},                    // TFM: the setup
        // W indexed modes and E/F/W offsets (base LDA indexed 4)
        {{0xA6, 0x8F}, 4, 4, 0, 0},                          // ,W
        {{0xA6, 0x90}, 7, 7, 0, 0},                          // [,W]
        {{0xA6, 0xAF, 0x00, 0x10}, 6, 6, 0, 0},              // n,W
        {{0xA6, 0xB0, 0x00, 0x10}, 9, 9, 0, 0},              // [n,W]
        {{0xA6, 0xCF}, 5, 5, 0, 0},                          // ,W++
        {{0xA6, 0xD0}, 8, 8, 0, 0},                          // [,W++]
        {{0xA6, 0xEF}, 5, 5, 0, 0},                          // ,--W
        {{0xA6, 0x87}, 5, 5, 0, 0},                          // E,X
        {{0xA6, 0x8A}, 5, 5, 0, 0},                          // F,X
        {{0xA6, 0x8E}, 5, 5, 0, 0},                          // W,X
        {{0xA6, 0x97}, 8, 8, 0, 0},                          // [E,X]
    };
    for (const auto& c : table) {
        for (int native = 0; native < 2; ++native) {
            const int got = cyclesOf(c.code, native, c.cc, c.d);
            const int want = native ? c.nat : c.emu;
            if (got != want) {
                std::fprintf(stderr, "opcode");
                for (uint8_t b : c.code) std::fprintf(stderr, " %02X", b);
                std::fprintf(stderr, " (%s): %d cycles, expected %d\n", native ? "native" : "emulation", got, want);
            }
            EXPECT_EQ(got, want);
        }
    }
}

// The new accumulators: E, F, W, D inherent and memory ops, LDQ/STQ
TEST_CASE(hd6309_accumulators) {
    Rig6309 r;
    r.mem.write8(0x2010, 0x12); r.mem.write8(0x2011, 0x34); r.mem.write8(0x2012, 0x56); r.mem.write8(0x2013, 0x78);
    r.load(0x1000, {0x11, 0x86, 0x7F,             // LDE #$7F
                    0x11, 0x8B, 0x01,             // ADDE #1     -> $80 N V H
                    0x11, 0xC6, 0x10,             // LDF #$10
                    0x11, 0xC0, 0x11,             // SUBF #$11   -> $FF N C
                    0x11, 0x81, 0x80,             // CMPE #$80   -> Z
                    0x11, 0x4C,                   // INCE        -> $81
                    0x11, 0x5A,                   // DECF        -> $FE
                    0x10, 0x81, 0x81, 0xFE,       // CMPW #$81FE -> Z
                    0x10, 0x8B, 0x7E, 0x02,       // ADDW #$7E02 -> $0000 Z C
                    0x10, 0x80, 0x00, 0x01,       // SUBW #1     -> $FFFF N C
                    0x10, 0x54,                   // LSRW        -> $7FFF C
                    0x10, 0x59,                   // ROLW        -> $FFFF (C in) N V
                    0x10, 0x4F,                   // CLRD
                    0x10, 0x43,                   // COMD        -> $FFFF C
                    0x10, 0x40,                   // NEGD        -> $0001 C
                    0x10, 0x47,                   // ASRD        -> 0 Z C
                    0x86, 0x20,                   // LDA #$20
                    0x1F, 0x8B,                   // TFR A,DP
                    0x10, 0xDC, 0x10,             // LDQ <$10    -> $12345678
                    0x10, 0xDD, 0x20,             // STQ <$20
                    0x11, 0x97, 0x30,             // STE <$30
                    0x10, 0x5F});                 // CLRW
    r.run(2);
    EXPECT_EQ(r.cpu.e(), 0x80);
    EXPECT_EQ(r.cpu.cc() & 0x2F, M6809::CC_N | M6809::CC_V | M6809::CC_H);
    r.run(2);
    EXPECT_EQ(r.cpu.f(), 0xFF);
    EXPECT_EQ(r.cpu.cc() & 0x0F, M6809::CC_N | M6809::CC_C);
    r.run(1);
    EXPECT_TRUE(r.cpu.cc() & M6809::CC_Z);
    r.run(2);
    EXPECT_EQ(r.cpu.w(), 0x81FE);
    r.run(1);
    EXPECT_TRUE(r.cpu.cc() & M6809::CC_Z);
    r.run(1);
    EXPECT_EQ(r.cpu.w(), 0x0000);
    EXPECT_EQ(r.cpu.cc() & 0x0F, M6809::CC_Z | M6809::CC_C);
    r.run(1);
    EXPECT_EQ(r.cpu.w(), 0xFFFF);
    EXPECT_EQ(r.cpu.cc() & 0x0F, M6809::CC_N | M6809::CC_C);
    r.run(1);
    EXPECT_EQ(r.cpu.w(), 0x7FFF);
    EXPECT_EQ(r.cpu.cc() & 0x0F, M6809::CC_C);
    r.run(1);
    EXPECT_EQ(r.cpu.w(), 0xFFFF);
    EXPECT_EQ(r.cpu.cc() & 0x0F, M6809::CC_N | M6809::CC_V);
    r.run(2);
    EXPECT_EQ(r.cpu.d(), 0xFFFF);
    EXPECT_EQ(r.cpu.cc() & 0x0F, M6809::CC_N | M6809::CC_C);
    r.run(1);
    EXPECT_EQ(r.cpu.d(), 0x0001);
    EXPECT_EQ(r.cpu.cc() & 0x0F, M6809::CC_C);
    r.run(1);
    EXPECT_EQ(r.cpu.d(), 0x0000);
    EXPECT_EQ(r.cpu.cc() & 0x0F, M6809::CC_Z | M6809::CC_C);
    r.run(3);
    EXPECT_EQ(r.cpu.q(), 0x12345678u);
    EXPECT_EQ(r.cpu.cc() & 0x0E, 0);
    r.run(2);
    EXPECT_EQ(r.mem.peek8(0x2020), 0x12); EXPECT_EQ(r.mem.peek8(0x2023), 0x78);
    EXPECT_EQ(r.mem.peek8(0x2030), 0x56);
    r.run(1);
    EXPECT_EQ(r.cpu.w(), 0);
    EXPECT_EQ(r.cpu.d(), 0x1234);
}

// LDQ #: Z from D alone, V untouched (hoglet67); SEXW
TEST_CASE(hd6309_ldq_sexw) {
    Rig6309 r;
    r.load(0x1000, {0xCD, 0x00, 0x00, 0x12, 0x34,  // LDQ #$00001234
                    0x14,                          // SEXW (W=$1234) -> D=0, Z clear
                    0x10, 0x86, 0x80, 0x00,        // LDW #$8000
                    0x14,                          // SEXW -> D=$FFFF N
                    0x10, 0x5F,                    // CLRW
                    0x14});                        // SEXW -> Q=0 Z
    r.set(HD6309::R_CC, M6809::CC_V);
    r.run(1);
    EXPECT_EQ(r.cpu.q(), 0x00001234u);
    EXPECT_EQ(r.cpu.cc() & 0x0E, M6809::CC_Z | M6809::CC_V);
    r.run(1);
    EXPECT_EQ(r.cpu.d(), 0x0000);
    EXPECT_TRUE(!(r.cpu.cc() & M6809::CC_Z));
    r.run(2);
    EXPECT_EQ(r.cpu.d(), 0xFFFF);
    EXPECT_TRUE(r.cpu.cc() & M6809::CC_N);
    r.run(2);
    EXPECT_EQ(r.cpu.d(), 0x0000);
    EXPECT_TRUE(r.cpu.cc() & M6809::CC_Z);
    EXPECT_TRUE(!(r.cpu.cc() & M6809::CC_N));
}

// ADDR ADCR SUBR SBCR ANDR ORR EORR CMPR, mixed sizes, the zero register,
// CC as the destination
TEST_CASE(hd6309_inter_register) {
    Rig6309 r;
    r.load(0x1000, {0x10, 0x30, 0x12,       // ADDR X,Y
                    0x10, 0x30, 0x89,       // ADDR A,B
                    0x10, 0x30, 0x81,       // ADDR A,X  (A as D)
                    0x10, 0x30, 0x18,       // ADDR X,A  (low byte of X)
                    0x10, 0x31, 0x89,       // ADCR A,B  (C from the last)
                    0x10, 0x32, 0x12,       // SUBR X,Y
                    0x10, 0x33, 0x89,       // SBCR A,B
                    0x10, 0x34, 0x89,       // ANDR A,B
                    0x10, 0x35, 0x89,       // ORR A,B
                    0x10, 0x36, 0x89,       // EORR A,B
                    0x10, 0x37, 0x12,       // CMPR X,Y
                    0x10, 0x30, 0xC1,       // ADDR 0,X
                    0x10, 0x34, 0x8A,       // ANDR A,CC
                    0x1F, 0xC2});           // TFR 0,Y
    r.set(HD6309::R_X, 0x1234); r.set(HD6309::R_Y, 0x0100);
    r.set(HD6309::R_A, 0x10); r.set(HD6309::R_B, 0x20);
    r.run(1);
    EXPECT_EQ(r.cpu.y(), 0x1334);
    r.run(1);
    EXPECT_EQ(r.cpu.b(), 0x30);
    r.run(1);
    EXPECT_EQ(r.cpu.x(), (uint16_t)(0x1234 + 0x1030));
    r.set(HD6309::R_X, 0x12FF); r.set(HD6309::R_A, 0x01);
    r.run(1);
    EXPECT_EQ(r.cpu.a(), 0x00);
    EXPECT_EQ(r.cpu.cc() & 0x05, M6809::CC_Z | M6809::CC_C);
    r.set(HD6309::R_A, 0x01); r.set(HD6309::R_B, 0x01);
    r.run(1);
    EXPECT_EQ(r.cpu.b(), 0x03);                      // 1 + 1 + C
    r.set(HD6309::R_X, 0x0001); r.set(HD6309::R_Y, 0x0000);
    r.run(1);
    EXPECT_EQ(r.cpu.y(), 0xFFFF);                    // Y - X
    EXPECT_TRUE(r.cpu.cc() & M6809::CC_C);
    r.set(HD6309::R_A, 0x01); r.set(HD6309::R_B, 0x05);
    r.run(1);
    EXPECT_EQ(r.cpu.b(), 0x03);                      // 5 - 1 - C
    r.set(HD6309::R_A, 0x0C); r.set(HD6309::R_B, 0x0A);
    r.run(1);
    EXPECT_EQ(r.cpu.b(), 0x08);
    r.run(1);
    EXPECT_EQ(r.cpu.b(), 0x0C);
    r.run(1);
    EXPECT_EQ(r.cpu.b(), 0x00);
    EXPECT_TRUE(r.cpu.cc() & M6809::CC_Z);
    r.set(HD6309::R_X, 0x0005); r.set(HD6309::R_Y, 0x0005);
    r.set(HD6309::R_CC, 0);
    r.run(1);
    EXPECT_EQ(r.cpu.cc() & 0x0F, M6809::CC_Z);
    EXPECT_EQ(r.cpu.y(), 0x0005);
    r.run(1);
    EXPECT_EQ(r.cpu.x(), 0x0005);
    // ANDR A,CC with A=0: the result (0) is stored, then Z is set over it
    r.set(HD6309::R_A, 0x00); r.set(HD6309::R_CC, 0x0F);
    r.run(1);
    EXPECT_EQ(r.cpu.cc(), M6809::CC_Z);
    r.run(1);
    EXPECT_EQ(r.cpu.y(), 0x0000);
}

// TFR/EXG: E F W V, the zero register, 8 <-> 16 bits as the 6309 does it
TEST_CASE(hd6309_tfr_exg) {
    Rig6309 r;
    r.load(0x1000, {0x1F, 0x81,             // TFR A,X  -> A:A
                    0x1F, 0x28,             // TFR Y,A  -> high byte
                    0x1F, 0x29,             // TFR Y,B  -> low byte
                    0x1F, 0x2E,             // TFR Y,E  -> high byte
                    0x1F, 0x2F,             // TFR Y,F  -> low byte
                    0x1F, 0x67,             // TFR W,V
                    0x1E, 0x16,             // EXG X,W
                    0x1F, 0xC3,             // TFR 0,U
                    0x1F, 0x1C});           // TFR X,0 (discarded)
    r.set(HD6309::R_A, 0x12); r.set(HD6309::R_Y, 0xABCD); r.set(HD6309::R_U, 0x5555);
    r.run(1);
    EXPECT_EQ(r.cpu.x(), 0x1212);
    r.run(4);
    EXPECT_EQ(r.cpu.a(), 0xAB); EXPECT_EQ(r.cpu.b(), 0xCD);
    EXPECT_EQ(r.cpu.e(), 0xAB); EXPECT_EQ(r.cpu.f(), 0xCD);
    r.run(1);
    EXPECT_EQ(r.cpu.v(), 0xABCD);
    r.run(1);
    EXPECT_EQ(r.cpu.w(), 0x1212); EXPECT_EQ(r.cpu.x(), 0xABCD);
    r.run(2);
    EXPECT_EQ(r.cpu.u(), 0x0000);
    EXPECT_EQ(r.cpu.x(), 0xABCD);
}

// PSHSW PULSW PSHUW PULUW: E at the lower address
TEST_CASE(hd6309_push_pull_w) {
    Rig6309 r;
    r.load(0x1000, {0x10, 0x38,             // PSHSW
                    0x10, 0x5F,             // CLRW
                    0x10, 0x39,             // PULSW
                    0x10, 0x3A,             // PSHUW
                    0x10, 0x5F,
                    0x10, 0x3B});           // PULUW
    r.set(HD6309::R_S, 0x8000); r.set(HD6309::R_U, 0x7000); r.set(HD6309::R_W, 0x1234);
    r.run(1);
    EXPECT_EQ(r.cpu.sp(), 0x7FFEu);
    EXPECT_EQ(r.mem.peek8(0x7FFE), 0x12); EXPECT_EQ(r.mem.peek8(0x7FFF), 0x34);
    r.run(2);
    EXPECT_EQ(r.cpu.w(), 0x1234); EXPECT_EQ(r.cpu.sp(), 0x8000u);
    r.run(1);
    EXPECT_EQ(r.cpu.u(), 0x6FFE);
    EXPECT_EQ(r.mem.peek8(0x6FFE), 0x12);
    r.run(2);
    EXPECT_EQ(r.cpu.w(), 0x1234); EXPECT_EQ(r.cpu.u(), 0x7000);
}

// The W indexed modes and the E/F/W accumulator offsets
TEST_CASE(hd6309_w_indexing) {
    Rig6309 r;
    for (int i = 0; i < 0x40; ++i) r.mem.write8(0x3000 + i, (uint8_t)(0x80 + i));
    r.poke16(0x3020, 0x3031);                        // pointer at $3020 for [,W]
    r.load(0x1000, {0xA6, 0x8F,             // LDA ,W        $3000
                    0xA6, 0xCF,             // LDA ,W++      $3000, W=$3002
                    0xA6, 0xEF,             // LDA ,--W      W=$3000
                    0xA6, 0xAF, 0x00, 0x10, // LDA $10,W     $3010
                    0x10, 0x86, 0x30, 0x20, // LDW #$3020
                    0xA6, 0x90,             // LDA [,W]      $3031
                    0xA6, 0x87,             // LDA E,X       X + sext(E=$30)
                    0xA6, 0x8A,             // LDA F,X       X + sext(F=$20)
                    0xA6, 0x8E,             // LDA W,X       X + $3020
                    0x30, 0xCF});           // LEAX ,W++
    r.set(HD6309::R_W, 0x3000); r.set(HD6309::R_X, 0x0000);
    r.run(1); EXPECT_EQ(r.cpu.a(), 0x80);
    r.run(1); EXPECT_EQ(r.cpu.a(), 0x80); EXPECT_EQ(r.cpu.w(), 0x3002);
    r.run(1); EXPECT_EQ(r.cpu.a(), 0x80); EXPECT_EQ(r.cpu.w(), 0x3000);
    r.run(1); EXPECT_EQ(r.cpu.a(), 0x90);
    r.run(2); EXPECT_EQ(r.cpu.a(), 0xB1);
    r.set(HD6309::R_X, 0x2FF0);
    r.run(1); EXPECT_EQ(r.cpu.a(), r.mem.peek8(0x3020));
    r.run(1); EXPECT_EQ(r.cpu.a(), r.mem.peek8(0x3010));
    r.set(HD6309::R_X, 0x0001);
    r.run(1); EXPECT_EQ(r.cpu.a(), r.mem.peek8(0x3021));
    r.run(1); EXPECT_EQ(r.cpu.x(), 0x3020); EXPECT_EQ(r.cpu.w(), 0x3022);
}

// MULD, DIVD, DIVQ: signs, range and two's-complement overflow, divide by zero
TEST_CASE(hd6309_multiply_divide) {
    auto divd = [](uint16_t d, uint8_t div, int* cyc) {
        Rig6309* r = new Rig6309();
        r->load(0x1000, {0x11, 0x8D, div});
        r->set(HD6309::R_D, d);
        *cyc = r->cpu.step();
        return r;
    };
    int c;
    Rig6309* r = divd(100, 7, &c);
    EXPECT_EQ(r->cpu.b(), 14); EXPECT_EQ(r->cpu.a(), 2);
    EXPECT_EQ(r->cpu.cc() & 0x0F, 0); EXPECT_EQ(c, 25);
    delete r;
    r = divd(0xFF9C, 7, &c);                         // -100 / 7 = -14 rem -2
    EXPECT_EQ(r->cpu.b(), 0xF2); EXPECT_EQ(r->cpu.a(), 0xFE);
    EXPECT_EQ(r->cpu.cc() & 0x0F, M6809::CC_N); EXPECT_EQ(c, 26);
    delete r;
    r = divd(100, 0xF9, &c);                         // 100 / -7
    EXPECT_EQ(r->cpu.b(), 0xF2); EXPECT_EQ(r->cpu.a(), 0x02); EXPECT_EQ(c, 26);
    delete r;
    r = divd(1000, 2, &c);                           // range overflow: D left alone
    EXPECT_EQ(r->cpu.d(), 1000); EXPECT_EQ(r->cpu.cc() & 0x0F, M6809::CC_V); EXPECT_EQ(c, 12);
    delete r;
    r = divd((uint16_t)-1000, 2, &c);                // range overflow, negative dividend: D = |D|, N
    EXPECT_EQ(r->cpu.d(), 1000); EXPECT_EQ(r->cpu.cc() & 0x0F, M6809::CC_N | M6809::CC_V);
    delete r;
    r = divd(256, 2, &c);                            // 128: two's-complement overflow, a cycle fewer
    EXPECT_EQ(r->cpu.b(), 0x80); EXPECT_EQ(r->cpu.cc() & 0x0F, M6809::CC_N | M6809::CC_V); EXPECT_EQ(c, 24);
    delete r;
    r = divd(7, 2, &c);                              // C is bit 0 of the quotient
    EXPECT_EQ(r->cpu.b(), 3); EXPECT_EQ(r->cpu.a(), 1); EXPECT_EQ(r->cpu.cc() & 0x0F, M6809::CC_C);
    delete r;

    Rig6309 q;
    q.load(0x1000, {0x11, 0x8E, 0x00, 0x03,           // DIVQ #3
                    0x11, 0x8E, 0x00, 0x02,           // DIVQ #2
                    0x11, 0x8F, 0x00, 0x03,           // MULD #3
                    0x11, 0x8F, 0x00, 0x03});         // MULD #3
    q.set(HD6309::R_Q, 0x00010000);
    EXPECT_EQ(q.run(1), 34);
    EXPECT_EQ(q.cpu.w(), 0x5555); EXPECT_EQ(q.cpu.d(), 1);
    EXPECT_EQ(q.cpu.cc() & 0x0F, M6809::CC_C);
    q.set(HD6309::R_Q, 0xFFFFFFF9);                  // -7 / 2 = -3 rem -1
    q.run(1);
    EXPECT_EQ(q.cpu.w(), 0xFFFD); EXPECT_EQ(q.cpu.d(), 0xFFFF);
    EXPECT_EQ(q.cpu.cc() & 0x0F, M6809::CC_N | M6809::CC_C);
    q.set(HD6309::R_D, 0xFFFE);                      // -2 * 3 = -6
    q.set(HD6309::R_CC, M6809::CC_C);
    q.run(1);
    EXPECT_EQ(q.cpu.q(), 0xFFFFFFFAu);
    EXPECT_EQ(q.cpu.cc() & 0x0D, M6809::CC_N);       // C cleared
    q.set(HD6309::R_D, 0x0002);                      // 2 * 3: D = 0, so Z although W = 6
    q.run(1);
    EXPECT_EQ(q.cpu.q(), 6u);
    EXPECT_EQ(q.cpu.cc() & 0x0C, M6809::CC_Z);
}

// Divide by zero: MD /0, the entire state stacked, the vector at $FFF0;
// BITMD reads and clears it
TEST_CASE(hd6309_divide_by_zero_trap) {
    for (int which = 0; which < 2; ++which) {
        Rig6309 r;
        r.poke16(0xFFF0, 0x2800);
        r.mem.write8(0x2800, 0x11); r.mem.write8(0x2801, 0x3C); r.mem.write8(0x2802, 0x80);   // BITMD #$80
        r.mem.write8(0x2803, 0x11); r.mem.write8(0x2804, 0x3C); r.mem.write8(0x2805, 0x80);
        if (which == 0) r.load(0x1000, {0x11, 0x8D, 0x00});              // DIVD #0
        else r.load(0x1000, {0x11, 0x8E, 0x00, 0x00});                   // DIVQ #0
        r.set(HD6309::R_S, 0x8000);
        r.set(HD6309::R_D, 0x1234);
        r.set(HD6309::R_CC, 0);
        r.run(1);
        EXPECT_EQ(r.cpu.pc(), 0x2800u);
        EXPECT_TRUE(r.cpu.md() & HD6309::MD_DZ);
        EXPECT_EQ(r.cpu.sp(), 0x8000u - 12);
        EXPECT_EQ(r.mem.peek8(0x8000 - 12) & 0x84, M6809::CC_E | M6809::CC_Z);   // stacked CC: E, Z
        EXPECT_EQ(((r.mem.peek8(0x8000 - 2) << 8) | r.mem.peek8(0x8000 - 1)), which == 0 ? 0x1003 : 0x1004);
        EXPECT_EQ(r.cpu.d(), 0x1234);
        r.run(1);
        EXPECT_TRUE(!(r.cpu.cc() & M6809::CC_Z));        // the bit was set
        EXPECT_EQ(r.cpu.md() & HD6309::MD_DZ, 0);
        r.run(1);
        EXPECT_TRUE(r.cpu.cc() & M6809::CC_Z);           // and is now clear
    }
}

// Undefined opcodes trap through $FFF0 with MD.IL set (on every page)
TEST_CASE(hd6309_illegal_trap) {
    const std::vector<std::vector<uint8_t>> bad = {{0x15}, {0x87, 0x00}, {0x3E}, {0x41}, {0x10, 0x50},
                                                   {0x10, 0x20}, {0x10, 0xCC}, {0x11, 0x40}, {0x11, 0xC3},
                                                   {0x11, 0x38, 0x56}};   // the last: TFM PC,W (bad registers)
    for (const auto& code : bad) {
        Rig6309 r;
        r.poke16(0xFFF0, 0x2800);
        r.load(0x1000, code);
        r.set(HD6309::R_S, 0x8000);
        r.set(HD6309::R_MD, HD6309::MD_NM);
        r.run(1);
        EXPECT_EQ(r.cpu.pc(), 0x2800u);
        EXPECT_TRUE(r.cpu.md() & HD6309::MD_IL);
        EXPECT_EQ(r.cpu.sp(), 0x8000u - 14);             // native mode: E and F stacked too
        EXPECT_TRUE((r.cpu.cc() & (M6809::CC_I | M6809::CC_F)) == (M6809::CC_I | M6809::CC_F));
    }
    // BITMD tests only /0 and IL; NM and FM always read as 0
    Rig6309 r;
    r.load(0x1000, {0x11, 0x3D, 0x03, 0x11, 0x3C, 0x03});
    r.run(2);
    EXPECT_TRUE(r.cpu.nativeMode());
    EXPECT_TRUE(r.cpu.md() & HD6309::MD_FM);
    EXPECT_TRUE(r.cpu.cc() & M6809::CC_Z);
}

// The undefined indexed postbytes: exactly seven on the 6309 ([,-R] and
// [n16] with R bits other than 00), each taking the illegal-instruction trap,
// 21 cycles (23 native) for a page-0 opcode, the index register untouched
// (hoglet67's 6809Decoder). All 256 are tried with LDA.
TEST_CASE(hd6309_illegal_postbytes) {
    const uint8_t bad[] = {0x92, 0xB2, 0xD2, 0xF2, 0xBF, 0xDF, 0xFF};
    for (int pb = 0; pb < 256; ++pb) {
        const bool illegal = std::find(std::begin(bad), std::end(bad), (uint8_t)pb) != std::end(bad);
        for (int native = 0; native < 2; ++native) {
            Rig6309 r;
            r.poke16(0xFFF0, 0x2800);
            r.load(0x1000, {0xA6, (uint8_t)pb, 0x00, 0x00});
            r.set(HD6309::R_X, 0x3000); r.set(HD6309::R_Y, 0x3100); r.set(HD6309::R_U, 0x3200);
            r.set(HD6309::R_S, 0x8000); r.set(HD6309::R_W, 0x3300);
            r.set(HD6309::R_MD, native ? HD6309::MD_NM : 0);
            const int cyc = r.cpu.step();
            const bool trapped = r.cpu.pc() == 0x2800 && (r.cpu.md() & HD6309::MD_IL);
            if (trapped != illegal) std::fprintf(stderr, "postbyte $%02X: trapped %d\n", pb, trapped);
            EXPECT_EQ(trapped, illegal);
            if (!illegal) continue;
            EXPECT_EQ(cyc, native ? 23 : 21);
            EXPECT_EQ(r.cpu.sp(), 0x8000u - (native ? 14 : 12));
            EXPECT_EQ(r.mem.peek8(0x8000 - 2) << 8 | r.mem.peek8(0x8000 - 1), 0x1002);   // stacked PC
            EXPECT_EQ(r.cpu.x() + r.cpu.y() + r.cpu.u(), 0x3000 + 0x3100 + 0x3200);
            EXPECT_EQ(r.dis(0x1000), std::string("???"));
        }
    }
    // Through LEA, a page-2 opcode (one more cycle for the prefix) and AIM
    struct Case { std::vector<uint8_t> code; int emu; };
    const Case cases[] = {{{0x30, 0x92}, 21}, {{0x10, 0xAE, 0xF2}, 22}, {{0x11, 0xA6, 0xBF}, 22}, {{0x62, 0x01, 0xDF}, 22}};
    for (const auto& c : cases) {
        Rig6309 r;
        r.poke16(0xFFF0, 0x2800);
        r.load(0x1000, c.code);
        r.set(HD6309::R_S, 0x8000);
        EXPECT_EQ(r.cpu.step(), c.emu);
        EXPECT_EQ(r.cpu.pc(), 0x2800u);
        EXPECT_TRUE(r.cpu.md() & HD6309::MD_IL);
    }
    // The 6309 disassembles the legal neighbours as before; the illegal ones
    // take no displacement bytes
    Rig6309 r;
    r.load(0x1000, {0xA6, 0xBF, 0x12, 0x34});
    char b[64];
    EXPECT_EQ(r.cpu.disassembleOne(&r.mem, 0x1000, b, sizeof b), 2);
    r.load(0x1000, {0xA6, 0x9F, 0x12, 0x34});
    EXPECT_EQ(r.dis(0x1000), std::string("LDA [$1234]"));
    r.load(0x1000, {0xA6, 0x93});
    EXPECT_EQ(r.dis(0x1000), std::string("LDA [,--X]"));
}

// A page prefix after a page prefix is an undefined opcode on the 6309
// (hoglet67), not a chain as on the 6809: 21 cycles, 23 native
TEST_CASE(hd6309_prefix_pairs_trap) {
    for (const uint8_t a : {0x10, 0x11}) {
        for (const uint8_t b2 : {0x10, 0x11}) {
            for (int native = 0; native < 2; ++native) {
                Rig6309 r;
                r.poke16(0xFFF0, 0x2800);
                r.load(0x1000, {a, b2, 0x86, 0x55});      // [prefix] [prefix] LDA #$55
                r.set(HD6309::R_S, 0x8000);
                r.set(HD6309::R_MD, native ? HD6309::MD_NM : 0);
                EXPECT_EQ(r.cpu.step(), native ? 23 : 21);
                EXPECT_EQ(r.cpu.pc(), 0x2800u);
                EXPECT_TRUE(r.cpu.md() & HD6309::MD_IL);
                EXPECT_EQ(r.mem.peek8(0x8000 - 2) << 8 | r.mem.peek8(0x8000 - 1), 0x1002);
                EXPECT_EQ(r.dis(0x1000), std::string("???"));
            }
        }
    }
}

// DAA: the 6809's corrections, except with C set and A in $80-$99, where the
// 6309 adds its own (hoglet67's exhaustive test on a real part). V is bit 7
// of the result XOR C on both.
TEST_CASE(hd6309_daa) {
    struct Case { uint8_t a; bool h; uint8_t want; };
    const Case quirks[] = {{0x80, false, 0x00}, {0x8F, false, 0x15}, {0x8A, false, 0x10}, {0x90, false, 0x00},
                           {0x98, false, 0x00}, {0x99, false, 0x01}, {0x80, true, 0x06},  {0x8F, true, 0x15},  {0x90, true, 0x06},
                           {0x97, true, 0x0D},  {0x98, true, 0x00},  {0x99, true, 0x00}};
    for (const auto& q : quirks) {
        Rig6309 r;
        r.load(0x1000, {0x19});
        r.set(HD6309::R_A, q.a);
        r.set(HD6309::R_CC, M6809::CC_C | (q.h ? M6809::CC_H : 0));
        r.run(1);
        if (r.cpu.a() != q.want) std::fprintf(stderr, "DAA C=1 H=%d A=$%02X: $%02X\n", q.h, q.a, r.cpu.a());
        EXPECT_EQ(r.cpu.a(), q.want);
        EXPECT_TRUE(r.cpu.cc() & M6809::CC_C);
    }
    // Everywhere else the two CPUs agree, flags included
    for (int in = 0; in < 0x400; ++in) {
        const uint8_t a = (uint8_t)in, cc = (uint8_t)(((in >> 8) & 1 ? M6809::CC_C : 0) | ((in >> 9) & 1 ? M6809::CC_H : 0));
        Rig6309 r;
        r.load(0x1000, {0x19});
        r.set(HD6309::R_A, a); r.set(HD6309::R_CC, cc);
        r.run(1);
        FlatMemoryBus m{"m", 16};
        m.write8(0x1000, 0x19);
        M6809 c;
        c.setDataBus(&m);
        c.regWrite(M6809::R_A, a); c.regWrite(M6809::R_CC, cc); c.setPc(0x1000);
        c.step();
        if ((cc & M6809::CC_C) && a >= 0x80 && a <= 0x99) {
            EXPECT_TRUE(r.cpu.a() != c.a());
            continue;
        }
        EXPECT_EQ(r.cpu.a(), c.a());
        EXPECT_EQ(r.cpu.cc(), c.cc());
    }
    // V = bit 7 of the result XOR C: $7A + $06 = $80 sets N and V
    Rig6309 r;
    r.load(0x1000, {0x19});
    r.set(HD6309::R_A, 0x7A); r.set(HD6309::R_CC, 0);
    r.run(1);
    EXPECT_EQ(r.cpu.a(), 0x80);
    EXPECT_EQ(r.cpu.cc() & 0x0F, M6809::CC_N | M6809::CC_V);
    EXPECT_EQ(cyclesOf({0x19}, false), 2);
    EXPECT_EQ(cyclesOf({0x19}, true), 1);
}

// PC as an inter-register operand: TFR/EXG see the next instruction's address,
// ADDR ... CMPR one more (pipelining, hoglet67), in both modes. ADDR/ADCR
// leave H alone.
TEST_CASE(hd6309_pc_operand) {
    Rig6309 r;
    r.load(0x1000, {0x1F, 0x51,             // TFR PC,X     X = $1002
                    0x10, 0x30, 0x52,       // ADDR PC,Y    Y += $1006
                    0x10, 0x30, 0x58,       // ADDR PC,A    A += $09 (low byte of $1009)
                    0x10, 0x37, 0x51});     // CMPR PC,X    X - $100C
    r.set(HD6309::R_Y, 0x0100); r.set(HD6309::R_A, 0x01);
    r.run(1);
    EXPECT_EQ(r.cpu.x(), 0x1002);
    r.run(1);
    EXPECT_EQ(r.cpu.y(), 0x1106);
    r.run(1);
    EXPECT_EQ(r.cpu.a(), 0x0A);
    r.set(HD6309::R_X, 0x100C);
    r.run(1);
    EXPECT_TRUE(r.cpu.cc() & M6809::CC_Z);
    // ADDR X,PC: PC = (the address after the instruction + 1) + X
    r.load(0x1000, {0x10, 0x30, 0x15});
    r.set(HD6309::R_X, 0x0100);
    r.run(1);
    EXPECT_EQ(r.cpu.pc(), 0x1104u);
    // EXG X,PC: a jump, X gets the return address
    r.load(0x1000, {0x1E, 0x15});
    r.set(HD6309::R_X, 0x2000);
    r.run(1);
    EXPECT_EQ(r.cpu.pc(), 0x2000u);
    EXPECT_EQ(r.cpu.x(), 0x1002);
    // H: kept set without a half carry, kept clear with one
    r.load(0x1000, {0x10, 0x30, 0x89, 0x10, 0x31, 0x89});   // ADDR A,B; ADCR A,B
    r.set(HD6309::R_A, 0x01); r.set(HD6309::R_B, 0x01); r.set(HD6309::R_CC, M6809::CC_H);
    r.run(1);
    EXPECT_TRUE(r.cpu.cc() & M6809::CC_H);
    r.set(HD6309::R_A, 0x0F); r.set(HD6309::R_B, 0x01); r.set(HD6309::R_CC, 0);
    r.run(1);
    EXPECT_EQ(r.cpu.b(), 0x10);
    EXPECT_TRUE(!(r.cpu.cc() & M6809::CC_H));
}

// TFM with a register other than D X Y U S: 23 cycles (25 native), Z from W
TEST_CASE(hd6309_tfm_bad_register) {
    for (int native = 0; native < 2; ++native) {
        for (const uint16_t w : {0, 5}) {
            Rig6309 r;
            r.poke16(0xFFF0, 0x2800);
            r.load(0x1000, {0x11, 0x38, 0x56});           // TFM PC+,W+
            r.set(HD6309::R_S, 0x8000); r.set(HD6309::R_W, w);
            r.set(HD6309::R_MD, native ? HD6309::MD_NM : 0);
            EXPECT_EQ(r.cpu.step(), native ? 25 : 23);
            EXPECT_EQ(r.cpu.pc(), 0x2800u);
            const uint8_t stackedCc = r.mem.peek8(r.cpu.sp());
            EXPECT_EQ((bool)(stackedCc & M6809::CC_Z), w == 0);
        }
    }
}

// TFM in its four forms; 6 + 3n cycles; Z set at the end
TEST_CASE(hd6309_tfm) {
    struct T { uint8_t op; uint16_t x0, y0, x1, y1; };
    const T forms[] = {{0x38, 0x3000, 0x4000, 0x3008, 0x4008},
                       {0x39, 0x3007, 0x4007, 0x2FFF, 0x3FFF},
                       {0x3A, 0x3000, 0x4000, 0x3008, 0x4000},
                       {0x3B, 0x3000, 0x4000, 0x3000, 0x4008}};
    for (const T& t : forms) {
        Rig6309 r;
        for (int i = 0; i < 8; ++i) r.mem.write8(0x3000 + i, (uint8_t)(i + 1));
        r.load(0x1000, {0x11, t.op, 0x12, 0x12});    // TFM X?,Y? ; NOP
        r.set(HD6309::R_X, t.x0); r.set(HD6309::R_Y, t.y0); r.set(HD6309::R_W, 8);
        int cyc = r.cpu.step();
        EXPECT_TRUE(r.cpu.tfmActive());
        EXPECT_EQ(r.cpu.pc(), 0x1000u);
        while (r.cpu.tfmActive()) cyc += r.cpu.step();
        EXPECT_EQ(cyc, 6 + 3 * 8);
        EXPECT_EQ(r.cpu.pc(), 0x1003u);
        EXPECT_EQ(r.cpu.x(), t.x1); EXPECT_EQ(r.cpu.y(), t.y1);
        EXPECT_EQ(r.cpu.w(), 0);
        EXPECT_TRUE(r.cpu.cc() & M6809::CC_Z);
        switch (t.op) {
            case 0x38: for (int i = 0; i < 8; ++i) EXPECT_EQ(r.mem.peek8(0x4000 + i), i + 1); break;
            case 0x39: for (int i = 0; i < 8; ++i) EXPECT_EQ(r.mem.peek8(0x4000 + i), i + 1); break;
            case 0x3A: EXPECT_EQ(r.mem.peek8(0x4000), 8); break;     // a port: the last byte
            case 0x3B: for (int i = 0; i < 8; ++i) EXPECT_EQ(r.mem.peek8(0x4000 + i), 1); break;   // fill
        }
    }
    // W = 0: nothing moves, 6 cycles
    Rig6309 z;
    z.load(0x1000, {0x11, 0x38, 0x12, 0x12});
    z.set(HD6309::R_W, 0);
    EXPECT_EQ(z.cpu.step(), 6);
    EXPECT_TRUE(!z.cpu.tfmActive());
    EXPECT_EQ(z.cpu.pc(), 0x1003u);
}

// An interrupt between a TFM's read and write: the PC stacked is the TFM's,
// the pointers and W are not advanced, and after RTI the TFM resumes
TEST_CASE(hd6309_tfm_interrupted) {
    Rig6309 r;
    r.poke16(0xFFF8, 0x2000);
    r.mem.write8(0x2000, 0x3B);                       // RTI
    for (int i = 0; i < 4; ++i) r.mem.write8(0x3000 + i, (uint8_t)(0xA0 + i));
    r.load(0x1000, {0x1C, 0xEF,                       // ANDCC #$EF
                    0x11, 0x38, 0x12,                 // TFM X+,Y+
                    0x12});
    r.set(HD6309::R_S, 0x8000);
    r.set(HD6309::R_X, 0x3000); r.set(HD6309::R_Y, 0x4000); r.set(HD6309::R_W, 4);
    r.run(2);                                         // ANDCC, TFM setup
    r.run(1);                                         // first byte
    EXPECT_EQ(r.cpu.x(), 0x3001);
    r.cpu.setIrqLine(true);
    EXPECT_EQ(r.cpu.step(), 1 + 19);                  // the read, then the IRQ
    EXPECT_EQ(r.cpu.pc(), 0x2000u);
    EXPECT_EQ(r.cpu.x(), 0x3001); EXPECT_EQ(r.cpu.w(), 3);
    EXPECT_EQ(((r.mem.peek8(0x7FFE) << 8) | r.mem.peek8(0x7FFF)), 0x1002);   // stacked PC: the TFM
    EXPECT_TRUE(!(r.mem.peek8(0x8000 - 12) & M6809::CC_Z));
    r.cpu.setIrqLine(false);
    r.run(1);                                         // RTI
    EXPECT_EQ(r.cpu.pc(), 0x1002u);
    for (int i = 0; i < 10 && r.cpu.pc() != 0x1005u; ++i) r.cpu.step();
    EXPECT_EQ(r.cpu.pc(), 0x1005u);
    EXPECT_EQ(r.cpu.w(), 0);
    for (int i = 0; i < 4; ++i) EXPECT_EQ(r.mem.peek8(0x4000 + i), 0xA0 + i);
}

// Native mode stacks E and F (14 bytes, 21 cycles; RTI 17 pulls them); FM
// makes FIRQ stack everything with E set
TEST_CASE(hd6309_native_interrupts) {
    Rig6309 r;
    r.poke16(0xFFF8, 0x2000); r.poke16(0xFFF6, 0x2100);
    r.mem.write8(0x2000, 0x3B); r.mem.write8(0x2100, 0x3B);
    r.load(0x1000, {0x10, 0xCE, 0x80, 0x00,           // LDS #$8000
                    0x11, 0x3D, 0x01,                 // LDMD #1 (native)
                    0x1C, 0xAF,                       // ANDCC #$AF
                    0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12});
    r.run(3);
    r.set(HD6309::R_E, 0x12); r.set(HD6309::R_F, 0x34);
    r.set(HD6309::R_A, 0x56); r.set(HD6309::R_B, 0x78); r.set(HD6309::R_DP, 0x9A);
    r.cpu.setIrqLine(true);
    EXPECT_EQ(r.cpu.step(), 21);
    EXPECT_EQ(r.cpu.pc(), 0x2000u);
    EXPECT_EQ(r.cpu.sp(), 0x8000u - 14);
    const uint16_t s = 0x8000 - 14;
    EXPECT_EQ(r.mem.peek8(s + 1), 0x56); EXPECT_EQ(r.mem.peek8(s + 2), 0x78);
    EXPECT_EQ(r.mem.peek8(s + 3), 0x12); EXPECT_EQ(r.mem.peek8(s + 4), 0x34);
    EXPECT_EQ(r.mem.peek8(s + 5), 0x9A);
    r.cpu.setIrqLine(false);
    r.set(HD6309::R_W, 0);
    EXPECT_EQ(r.cpu.step(), 17);                      // RTI
    EXPECT_EQ(r.cpu.w(), 0x1234);
    EXPECT_EQ(r.cpu.sp(), 0x8000u);
    EXPECT_EQ(r.cpu.pc(), 0x1009u);
    // FIRQ without FM: PC and CC only, in either mode
    r.cpu.setFirqLine(true);
    EXPECT_EQ(r.cpu.step(), 10);
    EXPECT_EQ(r.cpu.sp(), 0x8000u - 3);
    r.cpu.setFirqLine(false);
    EXPECT_EQ(r.cpu.step(), 6);
    // FM + native: FIRQ stacks the entire state, E set
    r.set(HD6309::R_MD, HD6309::MD_NM | HD6309::MD_FM);
    r.cpu.setFirqLine(true);
    EXPECT_EQ(r.cpu.step(), 21);
    EXPECT_EQ(r.cpu.sp(), 0x8000u - 14);
    EXPECT_TRUE(r.mem.peek8(0x8000 - 14) & M6809::CC_E);
    r.cpu.setFirqLine(false);
    EXPECT_EQ(r.cpu.step(), 17);
    // FM in emulation mode: 12 bytes, 19 cycles
    r.set(HD6309::R_MD, HD6309::MD_FM);
    r.cpu.setFirqLine(true);
    EXPECT_EQ(r.cpu.step(), 19);
    EXPECT_EQ(r.cpu.sp(), 0x8000u - 12);
    r.cpu.setFirqLine(false);
    EXPECT_EQ(r.cpu.step(), 15);
    EXPECT_EQ(r.cpu.sp(), 0x8000u);
}

// CWAI in native mode stacks 14 bytes; SWI in native mode too
TEST_CASE(hd6309_native_cwai_swi) {
    Rig6309 r;
    r.poke16(0xFFF8, 0x2000); r.poke16(0xFFFA, 0x2100);
    r.mem.write8(0x2000, 0x3B); r.mem.write8(0x2100, 0x3B);
    r.load(0x1000, {0x10, 0xCE, 0x80, 0x00, 0x11, 0x3D, 0x01,
                    0x3C, 0xEF,                       // CWAI #$EF
                    0x3F,                             // SWI
                    0x12});
    r.run(2);
    EXPECT_EQ(r.cpu.step(), 20);
    EXPECT_TRUE(r.cpu.isHalted());
    EXPECT_EQ(r.cpu.sp(), 0x8000u - 14);
    r.cpu.setIrqLine(true);
    EXPECT_EQ(r.cpu.step(), 4);                       // only the vector fetch is left
    EXPECT_EQ(r.cpu.pc(), 0x2000u);
    r.cpu.setIrqLine(false);
    r.run(1);
    EXPECT_EQ(r.cpu.pc(), 0x1009u);
    EXPECT_EQ(r.cpu.step(), 21);                      // SWI
    EXPECT_EQ(r.cpu.sp(), 0x8000u - 14);
    EXPECT_EQ(r.cpu.step(), 17);                      // RTI
    EXPECT_EQ(r.cpu.pc(), 0x100Au);
}

// BAND BIAND BOR BIOR BEOR BIEOR LDBT STBT
TEST_CASE(hd6309_bit_ops) {
    Rig6309 r;
    r.mem.write8(0x2010, 0xA5);                       // 1010 0101
    r.load(0x1000, {0x86, 0x20, 0x1F, 0x8B,           // DP = $20
                    0x4F,                             // CLRA
                    0x11, 0x36, 0x57, 0x10,           // LDBT A,2,7,<$10  A.7 = M.2 (1)
                    0x11, 0x30, 0x4F, 0x10,           // BAND A,1,7       A.7 = M.1 (0) & A.7
                    0x11, 0x31, 0x48, 0x10,           // BIAND A,1,0      A.0 = !M.1 (1) & A.0 (0)
                    0x11, 0x32, 0x40, 0x10,           // BOR A,0,0        A.0 = M.0 (1) | A.0
                    0x11, 0x33, 0x81, 0x10,           // BIOR B,0,1       B.1 = !M.0 (0) | B.1 (0)
                    0x11, 0x34, 0x40, 0x10,           // BEOR A,0,0       A.0 = 1 ^ 1 = 0
                    0x11, 0x35, 0x4A, 0x10,           // BIEOR A,1,2      A.2 = !M.1 (1) ^ A.2 (0)
                    0x11, 0x37, 0x83, 0x10,           // STBT B,0,3       M.3 = B.0
                    0x11, 0x32, 0x00, 0x10});         // BOR CC,0,0       C = M.0 | C
    r.run(3);
    r.run(1); EXPECT_EQ(r.cpu.a(), 0x80);
    r.run(1); EXPECT_EQ(r.cpu.a(), 0x00);
    r.run(1); EXPECT_EQ(r.cpu.a(), 0x00);
    r.run(1); EXPECT_EQ(r.cpu.a(), 0x01);
    r.set(HD6309::R_B, 0x01);
    r.run(1); EXPECT_EQ(r.cpu.b(), 0x01);
    r.run(1); EXPECT_EQ(r.cpu.a(), 0x00);
    r.run(1); EXPECT_EQ(r.cpu.a(), 0x04);
    r.run(1); EXPECT_EQ(r.mem.peek8(0x2010), 0xAD);
    r.set(HD6309::R_CC, 0);
    r.run(1); EXPECT_EQ(r.cpu.cc(), M6809::CC_C);
}

// AIM OIM EIM TIM
TEST_CASE(hd6309_immediate_memory_ops) {
    Rig6309 r;
    r.mem.write8(0x2010, 0xF0); r.mem.write8(0x3005, 0x0F);
    r.load(0x1000, {0x86, 0x20, 0x1F, 0x8B,           // DP = $20
                    0x02, 0x3C, 0x10,                 // AIM #$3C;<$10 -> $30
                    0x01, 0x8F, 0x10,                 // OIM #$8F;<$10 -> $BF N
                    0x05, 0xFF, 0x10,                 // EIM #$FF;<$10 -> $40
                    0x0B, 0x80, 0x10,                 // TIM #$80;<$10 -> Z, unchanged
                    0x65, 0x0F, 0x05,                 // EIM #$0F;5,X  -> 0 Z
                    0x7B, 0x40, 0x20, 0x10});         // TIM #$40;$2010 -> not Z
    r.set(HD6309::R_X, 0x3000);
    r.set(HD6309::R_CC, M6809::CC_V);
    r.run(3);
    EXPECT_EQ(r.mem.peek8(0x2010), 0x30); EXPECT_EQ(r.cpu.cc() & 0x0E, 0);
    r.run(1);
    EXPECT_EQ(r.mem.peek8(0x2010), 0xBF); EXPECT_EQ(r.cpu.cc() & 0x0E, M6809::CC_N);
    r.run(1);
    EXPECT_EQ(r.mem.peek8(0x2010), 0x40);
    r.run(1);
    EXPECT_EQ(r.mem.peek8(0x2010), 0x40); EXPECT_EQ(r.cpu.cc() & 0x0E, M6809::CC_Z);
    r.run(1);
    EXPECT_EQ(r.mem.peek8(0x3005), 0x00); EXPECT_EQ(r.cpu.cc() & 0x0E, M6809::CC_Z);
    r.run(1);
    EXPECT_EQ(r.cpu.cc() & 0x0E, 0);
}

// Native mode runs 6809 code at the reduced counts with the same results
TEST_CASE(hd6309_native_program) {
    Rig6309 r;
    // Sum 1..10 into B, the loop counted in E
    r.load(0x1000, {0x11, 0x3D, 0x01,                 // LDMD #1
                    0x5F,                             // CLRB
                    0x11, 0x86, 0x0A,                 // LDE #10
                    0x1F, 0xE8,                       // L: TFR E,A
                    0x10, 0x30, 0x89,                 // ADDR A,B
                    0x11, 0x4A,                       // DECE
                    0x26, 0xF7,                       // BNE L
                    0x12});
    for (int i = 0; i < 200 && r.cpu.pc() != 0x1010u; ++i) r.cpu.step();
    EXPECT_EQ(r.cpu.b(), 55);
    EXPECT_EQ(r.cpu.e(), 0);
}

// The disassembler: the new opcodes, postbytes and indexed modes
TEST_CASE(hd6309_disassembler) {
    Rig6309 r;
    struct D { std::vector<uint8_t> code; const char* text; int len; };
    const std::vector<D> cases = {
        {{0x10, 0x30, 0x12}, "ADDR X,Y", 3},
        {{0x10, 0x37, 0xE9}, "CMPR E,B", 3},
        {{0x11, 0x38, 0x12}, "TFM X+,Y+", 3},
        {{0x11, 0x39, 0x34}, "TFM U-,S-", 3},
        {{0x11, 0x3A, 0x01}, "TFM D+,X", 3},
        {{0x11, 0x3B, 0x12}, "TFM X,Y+", 3},
        {{0x11, 0x30, 0x57, 0x10}, "BAND A,2,7,<$10", 4},
        {{0x11, 0x37, 0x83, 0x40}, "STBT B,0,3,<$40", 4},
        {{0x02, 0x3C, 0x10}, "AIM #$3C;<$10", 3},
        {{0x62, 0x3C, 0x84}, "AIM #$3C;,X", 3},
        {{0x7B, 0x01, 0x12, 0x34}, "TIM #$01;$1234", 4},
        {{0xA6, 0x8F}, "LDA ,W", 2},
        {{0xA6, 0xAF, 0x12, 0x34}, "LDA $1234,W", 4},
        {{0xA6, 0xD0}, "LDA [,W++]", 2},
        {{0xA6, 0xEF}, "LDA ,--W", 2},
        {{0xA6, 0x87}, "LDA E,X", 2},
        {{0xE6, 0xAA}, "LDB F,Y", 2},
        {{0xA6, 0x8E}, "LDA W,X", 2},
        {{0xA6, 0x97}, "LDA [E,X]", 2},
        {{0x11, 0x8D, 0x05}, "DIVD #$05", 3},
        {{0x11, 0x8E, 0x00, 0x05}, "DIVQ #$0005", 4},
        {{0x11, 0x8F, 0x12, 0x34}, "MULD #$1234", 4},
        {{0x10, 0x86, 0x12, 0x34}, "LDW #$1234", 4},
        {{0x10, 0xDC, 0x10}, "LDQ <$10", 3},
        {{0x10, 0xED, 0x84}, "STQ ,X", 3},
        {{0xCD, 0x12, 0x34, 0x56, 0x78}, "LDQ #$12345678", 5},
        {{0x14}, "SEXW", 1},
        {{0x11, 0x3D, 0x01}, "LDMD #$01", 3},
        {{0x11, 0x3C, 0x80}, "BITMD #$80", 3},
        {{0x10, 0x38}, "PSHSW", 2},
        {{0x10, 0x3B}, "PULUW", 2},
        {{0x10, 0x4F}, "CLRD", 2},
        {{0x10, 0x59}, "ROLW", 2},
        {{0x11, 0x4C}, "INCE", 2},
        {{0x11, 0x5A}, "DECF", 2},
        {{0x1F, 0x6E}, "TFR W,E", 2},
        {{0x1E, 0xC7}, "EXG 0,V", 2},
        {{0x11, 0xC6, 0x05}, "LDF #$05", 3},
        {{0x11, 0x97, 0x10}, "STE <$10", 3},
        {{0x11, 0xBB, 0x12, 0x34}, "ADDE $1234", 4},
        {{0x10, 0x89, 0x00, 0x01}, "ADCD #$0001", 4},
        {{0x10, 0xB7, 0x12, 0x34}, "STW $1234", 4},
        {{0x15}, "???", 1},
        {{0x10, 0x50}, "???", 2},
        {{0x87}, "???", 1},
    };
    for (const auto& c : cases) {
        r.load(0x1000, c.code);
        char b[64];
        const int n = r.cpu.disassembleOne(&r.mem, 0x1000, b, sizeof b);
        if (std::string(b) != c.text) std::fprintf(stderr, "disassembly: '%s', expected '%s'\n", b, c.text);
        EXPECT_EQ(std::string(b), std::string(c.text));
        EXPECT_EQ(n, c.len);
    }
    // Branch classification: BAND and BITD are not branches, LBEQ is
    r.load(0x1000, {0x11, 0x30, 0x57, 0x10, 0x10, 0x85, 0x00, 0x01, 0x10, 0x27, 0x00, 0x10});
    DisasmEntry e;
    r.cpu.disassembleEntry(&r.mem, 0x1000, &e);
    EXPECT_TRUE(!e.isBranch);
    r.cpu.disassembleEntry(&r.mem, 0x1004, &e);
    EXPECT_TRUE(!e.isBranch);
    EXPECT_EQ(e.mnemonic, std::string("BITD"));
    r.cpu.disassembleEntry(&r.mem, 0x1008, &e);
    EXPECT_TRUE(e.isBranch);
}

// Save / load state carries the 6309 registers and a TFM in progress
TEST_CASE(hd6309_state) {
    Rig6309 r;
    r.load(0x1000, {0x11, 0x38, 0x12});
    r.set(HD6309::R_X, 0x3000); r.set(HD6309::R_Y, 0x4000); r.set(HD6309::R_W, 5);
    r.set(HD6309::R_V, 0xCAFE); r.set(HD6309::R_MD, HD6309::MD_NM);
    r.run(2);
    std::vector<uint8_t> st(r.cpu.stateSize());
    r.cpu.saveState(st.data());
    HD6309 c;
    c.setDataBus(&r.mem);
    c.loadState(st.data());
    EXPECT_EQ(c.w(), 4); EXPECT_EQ(c.v(), 0xCAFE);
    EXPECT_TRUE(c.nativeMode());
    EXPECT_TRUE(c.tfmActive());
    EXPECT_EQ(c.x(), 0x3001);
    while (c.tfmActive()) c.step();
    EXPECT_EQ(c.x(), 0x3005);
    EXPECT_EQ(c.pc(), 0x1003u);
}
