#include "test_harness.h"
#include "plugins/machines/atari2600/main/machine_a2600.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ivideo_output.h"
#include "libmem/main/ibus.h"
#include <cstdio>
#include <string>
#include <vector>

namespace {
// A 4K cartridge: `code` at $F000, the reset vector there
std::vector<uint8_t> cart4k(std::vector<uint8_t> code) {
    std::vector<uint8_t> rom(4096, 0xEA);
    std::copy(code.begin(), code.end(), rom.begin());
    rom[0xFFC] = 0x00; rom[0xFFD] = 0xF0;
    return rom;
}
uint64_t run(MachineDescriptor* m, uint64_t cycles) {
    uint64_t t = 0;
    while (t < cycles) t += (uint64_t)m->schedulerStep(*m);
    return t;
}
}  // namespace

// The memory map: RIOT RAM at $80, mirrored at $180; the cartridge at $F000
TEST_CASE(a2600_boot_ram) {
    std::string err;
    MachineDescriptor* m = a2600::create(false, &err);
    ASSERT(m);
    // SEI; CLD; LDX #$FF; TXS; LDA #$42; STA $80; LDA $180; STA $81; JMP *
    ASSERT(a2600::insertCartridge(m, cart4k({0x78, 0xD8, 0xA2, 0xFF, 0x9A, 0xA9, 0x42, 0x85, 0x80,
                                             0xAD, 0x80, 0x01, 0x85, 0x81, 0x4C, 0x0E, 0xF0}), "t.a26", err));
    EXPECT_EQ(a2600::cartridgeInfo(m), std::string("t.a26 (4K)"));
    EXPECT_EQ(m->cpus[0].cpu->pc(), 0xF000);
    run(m, 200);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0x80), 0x42);
    EXPECT_EQ(bus->peek8(0x81), 0x42);
    delete m;
}

// WSYNC holds the CPU to the end of the line: a loop of STA WSYNC takes
// exactly 76 cycles a pass (228 colour clocks)
TEST_CASE(a2600_wsync) {
    std::string err;
    MachineDescriptor* m = a2600::create(false, &err);
    ASSERT(m);
    // loop: STA WSYNC ($02); INC $80; JMP loop
    ASSERT(a2600::insertCartridge(m, cart4k({0x85, 0x02, 0xE6, 0x80, 0x4C, 0x00, 0xF0}), "w.a26", err));
    run(m, 76 * 3);                                 // settle on a line
    IBus* bus = m->buses[0].bus;
    uint8_t before = bus->peek8(0x80);
    uint64_t t = run(m, 76 * 100);
    int lines = (uint8_t)(bus->peek8(0x80) - before);
    EXPECT_EQ(lines, (int)(t / 76));
    delete m;
}

// F8: an access to $1FF8 / $1FF9 picks bank 0 / 1; it starts in bank 1
TEST_CASE(a2600_f8_banks) {
    std::string err;
    MachineDescriptor* m = a2600::create(false, &err);
    ASSERT(m);
    std::vector<uint8_t> rom(8192, 0xEA);
    // bank 1 (start): $F000: LDA $1FF8 -> bank 0 continues at $F003
    const uint8_t b1[] = {0xAD, 0xF8, 0x1F};
    std::copy(b1, b1 + 3, rom.begin() + 4096);
    // bank 0 at $F003: LDA #$77; STA $80; LDA $1FF9 (back to 1); JMP $F00C
    const uint8_t b0[] = {0xA9, 0x77, 0x85, 0x80, 0xAD, 0xF9, 0x1F, 0x4C, 0x0C, 0xF0};
    std::copy(b0, b0 + sizeof b0, rom.begin() + 3);
    // bank 1 at $F00A: (after the switch back) LDA #$88; STA $81; JMP *
    const uint8_t b1b[] = {0xA9, 0x88, 0x85, 0x81, 0x4C, 0x0E, 0xF0};
    std::copy(b1b, b1b + sizeof b1b, rom.begin() + 4096 + 10);
    rom[4096 + 0xFFC] = 0x00; rom[4096 + 0xFFD] = 0xF0;
    ASSERT_EQ(a2600::detectType(rom), std::string("F8"));
    ASSERT(a2600::insertCartridge(m, rom, "f8.bin", err));
    run(m, 100);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0x80), 0x77);
    EXPECT_EQ(bus->peek8(0x81), 0x88);
    // Detection (Stella CartDetector)
    EXPECT_EQ(a2600::detectType(std::vector<uint8_t>(2048, 1)), std::string("2K"));
    std::vector<uint8_t> sc(16384, 0);                 // each bank repeats its first 128 bytes
    EXPECT_EQ(a2600::detectType(sc), std::string("F6SC"));
    delete m;
}

// Joysticks on SWCHA and fire on INPT4; the console switches on SWCHB
TEST_CASE(a2600_inputs) {
    std::string err;
    MachineDescriptor* m = a2600::create(false, &err);
    ASSERT(m);
    // loop: LDA SWCHA; STA $80; LDA SWCHB; STA $81; LDA INPT4; STA $82; JMP loop
    ASSERT(a2600::insertCartridge(m, cart4k({0xAD, 0x80, 0x02, 0x85, 0x80, 0xAD, 0x82, 0x02, 0x85, 0x81,
                                             0xA5, 0x0C, 0x85, 0x82, 0x4C, 0x00, 0xF0}), "i.a26", err));
    IBus* bus = m->buses[0].bus;
    run(m, 100);
    EXPECT_EQ(bus->peek8(0x80), 0xFF);
    EXPECT_EQ(bus->peek8(0x81) & 0x0B, 0x0B);
    EXPECT_EQ(bus->peek8(0x82) & 0x80, 0x80);       // fire up
    m->onJoystick(0, 0x1F & ~0x08 & ~0x10);         // P0 right + fire
    m->onKey("f2", true);                           // RESET
    run(m, 100);
    EXPECT_EQ(bus->peek8(0x80), 0x7F);              // bit 7: P0 right
    EXPECT_EQ(bus->peek8(0x81) & 0x01, 0x00);
    EXPECT_EQ(bus->peek8(0x82) & 0x80, 0x00);
    delete m;
}
