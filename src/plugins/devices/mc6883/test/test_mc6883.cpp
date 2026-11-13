#include "test_harness.h"
#include "plugins/devices/mc6883/main/mc6883.h"

// The control register: a bit set by a write to the odd address, cleared by the even
TEST_CASE(mc6883_register_writes) {
    Mc6883 sam;
    ASSERT_EQ(sam.reg(), 0);
    sam.access(0xFFC9, true);             // F1: video at $0400
    ASSERT_EQ(sam.f(), 0x0400);
    sam.access(0xFFC1, true);             // V0
    sam.access(0xFFC5, true);             // V2
    ASSERT_EQ(sam.v(), 5u);
    sam.access(0xFFC0, true);
    ASSERT_EQ(sam.v(), 4u);
    sam.access(0xFFDD, true);             // M1: 64K
    ASSERT_EQ(sam.memSize(), 2u);
    sam.access(0xFFD5, true);             // P1
    ASSERT(sam.page());
    sam.access(0xFFD7, true);             // R0
    ASSERT_EQ(sam.rate(), 1u);
    sam.access(0xFFDF, true);             // TY
    ASSERT(sam.mapType());
    sam.access(0xFFDE, true);
    ASSERT(!sam.mapType());
    // Reads don't change it
    sam.access(0xFFD9, false);
    ASSERT_EQ(sam.rate(), 1u);
    sam.reset();
    ASSERT_EQ(sam.reg(), 0);
}

// Map type 0: 32K RAM, Extended BASIC, Color BASIC, the cartridge, I/O
TEST_CASE(mc6883_address_decode) {
    Mc6883 sam;
    ASSERT_EQ(sam.select(0x0000, false), Mc6883::S_RAM);
    ASSERT_EQ(sam.select(0x7FFF, false), Mc6883::S_RAM);
    ASSERT_EQ(sam.select(0x8000, false), Mc6883::S_ROM0);
    ASSERT_EQ(sam.select(0xA000, false), Mc6883::S_ROM1);
    ASSERT_EQ(sam.select(0xC000, false), Mc6883::S_CTS);
    ASSERT_EQ(sam.select(0xFEFF, false), Mc6883::S_CTS);
    ASSERT_EQ(sam.select(0xFF00, false), Mc6883::S_IO0);
    ASSERT_EQ(sam.select(0xFF1F, false), Mc6883::S_IO0);
    ASSERT_EQ(sam.select(0xFF20, false), Mc6883::S_IO1);
    ASSERT_EQ(sam.select(0xFF40, false), Mc6883::S_IO2);
    ASSERT_EQ(sam.select(0xFF60, false), Mc6883::S_NONE);
    ASSERT_EQ(sam.select(0xFFF0, false), Mc6883::S_ROM1);   // the vectors from Color BASIC
    auto c = sam.access(0x8000, true);
    ASSERT(!c.write);                     // no RAM above $7FFF in map type 0
    // Map type 1: RAM to $FEFF; writes there go to RAM and strobe the ROM select
    sam.access(0xFFDF, true);
    ASSERT_EQ(sam.select(0x8000, false), Mc6883::S_RAM);
    ASSERT_EQ(sam.select(0xFEFF, false), Mc6883::S_RAM);
    ASSERT_EQ(sam.select(0xFF00, false), Mc6883::S_IO0);
    ASSERT_EQ(sam.select(0xFFFE, false), Mc6883::S_ROM1);
    c = sam.access(0xC000, true);
    ASSERT(c.write);
    ASSERT_EQ(c.s, Mc6883::S_CTS);
}

// The address multiplexer: 4K and 16K chips in two banks (RAS1 by A12 or
// A14); 64K one bank, with P1 putting the upper 32K at $0000
TEST_CASE(mc6883_memory_sizes) {
    Mc6883 sam;
    auto c = sam.decode(0x1234, false);   // 4K: A12 picks bank 1
    ASSERT_EQ(c.bank, 1);
    c = sam.decode(0x0234, false);
    ASSERT_EQ(c.bank, 0);
    ASSERT_EQ(c.row & 0x3F, 0x34);
    ASSERT_EQ(c.col & 0x3F, (0x0234 >> 6) & 0x3F);
    sam.access(0xFFDB, true);             // M0: 16K
    c = sam.decode(0x4000, false);
    ASSERT_EQ(c.bank, 1);
    c = sam.decode(0x3FFF, false);
    ASSERT_EQ(c.bank, 0);
    sam.access(0xFFDA, true);
    sam.access(0xFFDD, true);             // 64K
    c = sam.decode(0x8123, false);
    ASSERT_EQ(c.bank, 0);
    ASSERT(!c.ram);                       // map type 0: ROM up there
    c = sam.decode(0x0123, false);
    ASSERT_EQ(c.row, 0x23);
    ASSERT_EQ(c.col, 0x01);
    sam.access(0xFFD5, true);             // P1
    c = sam.decode(0x0123, false);
    ASSERT_EQ(c.col, 0x81);               // the same cell as $8123
}

// CPU rate: 16 ticks of 14.31818 MHz a slow cycle, 8 fast; changing rate
// costs a tick or more to stay in step with the VDG
TEST_CASE(mc6883_cpu_rate) {
    Mc6883 sam;
    ASSERT_EQ(sam.access(0x0000, false).ticks, 16);
    ASSERT_EQ(sam.access(0xA000, false).ticks, 16);
    sam.access(0xFFD9, true);             // R1: 1.78 MHz everywhere
    ASSERT_EQ(sam.access(0x0000, false).ticks, 15);
    ASSERT_EQ(sam.access(0x0000, false).ticks, 8);
    ASSERT_EQ(sam.access(0x0000, false).ticks, 8);
    sam.access(0xFFD8, true);             // back to slow
    sam.access(0xFFD7, true);             // R0: address-dependent
    int t = sam.access(0x0000, false).ticks;
    ASSERT(t == 16 || t == 17 || t == 25);
    ASSERT_EQ(sam.access(0x0000, false).ticks, 16);   // RAM: slow
    ASSERT_EQ(sam.access(0xA000, false).ticks, 15);   // ROM: fast
    ASSERT_EQ(sam.access(0xA001, false).ticks, 8);
    ASSERT_EQ(sam.access(0xA002, false).ticks, 8);
    ASSERT_EQ(sam.access(0xFF00, false).ticks, 17);   // $FF00-$FF1F: slow
    ASSERT_EQ(sam.access(0xA000, false).ticks, 15);
    ASSERT_EQ(sam.access(0xA001, false).ticks, 8);
    ASSERT_EQ(sam.access(0x0000, false).ticks, 25);   // out of step: a longer slow cycle
    ASSERT_EQ(sam.access(0xFF20, false).ticks, 15);   // $FF20 up: fast
}

// The video counter: FS loads it from F; in alphanumerics (V=0, X1 Y12) a
// row of 32 bytes is shown twelve times, then the next row; in V=6
// (X1 Y1) each line moves on 32
TEST_CASE(mc6883_video_counter) {
    Mc6883 sam;
    sam.access(0xFFC9, true);             // $0400
    sam.fsync(false);
    sam.fsync(true);
    ASSERT_EQ(sam.videoAddress(), 0x0400);
    uint16_t row, col;
    auto line = [&]() {
        // The VDG clocks 42 bytes in a 32-byte mode by the end of the line
        for (int left = 42; left > 0;) left -= sam.videoBytes(left, row, col);
        sam.hsync(false);
        sam.hsync(true);
    };
    for (int i = 0; i < 11; ++i) {
        line();
        ASSERT_EQ(sam.videoAddress(), 0x0400);
    }
    line();
    ASSERT_EQ(sam.videoAddress(), 0x0420);
    for (int i = 0; i < 12; ++i) line();
    ASSERT_EQ(sam.videoAddress(), 0x0440);

    sam.access(0xFFC3, true);             // V1, V2: mode 6
    sam.access(0xFFC5, true);
    sam.fsync(true);
    ASSERT_EQ(sam.videoAddress(), 0x0400);
    line();
    ASSERT_EQ(sam.videoAddress(), 0x0420);
    line();
    ASSERT_EQ(sam.videoAddress(), 0x0440);
    // The translated address of the first byte
    sam.videoBytes(1, row, col);
    ASSERT_EQ(row, 0x40 | 0x80);          // 4K mapping: row A0-A6, column A6-A11, bit 7 for RAS0
    ASSERT_EQ(col, 0x11 | 0x80);
}
