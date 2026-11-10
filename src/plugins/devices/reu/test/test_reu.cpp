#include "test_harness.h"
#include "plugins/devices/reu/main/reu.h"
#include "libmem/main/memory_bus.h"
#include "libdevices/main/simple_signal_line.h"
#include <cstdio>
#include <string>
#include <vector>

// The REU against VICE's reu.c: registers, the four transfer types, address
// fixing, autoload, the $FF00 trigger, the IRQ, and how each size wraps
namespace {
struct Rig {
    FlatMemoryBus bus{"c64", 16};
    SimpleSignalLine irq;
    Reu reu;
    explicit Rig(uint32_t size = 512 * 1024) : reu(size) {
        irq.set(false);
        reu.setDmaBus(&bus);
        reu.setIrqLine(&irq);
    }
    void w(int r, uint8_t v) { reu.ioWrite(&bus, 0xDF00 + (uint32_t)r, v); }
    uint8_t r(int reg) { uint8_t v = 0; reu.ioRead(&bus, 0xDF00 + (uint32_t)reg, &v); return v; }
    // Set up a transfer: the C64 address, the REU address (with its bank), the length
    void setup(uint16_t host, uint32_t reuAddr, uint16_t len) {
        w(2, (uint8_t)host); w(3, (uint8_t)(host >> 8));
        w(4, (uint8_t)reuAddr); w(5, (uint8_t)(reuAddr >> 8)); w(6, (uint8_t)(reuAddr >> 16));
        w(7, (uint8_t)len); w(8, (uint8_t)(len >> 8));
    }
    // Run the DMA to the end: the cycles it held the bus
    int run() {
        int cycles = 0;
        for (int c; (c = reu.busMasterCycle()) > 0;) cycles += c;
        return cycles;
    }
    int go(uint8_t command) { w(1, (uint8_t)(command | 0x90)); return run(); }   // execute, no $FF00
    uint16_t host() const { return (uint16_t)(reu.reg(2) | reu.reg(3) << 8); }
    uint32_t reuAddr() const { return (uint32_t)(reu.reg(4) | reu.reg(5) << 8); }
};
}  // namespace

TEST_CASE(reu_registers) {
    Rig t;
    EXPECT_EQ(t.reu.reg(0), 0x10);                  // 256K chips, version 0
    EXPECT_EQ(t.reu.reg(1), 0x10);                  // $FF00 trigger off
    EXPECT_EQ(t.reu.reg(6), 0xF8);                  // bank: bits 3-7 read 1
    EXPECT_EQ(t.reu.reg(7), 0xFF);
    EXPECT_EQ(t.reu.reg(8), 0xFF);
    EXPECT_EQ(t.reu.reg(9), 0x1F);
    EXPECT_EQ(t.reu.reg(10), 0x3F);
    EXPECT_EQ(t.r(0x0B), 0xFF);                     // unused
    EXPECT_EQ(t.r(0x1F), 0xFF);
    t.w(0x22, 0x34);                                // mirrored every 32 bytes
    EXPECT_EQ(t.r(0x02), 0x34);
    EXPECT_EQ(t.r(0xE2), 0x34);
    t.w(6, 0xFF);
    EXPECT_EQ(t.r(6), 0xFF);
    EXPECT_EQ(t.reu.reg(6), 0xFF);
    uint8_t v = 0;
    EXPECT_TRUE(!t.reu.ioRead(&t.bus, 0xDE00, &v)); // I/O1 isn't its
    EXPECT_EQ(Rig(128 * 1024).reu.reg(0), 0x00);   // the 1700: 64K chips
    EXPECT_EQ(Rig(2u << 20).reu.reg(6), 0x00);      // above 512K the bank is all there
}

TEST_CASE(reu_transfers) {
    Rig t;
    for (int i = 0; i < 256; ++i) t.bus.write8(0x1000 + (uint32_t)i, (uint8_t)(i * 3));
    // C64 -> REU: a byte a cycle; the addresses end past the block, length 1
    t.setup(0x1000, 0x020000, 256);
    EXPECT_EQ(t.go(0), 256);
    bool ok = true;
    for (int i = 0; i < 256; ++i) ok = ok && t.reu.ram()[0x20000 + (size_t)i] == (uint8_t)(i * 3);
    EXPECT_TRUE(ok);
    EXPECT_EQ(t.host(), 0x1100);
    EXPECT_EQ(t.reuAddr(), 0x0100);
    EXPECT_EQ(t.reu.reg(6), 0xFA);
    EXPECT_EQ(t.reu.reg(7), 1);
    EXPECT_EQ(t.reu.reg(8), 0);
    EXPECT_EQ(t.reu.reg(1) & 0x90, 0x10);           // execute off, $FF00 off
    EXPECT_EQ(t.r(0), 0x50);                        // end of block (read clears it)
    EXPECT_EQ(t.r(0), 0x10);
    // REU -> C64 with the C64 address fixed: every byte lands on $2000
    t.setup(0x2000, 0x020000, 4);
    t.w(10, 0x80);
    EXPECT_EQ(t.go(1), 4);
    EXPECT_EQ(t.bus.read8(0x2000), 9);              // the last of 0, 3, 6, 9
    EXPECT_EQ(t.bus.read8(0x2001), 0);
    EXPECT_EQ(t.host(), 0x2000);
    EXPECT_EQ(t.reuAddr(), 0x0004);
    t.w(10, 0);
    // Swap: two cycles a byte
    t.bus.write8(0x3000, 0xAA); t.bus.write8(0x3001, 0xBB);
    t.setup(0x3000, 0x020000, 2);
    EXPECT_EQ(t.go(2), 4);
    EXPECT_EQ(t.bus.read8(0x3000), 0);
    EXPECT_EQ(t.bus.read8(0x3001), 3);
    EXPECT_EQ(t.reu.ram()[0x20000], 0xAA);
    EXPECT_EQ(t.reu.ram()[0x20001], 0xBB);
    // Verify: equal blocks end with end of block; a mismatch stops it there
    t.setup(0x3000, 0x020000, 2);
    t.bus.write8(0x3000, 0xAA); t.bus.write8(0x3001, 0xBB);
    t.go(3);
    EXPECT_EQ(t.r(0), 0x50);
    t.reu.ram()[0x20005] = 0x77;
    for (int i = 0; i < 16; ++i) t.bus.write8(0x4000 + (uint32_t)i, t.reu.ram()[0x20000 + (size_t)i]);
    t.reu.ram()[0x20005] = 0x78;
    t.setup(0x4000, 0x020000, 16);
    EXPECT_EQ(t.go(3), 6 + 1);                       // six compared, a cycle more for the stop
    EXPECT_EQ(t.r(0), 0x30);                        // verify error, no end of block
    EXPECT_EQ(t.host(), 0x4006);
    EXPECT_EQ(t.reuAddr(), 0x0006);
    EXPECT_EQ(t.reu.reg(7), 10);
    // A mismatch on the last byte: end of block too, length 1
    t.setup(0x4004, 0x020004, 2);
    EXPECT_EQ(t.go(3), 2);
    EXPECT_EQ(t.r(0), 0x70);
    EXPECT_EQ(t.reu.reg(7), 1);
    // On the last byte but one: the last is compared as well (it matches:
    // end of block); the registers stop after the mismatch
    t.setup(0x4005, 0x020005, 2);
    EXPECT_EQ(t.go(3), 2);
    EXPECT_EQ(t.r(0), 0x70);
    EXPECT_EQ(t.host(), 0x4006);
    EXPECT_EQ(t.reu.reg(7), 1);
    t.bus.write8(0x4006, 0xEE);                      // ...and when it doesn't, no end of block
    t.setup(0x4005, 0x020005, 2);
    t.go(3);
    EXPECT_EQ(t.r(0), 0x30);
    // A length of 0 is 64 KB
    t.setup(0x0000, 0x000000, 0);
    EXPECT_EQ(t.go(0), 0x10000);
}

TEST_CASE(reu_autoload_trigger_irq) {
    Rig t;
    t.setup(0x1234, 0x056789, 0x0100);
    EXPECT_EQ(t.go(0x20), 0x100);                   // autoload: the registers come back
    EXPECT_EQ(t.host(), 0x1234);
    EXPECT_EQ(t.reuAddr(), 0x6789);
    EXPECT_EQ(t.reu.reg(6), 0xFD);
    EXPECT_EQ(t.reu.reg(7), 0x00);
    EXPECT_EQ(t.reu.reg(8), 0x01);
    // Execute without bit 4: armed until a write to $FF00
    t.w(1, 0x80);
    EXPECT_EQ(t.reu.busMasterCycle(), 0);
    EXPECT_TRUE(!t.reu.ioWrite(&t.bus, 0xFF00, 0));  // passed on to whatever is at $FF00
    EXPECT_TRUE(t.reu.dmaActive());
    EXPECT_EQ(t.run(), 0x100);
    t.reu.ioWrite(&t.bus, 0xFF00, 0);               // once only
    EXPECT_EQ(t.reu.busMasterCycle(), 0);
    // While the DMA runs the registers neither answer nor change
    t.w(1, 0x90);
    uint8_t v = 0;
    EXPECT_TRUE(!t.reu.ioRead(&t.bus, 0xDF00, &v));
    t.w(2, 0x55);
    EXPECT_EQ(t.reu.reg(2) == 0x55, false);
    t.run();
    // The IRQ: end of block with bits 7 and 6 of the mask; reading the status releases it
    t.r(0);
    t.w(9, 0xC0);
    t.setup(0x1000, 0, 8);
    t.go(0);
    EXPECT_TRUE(t.irq.get());
    EXPECT_EQ(t.reu.reg(0) & 0xC0, 0xC0);
    t.r(0);
    EXPECT_TRUE(!t.irq.get());
    // Enabling the mask with end of block already set raises it at once
    t.w(9, 0x00);
    t.go(0);
    EXPECT_TRUE(!t.irq.get());
    t.w(9, 0xC0);
    EXPECT_TRUE(t.irq.get());
    t.r(0);
    // A verify error with bit 5
    t.w(9, 0xA0);
    t.bus.write8(0x5000, 1);
    t.reu.ram()[0] = 2;
    t.setup(0x5000, 0, 4);
    t.go(3);
    EXPECT_TRUE(t.irq.get());
    // A reset: the registers, not the RAM
    t.reu.ram()[123] = 0x5A;
    t.reu.reset();
    EXPECT_TRUE(!t.irq.get());
    EXPECT_EQ(t.reu.reg(1), 0x10);
    EXPECT_EQ(t.reu.ram()[123], 0x5A);
}

// The REC counts 19 bits: a 512K unit wraps to 0; a 1764's upper 256 KB
// float; a 1700 wraps at 128 KB; above 512 KB the bank's upper bits are a
// latch (the "1750 XL" wrap at each 512 KB)
TEST_CASE(reu_sizes_wrap) {
    {
        Rig t;
        t.bus.write8(0x1000, 0x11); t.bus.write8(0x1001, 0x22);
        t.setup(0x1000, 0x07FFFF, 2);
        t.go(0);
        EXPECT_EQ(t.reu.ram()[0x7FFFF], 0x11);
        EXPECT_EQ(t.reu.ram()[0], 0x22);
        EXPECT_EQ(t.reu.reg(6), 0xF8);              // bank 0 again
    }
    {
        Rig t(256 * 1024);
        t.bus.write8(0x1000, 0x33);
        t.setup(0x1000, 0x040000, 1);              // unbacked: lost
        t.go(0);
        t.setup(0x1000, 0x000000, 1);
        t.go(0);
        t.bus.write8(0x1000, 0);
        t.setup(0x1000, 0x0C0000, 1);              // bank 12 is bank 4's mirror: floats
        t.go(1);
        EXPECT_EQ(t.reu.ram().size(), 256u * 1024);
        t.setup(0x1001, 0x080000, 1);              // bank 8: bank 0 again
        t.go(1);
        EXPECT_EQ(t.bus.read8(0x1001), 0x33);
    }
    {
        Rig t(128 * 1024);
        t.bus.write8(0x1000, 0x44);
        t.setup(0x1000, 0x01FFFF, 2);
        t.go(0);
        EXPECT_EQ(t.reu.ram()[0x1FFFF], 0x44);
        EXPECT_EQ(t.reu.ram()[0], t.bus.read8(0x1001));
        t.setup(0x1002, 0x060000, 1);              // bank 6 is bank 0 on a 1700
        t.go(1);
        EXPECT_EQ(t.bus.read8(0x1002), t.reu.ram()[0]);
    }
    {
        Rig t(16u << 20);
        t.bus.write8(0x1000, 0x55); t.bus.write8(0x1001, 0x66);
        t.setup(0x1000, 0xFFFFFF, 2);
        t.go(0);
        EXPECT_EQ(t.reu.ram()[0xFFFFFF], 0x55);
        EXPECT_EQ(t.reu.ram()[0xF80000], 0x66);    // the counter wraps; the latch doesn't
        EXPECT_EQ(t.reu.reg(6), 0xF8);
    }
}

// What size-detection code does: a signature in each bank from the top
// down, then count the banks that kept theirs
TEST_CASE(reu_size_probe) {
    for (uint32_t kb : {128u, 256u, 512u, 1024u, 2048u, 4096u, 8192u, 16384u}) {
        Rig t(kb * 1024);
        for (int b = 255; b >= 0; --b) {
            t.bus.write8(0x1000, (uint8_t)b); t.bus.write8(0x1001, (uint8_t)~b);
            t.setup(0x1000, (uint32_t)b << 16, 2);
            t.go(0);
        }
        int banks = 0;
        for (int b = 0; b < 256; ++b) {
            t.setup(0x2000, (uint32_t)b << 16, 2);
            t.go(1);
            if (t.bus.read8(0x2000) != (uint8_t)b || t.bus.read8(0x2001) != (uint8_t)~b) break;
            ++banks;
        }
        if (banks * 64 != (int)kb) printf("  %u KB: %d banks\n", kb, banks);
        EXPECT_EQ(banks * 64, (int)kb);
    }
}

TEST_CASE(reu_options_and_images) {
    Reu reu;
    std::string err;
    EXPECT_TRUE(reu.setOption("size", "16M", err));
    EXPECT_EQ(reu.size(), 16u << 20);
    EXPECT_TRUE(reu.setOption("size", "128k", err));
    EXPECT_EQ(reu.size(), 128u * 1024);
    EXPECT_TRUE(!reu.setOption("size", "3M", err));
    EXPECT_TRUE(!reu.setOption("size", "32M", err));
    EXPECT_TRUE(!reu.setOption("colour", "red", err));
    EXPECT_TRUE(reu.setOption("size", "256K", err));
    reu.ram()[0x3FFFF] = 0xA5;
    const std::string path = "/tmp/mmemu_test_reu.reu";
    EXPECT_TRUE(reu.saveImage(path, err));
    Reu other;
    EXPECT_TRUE(other.setOption("image", path, err));
    EXPECT_EQ(other.size(), 256u * 1024);
    EXPECT_EQ(other.ram()[0x3FFFF], 0xA5);
    EXPECT_EQ(other.reg(0), 0x10);
    std::remove(path.c_str());
    EXPECT_TRUE(!other.setOption("image", "/nonexistent/x.reu", err));
}

// A swap against a badline (#309), after Denise's Reu::swap and VICE's
// testprogs REU/reutiming2 e5: BA low in cycles 12-54 of a line. Reads at 8
// and 10, writes at 9 and 11; the third byte is read at 12, read again at
// 14 (BA low since 12, under four cycles), and written at 55; the fourth is
// read at 56 and written at 57
namespace {
struct BadlineBa : IBaSource {
    int clock = 0;                                  // the next cycle to run
    bool baLow(int ahead) const override {
        const int c = (clock + ahead) % 63;
        return c >= 12 && c <= 54;
    }
};
}  // namespace

TEST_CASE(reu_swap_badline_timing_and_c64c_glue) {
    for (int c64c = 0; c64c < 2; ++c64c) {
        Rig t;
        BadlineBa ba;
        std::vector<uint8_t> ram(0x10000, 0x42);    // the RAM under I/O
        t.reu.setBaSource(&ba);
        t.reu.setRamData(ram.data());
        std::string err;
        EXPECT_TRUE(t.reu.setOption("glue", c64c ? "c64c" : "c64", err));
        for (int i = 0; i < 4; ++i) t.bus.write8(0x5000 + (uint32_t)i, (uint8_t)(0x11 * (i + 1)));
        t.setup(0x5000, 0, 4);
        t.w(1, 0x92);                               // swap, execute now
        ba.clock = 8;
        int cycles = 0;
        for (int c; (c = t.reu.busMasterCycle()) > 0;) { cycles += c; ba.clock += c; }
        EXPECT_EQ(cycles, 50);                      // cycles 8-57
        EXPECT_EQ(t.reu.ram()[0], 0x11);
        EXPECT_EQ(t.reu.ram()[1], 0x22);
        // Read with BA low: a C64C's glue gives the RAM under I/O
        EXPECT_EQ(t.reu.ram()[2], c64c ? 0x42 : 0x33);
        EXPECT_EQ(t.reu.ram()[3], 0x44);
        EXPECT_EQ(t.r(0) & 0x40, 0x40);             // end of block
    }
    // The first byte isn't read in the cycle BA falls
    Rig t;
    BadlineBa ba;
    t.reu.setBaSource(&ba);
    t.setup(0x5000, 0, 1);
    t.w(1, 0x92);
    ba.clock = 12;
    int cycles = 0;
    for (int c; (c = t.reu.busMasterCycle()) > 0;) { cycles += c; ba.clock += c; }
    EXPECT_EQ(cycles, 3);                           // 12 skipped, read 13, last byte written 14
}
