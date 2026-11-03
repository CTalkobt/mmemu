#include "test_harness.h"
#include "libdebug/main/sid_file.h"
#include "libmem/main/memory_bus.h"
#include "libdevices/main/io_registry.h"
#include "plugins/6502/main/cpu6502.h"
#include "plugins/devices/cia6526/main/cia6526.h"
#include <vector>
#include <string>

// PSID play driver: init once, then play at the song's rate (50 Hz VBI or the
// CIA timer), with $01 banked (the CLI used to call play back to back).

namespace {

/** A PSID v2 file: header + data at `load`. */
std::vector<uint8_t> makePsid(uint16_t load, uint16_t init, uint16_t play, uint32_t speed,
                              const std::vector<uint8_t>& data, int songs = 2) {
    std::vector<uint8_t> f(0x7C, 0);
    auto be = [&](size_t off, uint16_t v) { f[off] = (uint8_t)(v >> 8); f[off + 1] = (uint8_t)v; };
    f[0] = 'P'; f[1] = 'S'; f[2] = 'I'; f[3] = 'D';
    be(0x04, 2); be(0x06, 0x7C); be(0x08, load); be(0x0A, init); be(0x0C, play);
    be(0x0E, (uint16_t)songs); be(0x10, 1); be(0x12, (uint16_t)(speed >> 16)); be(0x14, (uint16_t)speed);
    const char* title = "Test Tune";
    for (int i = 0; title[i]; ++i) f[0x16 + i] = (uint8_t)title[i];
    be(0x76, 0x0014);   // PAL, 6581
    f.insert(f.end(), data.begin(), data.end());
    return f;
}

/** A 6502 with CIA 1 at $DC00 on the bus, run for `cycles`. */
struct Machine {
    FlatMemoryBus bus{"system", 16};
    MOS6502 cpu;
    IORegistry io;
    CIA6526* cia = new CIA6526("CIA1", 0xDC00);   // owned by the registry
    Machine() {
        cpu.setDataBus(&bus);
        io.registerHandler(cia);
        bus.setIoHooks([this](IBus* b, uint32_t a, uint8_t* v) { return io.dispatchRead(b, a, v); },
                       [this](IBus* b, uint32_t a, uint8_t v) { return io.dispatchWrite(b, a, v); });
        bus.setIoLowBase(0xD000);
    }
    void run(uint64_t cycles) {
        uint64_t end = cpu.cycles() + cycles;
        while (cpu.cycles() < end) {
            uint64_t before = cpu.cycles();
            cpu.step();
            io.tickAll(cpu.cycles() - before);
        }
    }
};

// init ($1000): STA $FB, then STX $FD (the bank seen: LDX $01), RTS
// play ($1010): INC $FC, LDA $01, STA $FE, RTS
std::vector<uint8_t> tuneCode(bool setTimer100Hz) {
    std::vector<uint8_t> c(0x20, 0xEA);
    std::vector<uint8_t> init = {0x85, 0xFB, 0xA6, 0x01, 0x86, 0xFD};
    if (setTimer100Hz) {   // CIA 1 timer A = 9852 cycles: 100 Hz
        uint16_t t = 9852 - 1;
        std::vector<uint8_t> s = {0xA9, (uint8_t)t, 0x8D, 0x04, 0xDC, 0xA9, (uint8_t)(t >> 8), 0x8D, 0x05, 0xDC};
        init.insert(init.end(), s.begin(), s.end());
    }
    init.push_back(0x60);
    std::copy(init.begin(), init.end(), c.begin());
    std::vector<uint8_t> play = {0xE6, 0xFC, 0xA5, 0x01, 0x85, 0xFE, 0x60};
    std::copy(play.begin(), play.end(), c.begin() + 0x10);
    return c;
}

}  // namespace

TEST_CASE(psid_vbi_song_plays_at_50hz) {
    Machine m;
    auto file = makePsid(0x1000, 0x1000, 0x1010, 0, tuneCode(false));
    psid::Info info;
    std::string err;
    ASSERT(psid::load(file, 2, &m.bus, &m.cpu, info, err));
    ASSERT_EQ(info.title, std::string("Test Tune"));
    ASSERT_EQ(info.subtune, 2);
    ASSERT(!info.ciaSpeed);
    ASSERT_EQ(info.sidModel, 1);
    ASSERT_EQ((int)info.driverAddr, 0x0334);
    ASSERT_EQ((int)m.cpu.pc(), 0x0334);

    m.run(985248);                                   // one second
    ASSERT_EQ((int)m.bus.peek8(0xFB), 1);            // init got song 2 as 1
    ASSERT_EQ((int)m.bus.peek8(0xFD), 0x37);         // $01 for code below $A000
    int calls = m.bus.peek8(0xFC);
    ASSERT(calls >= 49 && calls <= 51);              // 50 Hz (was: as fast as the CPU runs)
    ASSERT_EQ((int)m.bus.peek8(0xFE), 0x37);
    ASSERT(psid::describe(info).find("50 Hz") != std::string::npos);
}

TEST_CASE(psid_cia_speed_song_uses_its_timer) {
    Machine m;
    auto file = makePsid(0x1000, 0x1000, 0x1010, 0x1, tuneCode(true));   // song 1: CIA speed
    psid::Info info;
    std::string err;
    ASSERT(psid::load(file, 1, &m.bus, &m.cpu, info, err));
    ASSERT(info.ciaSpeed);
    m.run(985248);
    int calls = m.bus.peek8(0xFC);
    ASSERT(calls >= 99 && calls <= 101);             // the 100 Hz init set
}

TEST_CASE(psid_driver_avoids_the_tune_and_banks_rom_areas) {
    // Data covering $0300-$04FF: the driver moves to $0200
    Machine m;
    std::vector<uint8_t> big(0x200, 0x60);           // RTS everywhere
    auto file = makePsid(0x0300, 0x0300, 0x0301, 0, big);
    psid::Info info;
    std::string err;
    ASSERT(psid::load(file, 1, &m.bus, &m.cpu, info, err));
    ASSERT_EQ((int)info.driverAddr, 0x0200);

    ASSERT_EQ((int)psid::bankFor(0x9FFF), 0x37);
    ASSERT_EQ((int)psid::bankFor(0xA000), 0x36);
    ASSERT_EQ((int)psid::bankFor(0xD000), 0x34);
    ASSERT_EQ((int)psid::bankFor(0xE000), 0x35);

    std::vector<uint8_t> bad = {'X', 'S', 'I', 'D'};
    bad.resize(0x80, 0);
    ASSERT(!psid::load(bad, 1, &m.bus, &m.cpu, info, err));
    ASSERT(err.find("Not a PSID") != std::string::npos);
}
