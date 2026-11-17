#include "test_harness.h"
#include "plugins/devices/rex/main/rex.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

std::string tempFile(const std::string& tag) {
    return (std::filesystem::temp_directory_path() / ("mmsim_rex_" + tag + "_" + std::to_string(::getpid()) + ".bin"))
        .string();
}

// The REX command interface, as software drives it: reads at chosen addresses
struct Driver {
    Rex& r;
    uint64_t now = 0;
    uint8_t rd(uint16_t a) { return r.read(a, now++); }
    void key() { for (uint16_t a : {0x00B8, 0x01F2, 0x0234, 0x03B0, 0x0431, 0x05BF}) rd(a); }
    void sector(int s, bool deselect = false) { rd(0x0001); rd((uint16_t)(s | (deselect ? 0x40 : 0))); }
    // One flash bus write: REX: cmd 6 (address) then 2 (data); REX#: 6, data, address
    void write(uint16_t addr, uint8_t v) {
        if (r.kind() == Rex::Kind::Rex) {
            rd(0x0006); rd(addr); rd(v);
        } else {
            rd(0x0006); rd(v); rd(addr);
        }
    }
    // The AAA/AA and 555/55 unlock cycles through the dedicated commands
    void unlock() {
        if (r.kind() == Rex::Kind::Rex) {
            rd(0x00AA); rd(0x00AA);         // cmd 2 at xxAA (parameter $AA), data $AA
            rd(0x0055); rd(0x0000);         // cmd 5: 55 -> 55
        } else {
            rd(0x00AA); rd(0x00AA); rd(0x00AA);
            rd(0x0055); rd(0x0055); rd(0x0055);
        }
    }
    void command(uint8_t c) {
        if (r.kind() == Rex::Kind::Rex) { rd(0x00AA); rd(c); }
        else { rd(0x00AA); rd(c); rd(0x00AA); }
    }
    void program(uint32_t flat, uint8_t v) {
        sector((int)(flat >> 15));
        unlock();
        command(0xA0);
        write((uint16_t)(flat & 0x7FFF), v);
    }
    uint8_t status() { uint8_t s = rd(0x0003); rd(0x0000); return s; }
};

}  // namespace

// Reading, the key sequence, ID and status, and back
TEST_CASE(rex_key_and_id) {
    Rex r(Rex::Kind::Rex);
    r.flash().data()[0x0123] = 0x42;
    Driver d{r};
    ASSERT_EQ(d.rd(0x0123), 0x42);
    ASSERT_EQ(r.state(), 7);
    // A broken key sequence does nothing
    d.rd(0x00B8); d.rd(0x01F2); d.rd(0x0000);
    d.rd(0x0234);
    ASSERT_EQ(r.state(), 7);
    d.key();
    ASSERT_EQ(r.state(), 0);
    ASSERT_EQ(d.rd(0x0007), 0x11);          // REX
    ASSERT_EQ(d.rd(0x0000), 0x11);
    d.sector(5);
    ASSERT_EQ(d.status(), 0x80 | 0x40 | 5);
    d.sector(31, true);
    ASSERT_EQ(d.status(), 0x80 | 31);
    d.sector(0);
    ASSERT_EQ(d.rd(0x0004), 0xFF);          // cmd 4, then the read itself
    ASSERT_EQ(d.rd(0x0123), 0x42);
    ASSERT_EQ(d.rd(0x0000), 0x42);
    d.rd(0x0000);                           // cmd 0: back to reading
    ASSERT_EQ(r.state(), 7);
    ASSERT_EQ(r.peek(0x0123), 0x42);
    Rex s(Rex::Kind::RexSharp);
    Driver e{s};
    e.key();
    ASSERT_EQ(e.rd(0x0007), 0x13);          // REX#
}

// Programming through the socket (both models): bits only clear; busy then ready
TEST_CASE(rex_program) {
    for (auto kind : {Rex::Kind::Rex, Rex::Kind::RexSharp}) {
        Rex r(kind);
        Driver d{r};
        d.key();
        d.program(0x28123, 0x5A);           // sector 5, offset $0123
        ASSERT_EQ(r.flash().data()[0x28123], 0x5A);
        ASSERT_EQ(d.status() & 0x80, 0);    // busy right after
        d.now += AmdFlash::PROGRAM_CLOCKS;
        ASSERT_EQ(d.status() & 0x80, 0x80);
        d.program(0x28123, 0xF0);           // 0x5A & 0xF0
        ASSERT_EQ(r.flash().data()[0x28123], 0x50);
        // Read it back through the socket in reading mode
        d.sector(5);
        d.rd(0x0000);
        ASSERT_EQ(d.rd(0x0123), 0x50);
        ASSERT_EQ(r.flash().state(), AmdFlash::RO);
    }
}

// Sector and chip erase through the command sequence
TEST_CASE(rex_erase) {
    Rex r(Rex::Kind::Rex);
    auto& f = r.flash().data();
    std::fill(f.begin(), f.end(), 0x00);
    Driver d{r};
    d.key();
    // Sector erase of the 64 KB sector at $20000 (REX sector 4)
    d.sector(4);
    d.unlock(); d.command(0x80); d.unlock();
    d.write(0x0010, 0x30);
    ASSERT_EQ(f[0x20000], 0xFF);
    ASSERT_EQ(f[0x2FFFF], 0xFF);
    ASSERT_EQ(f[0x1FFFF], 0x00);
    ASSERT_EQ(f[0x30000], 0x00);
    // The boot sectors: $6000-$7FFF is one 8 KB sector
    d.sector(0);
    d.unlock(); d.command(0x80); d.unlock();
    d.write(0x6100, 0x30);
    ASSERT_EQ(f[0x6000], 0xFF);
    ASSERT_EQ(f[0x7FFF], 0xFF);
    ASSERT_EQ(f[0x5FFF], 0x00);
    ASSERT_EQ(f[0x8000], 0x00);
    // Chip erase (the AA@AA 10 sequence)
    d.unlock(); d.command(0x80); d.unlock(); d.command(0x10);
    ASSERT_EQ(f[0], 0xFF);
    ASSERT_EQ(f[0xFFFFF], 0xFF);
    ASSERT(r.flash().busy(d.now));
}

// The image persists: created erased, written back by flush(), read on open
TEST_CASE(rex_persist) {
    const std::string path = tempFile("persist");
    std::filesystem::remove(path);
    {
        Rex r(Rex::Kind::Rex);
        ASSERT(r.open(path));
        ASSERT_EQ(std::filesystem::file_size(path), Rex::SIZE);
        Driver d{r};
        d.key();
        d.program(0x9ABCD, 0x12);
        ASSERT(r.flash().dirty());
        ASSERT(r.flush());
        ASSERT(!r.flash().dirty());
    }
    std::ifstream in(path, std::ios::binary);
    in.seekg(0x9ABCD);
    ASSERT_EQ(in.get(), 0x12);
    Rex r2;
    ASSERT(r2.open(path));
    ASSERT_EQ(r2.flash().data()[0x9ABCD], 0x12);
    ASSERT_EQ(r2.flash().data()[0x9ABCE], 0xFF);
    std::filesystem::remove(path);
}

// Autoselect and CFI reads (the Am29LV160D's, byte mode), and F0 back to the array
TEST_CASE(rex_flash_autoselect_cfi) {
    AmdFlash f;
    f.data()[0] = 0x12;
    f.data()[2] = 0x34;
    f.write(0xAA, 0xAA, 0);
    f.write(0x55, 0x55, 0);
    f.write(0xAA, 0x90, 0);
    ASSERT_EQ(f.read(0x00), 0x01);                     // AMD
    ASSERT_EQ(f.read(0x02), 0x49);                     // Am29LV160DB
    ASSERT_EQ(f.read(0x8004), 0x00);                   // sector not protected
    f.write(0x00, 0xF0, 0);
    ASSERT_EQ(f.read(0x00), 0x12);
    f.write(0xAA, 0x98, 0);                            // CFI query
    ASSERT_EQ(f.read(0x20), 'Q');
    ASSERT_EQ(f.read(0x22), 'R');
    ASSERT_EQ(f.read(0x24), 'Y');
    ASSERT_EQ(f.read(0x4E), 0x15);                     // 2^21 bytes
    ASSERT_EQ(f.read(0x58), 0x04);                     // four erase regions
    ASSERT_EQ(f.read(0x80), 'P');
    ASSERT_EQ(f.read(0x10), 0xFF);
    f.write(0x00, 0xF0, 0);
    ASSERT_EQ(f.read(0x02), 0x34);
}

// The REXCPM: the key (high address bytes B8 F2 C4 ED A1 98 on the RAM reads
// counted 1, 5, 9, 13, 17, 21), then the reads counted 23 and 26 set and
// read back a window register; write protection; a mistimed key starts over
#include "plugins/devices/rex/main/rexcpm.h"
namespace {
struct CpmDriver {
    RexCpm& c;
    std::vector<uint8_t> rom = std::vector<uint8_t>(0x8000, 0x77);
    uint8_t rd(uint16_t a) { return c.read(a, false, rom); }
    void key() {
        const uint8_t k[6] = {0xB8, 0xF2, 0xC4, 0xED, 0xA1, 0x98};
        rd(0x8000);              // an ordinary RAM read (as the firmware's code fetches): the count restarts
        for (int i = 0; i < 6; ++i) {
            rd((uint16_t)(k[i] << 8));
            if (i < 5) for (int f = 0; f < 3; ++f) rd(0x0000);     // three other reads (the ROM)
        }
    }
    // Reads 22-26: `hi` at 23, `lo` at 26; the answer
    uint8_t reg(uint16_t hi, uint16_t lo) {
        rd(0x0000);
        rd(hi);
        rd(0x0000);
        rd(0x0000);
        return rd(lo);
    }
};
}  // namespace

TEST_CASE(rexcpm_registers) {
    RexCpm c;
    c.reset();
    CpmDriver d{c};
    ASSERT_EQ(c.lowerBlock(), 0);
    ASSERT_EQ(c.upperBlock(), 1);
    ASSERT_EQ(c.optionBlock(), 1);
    ASSERT(c.writeProtect());
    ASSERT_EQ(d.rd(0x1234), 0x77);                     // the main ROM
    c.write(0x8001, 0xAB, false);                      // block 0: writable
    ASSERT_EQ(d.rd(0x8001), 0xAB);
    ASSERT_EQ(c.ram()[1], 0xAB);
    // RL = $53: high nibble 5 at 23 (bits 14-12 = 2), low 3 at 26
    d.key();
    ASSERT_EQ(d.reg(0xA500, 0xA300), 0x53);
    ASSERT_EQ(c.lowerBlock(), 0x53);
    c.write(0x8000, 0x11, false);                      // block $53 >= 8: protected
    ASSERT_EQ(c.ram()[(size_t)0x53 << 14], 0x00);
    // Write protect off (bits 14-12 = 0, bit 8 = 0)
    d.key();
    ASSERT_EQ(d.reg(0x8000, 0x8000), 0);
    ASSERT(!c.writeProtect());
    c.write(0x8000, 0x11, false);
    ASSERT_EQ(c.ram()[(size_t)0x53 << 14], 0x11);
    ASSERT_EQ(d.rd(0x8000), 0x11);
    // O = 32 KB block 9 (high bits 1 -> 8, low 1): the option ROM
    d.key();
    ASSERT_EQ(d.reg(0x9100, 0x9200), 9 * 2);
    ASSERT_EQ(c.optionBlock(), 9);
    c.write(0x0005, 0x5A, true);
    ASSERT_EQ(c.read(0x0005, true, d.rom), 0x5A);
    ASSERT_EQ(c.ram()[((size_t)9 << 15) + 5], 0x5A);
    ASSERT_EQ(c.read(0x0005, false, d.rom), 0x77);    // the main ROM when not selected
    // The model, without changing anything
    d.key();
    ASSERT_EQ(d.reg(0xC000, 0xC000), RexCpm::MODEL);
    // A key read one count late starts over: RL stays
    for (int f = 0; f < 3; ++f) d.rd(0x0000);
    d.rd(0xB800);
    for (int f = 0; f < 4; ++f) d.rd(0x0000);
    d.rd(0xF200);                                      // counted 6, not 5
    for (int i = 0; i < 20; ++i) d.rd(0xA000);
    ASSERT_EQ(c.lowerBlock(), 0x53);
    // Reset: the power-on windows, protected again
    c.reset();
    ASSERT(c.writeProtect() && c.lowerBlock() == 0 && c.upperBlock() == 1 && c.optionBlock() == 1);
}
