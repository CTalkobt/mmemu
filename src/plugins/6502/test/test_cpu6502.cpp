#include "test_harness.h"
#include "plugins/6502/main/cpu6502.h"
#include "plugins/6502/main/cpu65c02.h"
#include <memory>
#include "libmem/main/memory_bus.h"
#include <cstring>
#include <vector>

TEST_CASE(cpu6502_reset) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    cpu.setDataBus(&bus);

    // Set reset vector
    bus.write8(0xFFFC, 0x00);
    bus.write8(0xFFFD, 0xC0);

    cpu.reset();
    ASSERT(cpu.pc() == 0xC000);
    ASSERT(cpu.regRead(0) == 0); // A
    ASSERT(cpu.regRead(5) == (FLAG_U | FLAG_I)); // P
}

TEST_CASE(cpu6502_basic_ops) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    cpu.setDataBus(&bus);

    // Program: LDA #$42; STA $10; LDX #$01; STX $11; BRK
    uint8_t prog[] = { 0xA9, 0x42, 0x85, 0x10, 0xA2, 0x01, 0x86, 0x11, 0x00 };
    for (size_t i = 0; i < sizeof(prog); ++i) bus.write8(0xC000 + i, prog[i]);

    cpu.setPc(0xC000);
    
    cpu.step(); // LDA #$42
    ASSERT(cpu.regRead(0) == 0x42);
    ASSERT(cpu.pc() == 0xC002);

    cpu.step(); // STA $10
    ASSERT(bus.read8(0x0010) == 0x42);

    cpu.step(); // LDX #$01
    ASSERT(cpu.regRead(1) == 0x01);

    cpu.step(); // STX $11
    ASSERT(bus.read8(0x0011) == 0x01);
}

TEST_CASE(cpu6502_adc_sbc) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    cpu.setDataBus(&bus);

    // ADC #$10
    cpu.regWrite(0, 0x20); // A = $20
    cpu.regWrite(5, FLAG_U); // P = $20 (Clear C)
    bus.write8(0x1000, 0x69); // ADC imm
    bus.write8(0x1001, 0x10);
    cpu.setPc(0x1000);
    cpu.step();
    ASSERT(cpu.regRead(0) == 0x30);
    ASSERT(!(cpu.regRead(5) & FLAG_C));

    // SBC #$05
    bus.write8(0x1002, 0xE9); // SBC imm
    bus.write8(0x1003, 0x05);
    cpu.regWrite(5, cpu.regRead(5) | FLAG_C); // Set C for SBC (no borrow)
    cpu.step();
    ASSERT(cpu.regRead(0) == 0x2B);
}

TEST_CASE(cpu6502_branching) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    cpu.setDataBus(&bus);

    // BNE +$05
    bus.write8(0x1000, 0xD0);
    bus.write8(0x1001, 0x05);
    cpu.setPc(0x1000);
    cpu.regWrite(0, 0x01); // Z = 0
    cpu.regWrite(5, FLAG_U);
    cpu.step();
    ASSERT(cpu.pc() == 0x1007);

    // BNE -$05
    bus.write8(0x1007, 0xD0);
    bus.write8(0x1008, (uint8_t)-5);
    cpu.step();
    ASSERT(cpu.pc() == 0x1004);
}

TEST_CASE(cpu6502_snapshot) {
    MOS6502 cpu;
    cpu.regWrite(0, 0x12);
    cpu.regWrite(1, 0x34);
    cpu.setPc(0xABCD);

    size_t sz = cpu.stateSize();
    std::vector<uint8_t> buf(sz);
    cpu.saveState(buf.data());

    cpu.regWrite(0, 0x00);
    cpu.regWrite(1, 0x00);
    cpu.setPc(0x0000);

    cpu.loadState(buf.data());
    ASSERT(cpu.regRead(0) == 0x12);
    ASSERT(cpu.regRead(1) == 0x34);
    ASSERT(cpu.pc() == 0xABCD);
}

TEST_CASE(cpu6502_illegal_opcodes) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    cpu.setDataBus(&bus);

    // SLO $10: (ASL $10) then (ORA A)
    bus.write8(0x0010, 0x80);
    bus.write8(0x1000, 0x07); // SLO zp
    bus.write8(0x1001, 0x10);
    cpu.regWrite(0, 0x01); // A = 1
    cpu.setPc(0x1000);
    cpu.step();
    // ASL $80 -> $00, Carry = 1
    // A | $00 -> 1
    ASSERT(bus.read8(0x0010) == 0x00);
    ASSERT(cpu.regRead(0) == 0x01);
    ASSERT(cpu.regRead(5) & FLAG_C);

    // LAX $1234: (LDA $1234) then (LDX $1234)
    bus.write8(0x1234, 0x42);
    bus.write8(0x1002, 0xAF); // LAX abs
    bus.write8(0x1003, 0x34);
    bus.write8(0x1004, 0x12);
    cpu.step();
    ASSERT(cpu.regRead(0) == 0x42);
    ASSERT(cpu.regRead(1) == 0x42);
}

TEST_CASE(cpu6502_jsr_rts_stack) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    cpu.setDataBus(&bus);

    // Subroutine at $2000: STA $30; RTS
    bus.write8(0x2000, 0x85); // STA $30
    bus.write8(0x2001, 0x30);
    bus.write8(0x2002, 0x60); // RTS

    // Main at $1000: LDA #$AA; JSR $2000; LDA #$BB; BRK
    bus.write8(0x1000, 0xA9); // LDA #$AA
    bus.write8(0x1001, 0xAA);
    bus.write8(0x1002, 0x20); // JSR $2000
    bus.write8(0x1003, 0x00);
    bus.write8(0x1004, 0x20);
    bus.write8(0x1005, 0xA9); // LDA #$BB
    bus.write8(0x1006, 0xBB);
    bus.write8(0x1007, 0x00); // BRK

    cpu.setPc(0x1000);
    cpu.regWrite(3, 0xFF); // SP = $FF (top of stack page)
    cpu.regWrite(5, FLAG_U);

    cpu.step(); // LDA #$AA
    ASSERT(cpu.regRead(0) == 0xAA);

    cpu.step(); // JSR $2000
    ASSERT(cpu.pc() == 0x2000);

    // JSR pushes return address - 1 ($1004, the last byte of the JSR instruction).
    // Stack should contain: $01FE = $04 (low), $01FF = $10 (high)
    // SP should now be $FD (two bytes pushed)
    ASSERT(cpu.regRead(3) == 0xFD);
    uint8_t pushedHi = bus.read8(0x01FF);
    uint8_t pushedLo = bus.read8(0x01FE);
    uint16_t pushedAddr = (pushedHi << 8) | pushedLo;
    ASSERT(pushedHi == 0x10); // high byte of $1004
    ASSERT(pushedLo == 0x04); // low byte of $1004
    ASSERT(pushedAddr == 0x1004); // return address - 1

    // Verify the pushed address is NOT the instruction after JSR ($1005),
    // but rather JSR_addr + 2 = $1004 (last byte of the 3-byte JSR instruction)
    ASSERT(pushedAddr != 0x1005);

    // Write a sentinel value adjacent to the stack entries to confirm
    // no overwrite occurs during subroutine execution.
    // Place sentinel at $01FD (current top of stack)
    bus.write8(0x01FD, 0x42);

    cpu.step(); // STA $30 (inside subroutine)
    ASSERT(bus.read8(0x0030) == 0xAA);

    // Verify sentinel and return address are still intact
    ASSERT(bus.read8(0x01FD) == 0x42);
    ASSERT(bus.read8(0x01FF) == 0x10);
    ASSERT(bus.read8(0x01FE) == 0x04);

    cpu.step(); // RTS — pops $1004, adds 1 -> PC = $1005
    ASSERT(cpu.pc() == 0x1005);
    ASSERT(cpu.regRead(3) == 0xFF); // SP restored

    cpu.step(); // LDA #$BB
    ASSERT(cpu.regRead(0) == 0xBB);
}

TEST_CASE(cpu6502_nested_jsr_rts) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    cpu.setDataBus(&bus);

    // Inner subroutine at $3000: INX; RTS
    bus.write8(0x3000, 0xE8); // INX
    bus.write8(0x3001, 0x60); // RTS

    // Outer subroutine at $2000: JSR $3000; INX; RTS
    bus.write8(0x2000, 0x20); // JSR $3000
    bus.write8(0x2001, 0x00);
    bus.write8(0x2002, 0x30);
    bus.write8(0x2003, 0xE8); // INX
    bus.write8(0x2004, 0x60); // RTS

    // Main at $1000: LDX #$00; JSR $2000; STX $40; BRK
    bus.write8(0x1000, 0xA2); // LDX #$00
    bus.write8(0x1001, 0x00);
    bus.write8(0x1002, 0x20); // JSR $2000
    bus.write8(0x1003, 0x00);
    bus.write8(0x1004, 0x20);
    bus.write8(0x1005, 0x86); // STX $40
    bus.write8(0x1006, 0x40);

    cpu.setPc(0x1000);
    cpu.regWrite(3, 0xFF);
    cpu.regWrite(5, FLAG_U);

    cpu.step(); // LDX #$00
    cpu.step(); // JSR $2000
    ASSERT(cpu.pc() == 0x2000);
    ASSERT(cpu.regRead(3) == 0xFD);

    // Verify outer return address ($1004) on stack
    ASSERT(bus.read8(0x01FF) == 0x10);
    ASSERT(bus.read8(0x01FE) == 0x04);

    cpu.step(); // JSR $3000 (nested)
    ASSERT(cpu.pc() == 0x3000);
    ASSERT(cpu.regRead(3) == 0xFB);

    // Verify inner return address ($2002) on stack
    ASSERT(bus.read8(0x01FD) == 0x20);
    ASSERT(bus.read8(0x01FC) == 0x02);

    // Both return addresses intact
    ASSERT(bus.read8(0x01FF) == 0x10); // outer hi
    ASSERT(bus.read8(0x01FE) == 0x04); // outer lo

    cpu.step(); // INX (X=1)
    cpu.step(); // RTS -> $2003
    ASSERT(cpu.pc() == 0x2003);
    ASSERT(cpu.regRead(3) == 0xFD);

    cpu.step(); // INX (X=2)
    cpu.step(); // RTS -> $1005
    ASSERT(cpu.pc() == 0x1005);
    ASSERT(cpu.regRead(3) == 0xFF);

    cpu.step(); // STX $40
    ASSERT(bus.read8(0x0040) == 0x02);
}

TEST_CASE(cpu6502_flags_overflow) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    cpu.setDataBus(&bus);

    // ADC #$40 to #$40 -> $80 (V=1)
    cpu.regWrite(0, 0x40);
    cpu.regWrite(5, FLAG_U);
    bus.write8(0x1000, 0x69);
    bus.write8(0x1001, 0x40);
    cpu.setPc(0x1000);
    cpu.step();
    ASSERT(cpu.regRead(0) == 0x80);
    ASSERT(cpu.regRead(5) & FLAG_V);

    // ADC #$C0 to #$C0 -> $80 (V=1)
    cpu.regWrite(0, 0xC0);
    cpu.regWrite(5, FLAG_U | FLAG_C); // Clear C for next op
    bus.write8(0x1002, 0x69);
    bus.write8(0x1003, 0xC0);
    cpu.step();
    // 0xC0 + 0xC0 = 0x180. Carry = 1, A = 0x80.
    // -64 + -64 = -128. Signed result is correct, but bit 7 changed from 1 to 1? 
    // Actually V = (A^result) & (M^result) & 0x80
    // (0xC0^0x80) & (0xC0^0x80) & 0x80 = 0x40 & 0x40 & 0x80 = 0
    // Wait, the formula is V = ~(A^M) & (A^result) & 0x80
    // ~(0xC0^0xC0) & (0xC0^0x80) & 0x80 = ~0 & 0x40 & 0x80 = 0.
    // Let's try 0x80 + 0x80 = 0x100. A=0, C=1, V=1.
    // ~(0x80^0x80) & (0x80^0x00) & 0x80 = 0xFF & 0x80 & 0x80 = 0x80 (V=1).
    cpu.regWrite(0, 0x80);
    cpu.regWrite(5, FLAG_U);
    bus.write8(0x1004, 0x69);
    bus.write8(0x1005, 0x80);
    cpu.step();
    ASSERT(cpu.regRead(0) == 0x00);
    ASSERT(cpu.regRead(5) & FLAG_V);
    ASSERT(cpu.regRead(5) & FLAG_C);
}

// NMOS decimal mode (SED): ADC/SBC adjust to BCD. Values from Bruce Clark's
// "Decimal Mode in NMOS 6500 series" (6502.org): Z from the binary result,
// N/V from the sum before the high-nibble adjustment; SBC's flags as binary.
namespace {
struct DecResult { uint8_t a; bool c, z, n, v; };
DecResult decimalOp(bool sbc, uint8_t a, uint8_t m, bool carry) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    cpu.setDataBus(&bus);
    uint8_t prog[] = { 0xF8, (uint8_t)(carry ? 0x38 : 0x18), 0xA9, a, (uint8_t)(sbc ? 0xE9 : 0x69), m };
    for (size_t i = 0; i < sizeof(prog); ++i) bus.write8(0xC000 + (uint32_t)i, prog[i]);
    cpu.setPc(0xC000);
    for (int i = 0; i < 4; ++i) cpu.step();
    uint8_t p = (uint8_t)cpu.regRead(5);
    return { (uint8_t)cpu.regRead(0), (p & FLAG_C) != 0, (p & FLAG_Z) != 0, (p & FLAG_N) != 0, (p & FLAG_V) != 0 };
}
}  // namespace

TEST_CASE(cpu6502_decimal_adc) {
    auto r = decimalOp(false, 0x12, 0x34, false); ASSERT_EQ((int)r.a, 0x46); ASSERT(!r.c);
    r = decimalOp(false, 0x58, 0x46, true);  ASSERT_EQ((int)r.a, 0x05); ASSERT(r.c);    // 58+46+1 = 105
    r = decimalOp(false, 0x15, 0x26, false); ASSERT_EQ((int)r.a, 0x41); ASSERT(!r.c);
    r = decimalOp(false, 0x81, 0x92, false); ASSERT_EQ((int)r.a, 0x73); ASSERT(r.c);
    r = decimalOp(false, 0x99, 0x00, true);  ASSERT_EQ((int)r.a, 0x00); ASSERT(r.c);
    ASSERT(!r.z);                                         // binary $9A: NMOS Z is clear
    r = decimalOp(false, 0x0F, 0x01, false); ASSERT_EQ((int)r.a, 0x16);   // invalid BCD in
    r = decimalOp(false, 0x79, 0x00, true);  ASSERT_EQ((int)r.a, 0x80); ASSERT(r.n); ASSERT(r.v);
    r = decimalOp(false, 0x02, 0x03, false); ASSERT_EQ((int)r.a, 0x05);   // 2+3 (Atari BASIC's 2+3*4)
}

TEST_CASE(cpu6502_decimal_sbc) {
    auto r = decimalOp(true, 0x46, 0x12, true);  ASSERT_EQ((int)r.a, 0x34); ASSERT(r.c);
    r = decimalOp(true, 0x40, 0x13, true);  ASSERT_EQ((int)r.a, 0x27); ASSERT(r.c);
    r = decimalOp(true, 0x32, 0x02, false); ASSERT_EQ((int)r.a, 0x29); ASSERT(r.c);   // borrow in
    r = decimalOp(true, 0x12, 0x21, true);  ASSERT_EQ((int)r.a, 0x91); ASSERT(!r.c);  // borrow out
    r = decimalOp(true, 0x00, 0x01, true);  ASSERT_EQ((int)r.a, 0x99); ASSERT(!r.c); ASSERT(r.n);
    r = decimalOp(true, 0x21, 0x21, true);  ASSERT_EQ((int)r.a, 0x00); ASSERT(r.z); ASSERT(r.c);
}

TEST_CASE(cpu6502_binary_unchanged_without_sed) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    cpu.setDataBus(&bus);
    uint8_t prog[] = { 0xD8, 0x18, 0xA9, 0x09, 0x69, 0x05 };   // CLD CLC LDA #9 ADC #5
    for (size_t i = 0; i < sizeof(prog); ++i) bus.write8(0xC000 + (uint32_t)i, prog[i]);
    cpu.setPc(0xC000);
    for (int i = 0; i < 4; ++i) cpu.step();
    ASSERT_EQ((int)cpu.regRead(0), 0x0E);
}

// A read-modify-write instruction on the NMOS 6502 writes twice: the value it
// read, unchanged, then the result; the 65C02 writes only the result (#304:
// `ASL $D019` acknowledges a VIC-II interrupt with the first write)
namespace {
struct WriteRecorder : public FlatMemoryBus {
    WriteRecorder() : FlatMemoryBus("system", 16) {}
    std::vector<std::pair<uint32_t, uint8_t>> writes;
    void write8(uint32_t addr, uint8_t val) override {
        writes.push_back({addr, val});
        FlatMemoryBus::write8(addr, val);
    }
};
}

TEST_CASE(cpu6502_rmw_writes_twice) {
    // ASL $D019; INC $10; LSR $11,X; ROL $1234; ROR $12; DEC $13; DCP $14; ISC $15
    const uint8_t prog[] = {0x0E, 0x19, 0xD0, 0xE6, 0x10, 0x56, 0x11, 0x2E, 0x34, 0x12,
                            0x66, 0x12, 0xC6, 0x13, 0xC7, 0x14, 0xE7, 0x15};
    for (int cmos = 0; cmos < 2; ++cmos) {
        WriteRecorder bus;
        std::unique_ptr<MOS6502> core(cmos ? new MOS65C02() : new MOS6502());
        MOS6502& cpu = *core;
        cpu.setDataBus(&bus);
        for (size_t i = 0; i < sizeof(prog); ++i) bus.write8(0xC000 + (uint32_t)i, prog[i]);
        bus.write8(0xD019, 0x81);
        for (uint32_t a = 0x10; a <= 0x15; ++a) bus.write8(a, 0x40);
        bus.write8(0x1234, 0x40);
        cpu.setPc(0xC000);
        const int count = cmos ? 6 : 8;    // the 65C02 has no DCP/ISC
        for (int i = 0; i < count; ++i) {
            bus.writes.clear();
            cpu.step();
            if (cmos) {
                ASSERT_EQ((int)bus.writes.size(), 1);
            } else {
                ASSERT_EQ((int)bus.writes.size(), 2);
                ASSERT_EQ(bus.writes[0].first, bus.writes[1].first);
                if (i == 0) {
                    ASSERT_EQ(bus.writes[0].second, 0x81);   // the value read
                    ASSERT_EQ(bus.writes[1].second, 0x02);   // shifted
                }
            }
        }
    }
}
