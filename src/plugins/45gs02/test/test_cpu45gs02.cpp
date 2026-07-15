#include "test_harness.h"
#include "plugins/45gs02/main/cpu45gs02.h"
#include "libmem/main/memory_bus.h"
#include "libtoolchain/main/idisasm.h"

struct TestFixture {
    FlatMemoryBus bus{"test", 16};
    MOS45GS02 cpu;

    TestFixture() {
        cpu.setDataBus(&bus);
        cpu.reset();
        // Set PC directly — no reset vector needed
        cpu.regWrite(6, 0x0200); // PC = $0200
    }

    void poke(uint16_t addr, uint8_t val) { bus.write8(addr, val); }

    void pokeProgram(uint16_t addr, std::initializer_list<uint8_t> bytes) {
        for (auto b : bytes) bus.write8(addr++, b);
    }

    void step(int n = 1) { for (int i = 0; i < n; i++) cpu.step(); }
};

TEST_CASE(gs02_register_access) {
    TestFixture t;

    // Write and read all registers
    t.cpu.regWrite(0, 0x11); // A
    t.cpu.regWrite(1, 0x22); // X
    t.cpu.regWrite(2, 0x33); // Y
    t.cpu.regWrite(3, 0x44); // Z
    t.cpu.regWrite(4, 0x55); // B
    t.cpu.regWrite(5, 0x01FE); // SP
    t.cpu.regWrite(6, 0x1234); // PC
    t.cpu.regWrite(7, 0x30);   // P

    ASSERT_EQ(t.cpu.regRead(0), (uint32_t)0x11);
    ASSERT_EQ(t.cpu.regRead(1), (uint32_t)0x22);
    ASSERT_EQ(t.cpu.regRead(2), (uint32_t)0x33);
    ASSERT_EQ(t.cpu.regRead(3), (uint32_t)0x44);
    ASSERT_EQ(t.cpu.regRead(4), (uint32_t)0x55);
    ASSERT_EQ(t.cpu.regRead(5), (uint32_t)0x01FE);
    ASSERT_EQ(t.cpu.regRead(6), (uint32_t)0x1234);
    ASSERT_EQ(t.cpu.regRead(7), (uint32_t)0x30);

    // Q register (idx 8) = A | (X<<8) | (Y<<16) | (Z<<24)
    uint32_t q = t.cpu.regRead(8);
    ASSERT_EQ(q, (uint32_t)(0x11 | (0x22 << 8) | (0x33 << 16) | (0x44 << 24)));

    // Write Q
    t.cpu.regWrite(8, 0xAABBCCDD);
    ASSERT_EQ(t.cpu.regRead(0), (uint32_t)0xDD); // A = low byte
    ASSERT_EQ(t.cpu.regRead(1), (uint32_t)0xCC); // X
    ASSERT_EQ(t.cpu.regRead(2), (uint32_t)0xBB); // Y
    ASSERT_EQ(t.cpu.regRead(3), (uint32_t)0xAA); // Z

    // Out-of-range register
    ASSERT_EQ(t.cpu.regRead(99), (uint32_t)0);

    // Descriptor
    ASSERT(t.cpu.regDescriptor(0) != nullptr);
    ASSERT(t.cpu.regDescriptor(99) == nullptr);
}

TEST_CASE(gs02_reset) {
    TestFixture t;

    t.cpu.regWrite(0, 0xFF);
    t.cpu.regWrite(1, 0xFF);
    t.cpu.reset();

    ASSERT_EQ(t.cpu.regRead(0), (uint32_t)0);   // A = 0
    ASSERT_EQ(t.cpu.regRead(1), (uint32_t)0);   // X = 0
    ASSERT_EQ(t.cpu.regRead(2), (uint32_t)0);   // Y = 0
    ASSERT_EQ(t.cpu.regRead(3), (uint32_t)0);   // Z = 0
    ASSERT_EQ(t.cpu.regRead(5), (uint32_t)0x01FF); // SP
    ASSERT(t.cpu.regRead(7) & 0x04);               // I flag set
}

TEST_CASE(gs02_isa_identity) {
    MOS45GS02 cpu;
    ASSERT(std::string(cpu.isaName()) == "45GS02");
    ASSERT(std::string(cpu.variantName()) == "MEGA65 45GS02");
    ASSERT_EQ(cpu.regCount(), 9);
}

TEST_CASE(gs02_nop) {
    TestFixture t;
    t.poke(0x0200, 0xEA); // NOP
    uint16_t pcBefore = t.cpu.pc();
    t.step();
    ASSERT_EQ(t.cpu.pc(), (uint16_t)(pcBefore + 1));
}

TEST_CASE(gs02_lda_imm) {
    TestFixture t;
    t.pokeProgram(0x0200, {0xA9, 0x42}); // LDA #$42
    t.step();
    ASSERT_EQ(t.cpu.regRead(0), (uint32_t)0x42);
    // N=0, Z=0
    ASSERT((t.cpu.regRead(7) & 0x80) == 0);
    ASSERT((t.cpu.regRead(7) & 0x02) == 0);
}

TEST_CASE(gs02_ldx_ldy_ldz_imm) {
    TestFixture t;
    t.pokeProgram(0x0200, {
        0xA2, 0x10, // LDX #$10
        0xA0, 0x20, // LDY #$20
        0xA3, 0x30  // LDZ #$30
    });
    t.step(3);
    ASSERT_EQ(t.cpu.regRead(1), (uint32_t)0x10);
    ASSERT_EQ(t.cpu.regRead(2), (uint32_t)0x20);
    ASSERT_EQ(t.cpu.regRead(3), (uint32_t)0x30);
}

TEST_CASE(gs02_sta_stx_sty_stz) {
    TestFixture t;
    t.pokeProgram(0x0200, {
        0xA9, 0xAA,       // LDA #$AA
        0x85, 0x10,       // STA $10
        0xA2, 0xBB,       // LDX #$BB
        0x86, 0x11,       // STX $11
        0xA0, 0xCC,       // LDY #$CC
        0x84, 0x12,       // STY $12
        0xA3, 0xDD,       // LDZ #$DD
        0x64, 0x13        // STZ $13
    });
    t.step(8);
    ASSERT_EQ(t.bus.read8(0x10), (uint8_t)0xAA);
    ASSERT_EQ(t.bus.read8(0x11), (uint8_t)0xBB);
    ASSERT_EQ(t.bus.read8(0x12), (uint8_t)0xCC);
    ASSERT_EQ(t.bus.read8(0x13), (uint8_t)0xDD);
}

TEST_CASE(gs02_transfers) {
    TestFixture t;
    t.pokeProgram(0x0200, {
        0xA9, 0x42,  // LDA #$42
        0xAA,        // TAX
        0xA8,        // TAY
        0x4B,        // TAZ
        0xA9, 0x00,  // LDA #$00
        0x8A,        // TXA -> A=$42
        0xA9, 0x00,  // LDA #$00
        0x98,        // TYA -> A=$42
        0xA9, 0x00,  // LDA #$00
        0x6B         // TZA -> A=$42
    });
    t.step(4); // LDA, TAX, TAY, TAZ
    ASSERT_EQ(t.cpu.regRead(1), (uint32_t)0x42); // X
    ASSERT_EQ(t.cpu.regRead(2), (uint32_t)0x42); // Y
    ASSERT_EQ(t.cpu.regRead(3), (uint32_t)0x42); // Z

    t.step(2); // LDA #0, TXA
    ASSERT_EQ(t.cpu.regRead(0), (uint32_t)0x42);

    t.step(2); // LDA #0, TYA
    ASSERT_EQ(t.cpu.regRead(0), (uint32_t)0x42);

    t.step(2); // LDA #0, TZA
    ASSERT_EQ(t.cpu.regRead(0), (uint32_t)0x42);
}

TEST_CASE(gs02_inc_dec) {
    TestFixture t;
    t.pokeProgram(0x0200, {
        0xA9, 0x10,  // LDA #$10
        0x1A,        // INC A
        0xA2, 0x05,  // LDX #$05
        0xCA,        // DEX
        0xA0, 0x00,  // LDY #$00
        0xC8,        // INY
        0xA3, 0x03,  // LDZ #$03
        0x3B         // DEZ
    });
    t.step(2); // LDA, INC A
    ASSERT_EQ(t.cpu.regRead(0), (uint32_t)0x11);

    t.step(2); // LDX, DEX
    ASSERT_EQ(t.cpu.regRead(1), (uint32_t)0x04);

    t.step(2); // LDY, INY
    ASSERT_EQ(t.cpu.regRead(2), (uint32_t)0x01);

    t.step(2); // LDZ, DEZ
    ASSERT_EQ(t.cpu.regRead(3), (uint32_t)0x02);
}

TEST_CASE(gs02_jmp_abs) {
    TestFixture t;
    t.pokeProgram(0x0200, {0x4C, 0x00, 0x10}); // JMP $1000
    t.step();
    ASSERT_EQ(t.cpu.pc(), (uint16_t)0x1000);
}

TEST_CASE(gs02_jsr_rts) {
    TestFixture t;
    // JSR $0300 at $0200, subroutine at $0300 does RTS
    t.pokeProgram(0x0200, {0x20, 0x00, 0x03}); // JSR $0300
    t.poke(0x0300, 0x60);                        // RTS
    t.poke(0x0203, 0xEA);                        // NOP (return point)

    t.step(); // JSR
    ASSERT_EQ(t.cpu.pc(), (uint16_t)0x0300);

    t.step(); // RTS
    ASSERT_EQ(t.cpu.pc(), (uint16_t)0x0203);
}

TEST_CASE(gs02_flags_nz) {
    TestFixture t;

    // LDA #$00 -> Z=1, N=0
    t.pokeProgram(0x0200, {0xA9, 0x00, 0xA9, 0x80, 0xA9, 0x01});
    t.step();
    ASSERT(t.cpu.regRead(7) & 0x02);     // Z set
    ASSERT(!(t.cpu.regRead(7) & 0x80));   // N clear

    // LDA #$80 -> Z=0, N=1
    t.step();
    ASSERT(!(t.cpu.regRead(7) & 0x02));   // Z clear
    ASSERT(t.cpu.regRead(7) & 0x80);      // N set

    // LDA #$01 -> Z=0, N=0
    t.step();
    ASSERT(!(t.cpu.regRead(7) & 0x02));
    ASSERT(!(t.cpu.regRead(7) & 0x80));
}

TEST_CASE(gs02_adc_imm) {
    TestFixture t;
    // CLC; LDA #$10; ADC #$20 = $30
    t.pokeProgram(0x0200, {
        0x18,        // CLC
        0xA9, 0x10,  // LDA #$10
        0x69, 0x20   // ADC #$20
    });
    t.step(3);
    ASSERT_EQ(t.cpu.regRead(0), (uint32_t)0x30);
    ASSERT(!(t.cpu.regRead(7) & 0x01)); // C clear (no overflow)
}

TEST_CASE(gs02_adc_carry) {
    TestFixture t;
    // CLC; LDA #$FF; ADC #$01 = $00, C=1
    t.pokeProgram(0x0200, {
        0x18,        // CLC
        0xA9, 0xFF,  // LDA #$FF
        0x69, 0x01   // ADC #$01
    });
    t.step(3);
    ASSERT_EQ(t.cpu.regRead(0), (uint32_t)0x00);
    ASSERT(t.cpu.regRead(7) & 0x01); // C set
    ASSERT(t.cpu.regRead(7) & 0x02); // Z set
}

TEST_CASE(gs02_sbc_imm) {
    TestFixture t;
    // SEC; LDA #$30; SBC #$10 = $20
    t.pokeProgram(0x0200, {
        0x38,        // SEC
        0xA9, 0x30,  // LDA #$30
        0xE9, 0x10   // SBC #$10
    });
    t.step(3);
    ASSERT_EQ(t.cpu.regRead(0), (uint32_t)0x20);
    ASSERT(t.cpu.regRead(7) & 0x01); // C set (no borrow)
}

TEST_CASE(gs02_pha_pla) {
    TestFixture t;
    t.pokeProgram(0x0200, {
        0xA9, 0x42,  // LDA #$42
        0x48,        // PHA
        0xA9, 0x00,  // LDA #$00
        0x68         // PLA
    });
    t.step(4);
    ASSERT_EQ(t.cpu.regRead(0), (uint32_t)0x42);
}

TEST_CASE(gs02_dec_a) {
    TestFixture t;
    t.pokeProgram(0x0200, {
        0xA9, 0x01,  // LDA #$01
        0x3A         // DEC A
    });
    t.step(2);
    ASSERT_EQ(t.cpu.regRead(0), (uint32_t)0x00);
    ASSERT(t.cpu.regRead(7) & 0x02); // Z set
}

TEST_CASE(gs02_lbeq_offset_base) {
    // 16-bit branch offset is relative to PC+1 (not PC+2)
    // Verified against xemu _BRA16 and gs4510.vhdl B16TakeBranch
    TestFixture t;
    // LBEQ at $0202 with offset $0010:
    // After opcode fetch pc=$0203, then pc += 1 + $0010 = $0214
    t.pokeProgram(0x0200, {
        0xA9, 0x00,        // LDA #$00 (set Z flag)
        0xF3, 0x10, 0x00   // LBEQ +$0010
    });
    t.poke(0x0214, 0xA9);  // LDA #$42 at target
    t.poke(0x0215, 0x42);
    t.step(3);  // LDA #$00, LBEQ, LDA #$42
    ASSERT_EQ(t.cpu.regRead(6), (uint32_t)0x0216);  // PC past LDA #$42
    ASSERT_EQ(t.cpu.regRead(0), (uint32_t)0x42);     // A = $42
}

TEST_CASE(gs02_lbeq_not_taken) {
    TestFixture t;
    // LBEQ at $0202: not taken, falls through to $0205
    t.pokeProgram(0x0200, {
        0xA9, 0x01,        // $0200: LDA #$01 (Z clear)
        0xF3, 0x10, 0x00,  // $0202: LBEQ +$0010 (not taken)
        0xA9, 0x99         // $0205: LDA #$99
    });
    t.step(3);
    ASSERT_EQ(t.cpu.regRead(6), (uint32_t)0x0207);  // PC past LDA #$99
    ASSERT_EQ(t.cpu.regRead(0), (uint32_t)0x99);     // A = $99
}

TEST_CASE(gs02_lbne_offset_base) {
    TestFixture t;
    // LBNE at $0202 with offset $0010: target = $0203 + 1 + $0010 = $0214
    t.pokeProgram(0x0200, {
        0xA9, 0x01,        // LDA #$01 (Z clear)
        0xD3, 0x10, 0x00   // LBNE +$0010
    });
    t.poke(0x0214, 0xA9);
    t.poke(0x0215, 0x55);
    t.step(3);
    ASSERT_EQ(t.cpu.regRead(6), (uint32_t)0x0216);
    ASSERT_EQ(t.cpu.regRead(0), (uint32_t)0x55);
}

TEST_CASE(gs02_lbra_offset_base) {
    TestFixture t;
    // LBRA at $0200 with offset $0010: target = $0201 + 1 + $0010 = $0212
    t.pokeProgram(0x0200, {
        0x83, 0x10, 0x00   // LBRA +$0010
    });
    t.poke(0x0212, 0xA9);  // LDA #$77 at target $0212
    t.poke(0x0213, 0x77);
    t.step(2);
    ASSERT_EQ(t.cpu.regRead(6), (uint32_t)0x0214);
    ASSERT_EQ(t.cpu.regRead(0), (uint32_t)0x77);
}

TEST_CASE(gs02_disasm_entry) {
    TestFixture t;

    // Test a relative branch instruction: BNE $0210 at $0200 (offset is 14 bytes)
    t.pokeProgram(0x0200, {0xD0, 0x0E}); // BNE $0210
    DisasmEntry entry;
    int bytes = t.cpu.disassembleEntry(&t.bus, 0x0200, &entry);
    ASSERT_EQ(bytes, 2);
    ASSERT_EQ(entry.addr, (uint32_t)0x0200);
    ASSERT_EQ(entry.mnemonic, "BNE");
    ASSERT_EQ(entry.operands, "$0210");
    ASSERT_EQ(entry.complete, "BNE $0210");
    ASSERT(entry.isBranch);
    ASSERT(!entry.isCall);
    ASSERT(!entry.isReturn);
    ASSERT_EQ(entry.targetAddr, (uint32_t)0x0210);

    // Test JSR $0300 (0x20 0x00 0x03)
    t.pokeProgram(0x0220, {0x20, 0x00, 0x03}); // JSR $0300
    DisasmEntry entry2;
    bytes = t.cpu.disassembleEntry(&t.bus, 0x0220, &entry2);
    ASSERT_EQ(bytes, 3);
    ASSERT_EQ(entry2.mnemonic, "JSR");
    ASSERT_EQ(entry2.operands, "$0300");
    ASSERT(entry2.isCall);
    ASSERT(!entry2.isBranch);
    ASSERT_EQ(entry2.targetAddr, (uint32_t)0x0300);

    // Test BBR0 $02,$0240
    t.pokeProgram(0x0230, {0x0F, 0x02, 0x0D});
    DisasmEntry entry3;
    bytes = t.cpu.disassembleEntry(&t.bus, 0x0230, &entry3);
    ASSERT_EQ(bytes, 3);
    ASSERT_EQ(entry3.mnemonic, "BBR0");
    ASSERT(entry3.isBranch);
    ASSERT_EQ(entry3.targetAddr, (uint32_t)0x0240);
}

