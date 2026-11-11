// The Game Boy machine (machine_gameboy.h): hand-built cartridges run through
// the scheduler, the SM83 through ICore (registers, steps, breakpoints and
// watchpoints through an observer, an interrupt's handler stopped at), the
// screen, the sound, the joypad, battery saves, and a Blargg test ROM run as
// the frontends run it (MMEMU_GB_TEST_ROMS; skipped when absent).

#include "test_harness.h"
#include "plugins/machines/gameboy/main/gameboy.h"
#include "plugins/machines/gameboy/main/machine_gameboy.h"
#include "plugins/machines/gameboy/main/sm83_core.h"
#include "libcore/main/machine_desc.h"
#include "libdebug/main/execution_observer.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ivideo_output.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

/** A cartridge of `type` with `prog` at $0150 (the entry at $0100 jumps there) and `isr` at $0040. */
std::vector<uint8_t> cart(const std::vector<uint8_t>& prog, const std::vector<uint8_t>& isr = {}, uint8_t type = 0,
                          uint8_t ram = 0) {
    std::vector<uint8_t> r(0x8000, 0);
    const uint8_t entry[] = {0x00, 0xC3, 0x50, 0x01};
    std::memcpy(&r[0x100], entry, 4);
    std::memcpy(&r[0x134], "MACHINE", 7);
    r[0x147] = type;
    r[0x149] = ram;
    std::memcpy(&r[0x150], prog.data(), prog.size());
    if (!isr.empty()) std::memcpy(&r[0x40], isr.data(), isr.size());
    return r;
}

struct Machine {
    std::unique_ptr<MachineDescriptor> m{gb::createMachine()};
    ICore* cpu() const { return m->cpus[0].cpu; }
    IBus* bus() const { return m->buses[0].bus; }
    bool insert(const std::vector<uint8_t>& rom, const std::string& save = "") {
        std::string err;
        return gb::insertCartridge(m.get(), rom, "test.gb", err, save);
    }
    uint64_t run(uint64_t cycles) {
        uint64_t t = 0;
        while (t < cycles) t += (uint64_t)m->schedulerStep(*m);
        return t;
    }
    template <class T> T* device() {
        std::vector<IOHandler*> hs;
        m->ioRegistry->enumerate(hs);
        for (auto* h : hs) if (auto* d = dynamic_cast<T*>(h)) return d;
        return nullptr;
    }
};

struct Recorder : ExecutionObserver {
    std::vector<uint32_t> steps, reads, writes;
    uint32_t breakAt = ~0u;
    int hits = 0;
    bool onStep(ICore*, IBus*, const DisasmEntry& e) override {
        steps.push_back(e.addr);
        if (e.addr == breakAt) { ++hits; return false; }
        return true;
    }
    void onMemoryRead(IBus*, uint32_t a, uint8_t) override { reads.push_back(a); }
    void onMemoryWrite(IBus*, uint32_t a, uint8_t, uint8_t) override { writes.push_back(a); }
};

int count(const std::vector<uint32_t>& v, uint32_t x) {
    int n = 0;
    for (auto a : v) n += a == x;
    return n;
}

}  // namespace

TEST_CASE(gameboy_machine_registers_and_steps) {
    Machine g;
    EXPECT_EQ(g.m->machineId, std::string("gameboy"));
    EXPECT_EQ(std::string(g.cpu()->isaName()), std::string("sm83"));
    EXPECT_EQ(gb::cartridgeInfo(g.m.get()), std::string(""));
    // LD A,$42; LD ($C000),A; LD BC,$1234; LD HL,$C000; INC (HL); JR *
    ASSERT(g.insert(cart({0x3E, 0x42, 0xEA, 0x00, 0xC0, 0x01, 0x34, 0x12, 0x21, 0x00, 0xC0, 0x34, 0x18, 0xFE})));
    EXPECT_TRUE(gb::cartridgeInfo(g.m.get()).find("test.gb (ROM") == 0);
    ICore* c = g.cpu();
    EXPECT_EQ(c->pc(), 0x0100u);
    EXPECT_EQ(c->sp(), 0xFFFEu);
    EXPECT_EQ(c->regReadByName("A"), 0x01u);
    EXPECT_EQ(c->regReadByName("F"), 0xB0u);
    EXPECT_EQ(c->regReadByName("HL"), 0x014Du);
    EXPECT_EQ(std::string(c->regDescriptor(c->regIndexByName("F"))->flagNames), std::string("ZNHC----"));

    EXPECT_EQ(g.m->schedulerStep(*g.m), 4);        // NOP
    EXPECT_EQ(c->pc(), 0x0101u);
    EXPECT_EQ(g.m->schedulerStep(*g.m), 16);       // JP $0150
    EXPECT_EQ(c->pc(), 0x0150u);
    char buf[64];
    c->disassembleOne(g.bus(), c->pc(), buf, sizeof buf);
    EXPECT_EQ(std::string(buf), std::string("LD A,$42"));
    EXPECT_EQ(g.m->schedulerStep(*g.m), 8);        // LD A,$42
    EXPECT_EQ(c->regReadByName("A"), 0x42u);
    EXPECT_EQ(g.m->schedulerStep(*g.m), 16);       // LD ($C000),A
    EXPECT_EQ(g.bus()->peek8(0xC000), 0x42);
    g.m->schedulerStep(*g.m);
    EXPECT_EQ(c->regReadByName("B"), 0x12u);
    EXPECT_EQ(c->regReadByName("C"), 0x34u);
    g.m->schedulerStep(*g.m);
    EXPECT_EQ(g.m->schedulerStep(*g.m), 12);       // INC (HL)
    EXPECT_EQ(g.bus()->peek8(0xC000), 0x43);
    EXPECT_EQ(c->pc(), 0x015Cu);
    EXPECT_TRUE(!c->isProgramEnd(g.bus()));

    // Registers and memory written by the debugger
    c->regWriteByName("A", 0x99);
    c->regWriteByName("F", 0xFF);
    EXPECT_EQ(c->regReadByName("A"), 0x99u);
    EXPECT_EQ(c->regReadByName("F"), 0xF0u);        // the low nibble is always 0
    c->regWriteByName("DE", 0xBEEF);
    EXPECT_EQ(c->regReadByName("D"), 0xBEu);
    EXPECT_EQ(c->regReadByName("E"), 0xEFu);
    g.bus()->write8(0xC100, 0x77);
    EXPECT_EQ(g.bus()->peek8(0xC100), 0x77);
    EXPECT_EQ(g.bus()->peek8(0xE100), 0x77);        // echo RAM
    g.bus()->write8(0x8000, 0x3C);                  // VRAM, whatever the PPU's mode
    EXPECT_EQ(g.bus()->peek8(0x8000), 0x3C);
    EXPECT_EQ(g.bus()->peek8(0xFF40), 0x91);        // LCDC

    // The register snapshot round trip
    std::vector<uint8_t> st(c->stateSize());
    c->saveState(st.data());
    c->regWriteByName("A", 0);
    c->setPc(0x0150);
    c->loadState(st.data());
    EXPECT_EQ(c->regReadByName("A"), 0x99u);
    EXPECT_EQ(c->pc(), 0x015Cu);
}

TEST_CASE(gameboy_machine_breakpoints_watchpoints_interrupts) {
    Machine g;
    // IE = VBlank; IF = 0; EI; JR *. The handler: LD HL,$C010; INC (HL); RETI
    ASSERT(g.insert(cart({0x3E, 0x01, 0xE0, 0xFF, 0xAF, 0xE0, 0x0F, 0xFB, 0x18, 0xFE}, {0x21, 0x10, 0xC0, 0x34, 0xD9})));
    Recorder rec;
    g.cpu()->setObserver(&rec);
    g.bus()->setObserver(&rec);
    rec.breakAt = 0x0040;
    // Run until the breakpoint on the handler: stopped before its first instruction, after the dispatch
    uint64_t t = 0;
    while (!rec.hits && t < 200000) t += (uint64_t)g.m->schedulerStep(*g.m);
    ASSERT(rec.hits == 1);
    EXPECT_EQ(g.cpu()->pc(), 0x0040u);
    EXPECT_EQ(g.cpu()->regReadByName("IME"), 0u);           // the dispatch cleared it
    EXPECT_EQ(g.bus()->peek8(0xC010), 0);
    const uint16_t sp = (uint16_t)g.cpu()->sp();
    EXPECT_EQ(sp, 0xFFFC);
    EXPECT_EQ(g.bus()->peek8(0xFFFC) | g.bus()->peek8(0xFFFD) << 8, 0x0158);   // pushed: the JR's address
    EXPECT_EQ(count(rec.steps, 0x0158) > 0, true);

    // Go on from it: the handler runs; the watch sees INC (HL) read and write $C010
    rec.breakAt = ~0u;
    rec.steps.clear();
    rec.reads.clear();
    rec.writes.clear();
    for (int i = 0; i < 3; ++i) g.m->schedulerStep(*g.m);
    EXPECT_EQ(rec.steps.size(), 3u);                        // LD HL; INC (HL); RETI
    EXPECT_EQ(rec.steps[0], 0x0040u);
    EXPECT_EQ(rec.steps[1], 0x0043u);
    EXPECT_EQ(rec.steps[2], 0x0044u);
    EXPECT_EQ(count(rec.reads, 0xC010), 1);
    EXPECT_EQ(count(rec.writes, 0xC010), 1);
    EXPECT_EQ(count(rec.reads, 0xFFFC), 1);                 // RETI's pops
    EXPECT_EQ(g.bus()->peek8(0xC010), 1);
    EXPECT_EQ(g.cpu()->pc(), 0x0158u);
    EXPECT_EQ(g.cpu()->regReadByName("IME"), 1u);

    // Another frame, another interrupt
    g.run(70224);
    EXPECT_EQ(g.bus()->peek8(0xC010), 2);
    // A debugger's write goes to the observer too
    rec.writes.clear();
    g.bus()->write8(0xC020, 1);
    EXPECT_EQ(count(rec.writes, 0xC020), 1);
    g.cpu()->setObserver(nullptr);
    g.bus()->setObserver(nullptr);
}

TEST_CASE(gameboy_machine_lock_up_ends_program) {
    Machine g;
    ASSERT(g.insert(cart({0xD3})));              // an unused opcode: interrupts off, locked
    g.run(64);
    EXPECT_TRUE(g.cpu()->isProgramEnd(g.bus()));
    EXPECT_EQ(g.cpu()->pc(), 0x0151u);
}

TEST_CASE(gameboy_machine_screen_sound_joypad) {
    Machine g;
    // BGP = $FF (every shade black); JR *
    ASSERT(g.insert(cart({0x3E, 0xFF, 0xE0, 0x47, 0x18, 0xFE})));
    auto* video = g.device<IVideoOutput>();
    auto* audio = g.device<IAudioOutput>();
    ASSERT(video && audio);
    EXPECT_EQ(video->getDimensions().width, 160);
    EXPECT_EQ(video->getDimensions().height, 144);
    g.run(70224 * 2);
    std::vector<uint32_t> px(160 * 144);
    video->renderFrame(px.data());
    int dark = 0;
    for (auto p : px) dark += p == 0xFF081820;
    EXPECT_EQ(dark, 160 * 144);

    // The boot chime's sound reaches the host
    EXPECT_EQ(audio->nativeSampleRate(), 44100);
    std::vector<float> s(44100);
    const int n = audio->pullSamples(s.data(), (int)s.size());
    EXPECT_TRUE(n > 1000);                      // two frames' worth (1470), give or take
    float peak = 0;
    for (int i = 0; i < n; ++i) peak = std::max(peak, s[i] < 0 ? -s[i] : s[i]);
    EXPECT_TRUE(peak > 0.01f);

    // The joypad: START on Return, read through $FF00 with the buttons selected
    EXPECT_TRUE(g.m->onKey("return", true));
    g.bus()->write8(0xFF00, 0x10);
    EXPECT_EQ(g.bus()->peek8(0xFF00) & 0x0F, 0x07);
    EXPECT_TRUE(g.m->onKey("return", false));
    EXPECT_EQ(g.bus()->peek8(0xFF00) & 0x0F, 0x0F);
    EXPECT_TRUE(g.m->onGamepad(0, MachineDescriptor::PAD_LEFT | MachineDescriptor::PAD_A));
    EXPECT_EQ(g.bus()->peek8(0xFF00) & 0x0F, 0x0E);         // A
    g.bus()->write8(0xFF00, 0x20);                           // the d-pad
    EXPECT_EQ(g.bus()->peek8(0xFF00) & 0x0F, 0x0D);         // left
    EXPECT_TRUE(!g.m->onKey("q", true));
}

TEST_CASE(gameboy_machine_battery_save) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("mmemu_gb_save_" + std::to_string(std::rand()));
    fs::create_directories(dir);
    const std::string save = (dir / "game.sav").string();
    // MBC1 + RAM + battery, 8 KB: enable the RAM, write $5A to $A000 and $A5 to $BFFF; JR *
    const auto rom = cart({0x3E, 0x0A, 0xEA, 0x00, 0x00, 0x3E, 0x5A, 0xEA, 0x00, 0xA0, 0x3E, 0xA5, 0xEA, 0xFF, 0xBF, 0x18, 0xFE},
                          {}, 0x03, 0x02);
    {
        Machine g;
        ASSERT(g.insert(rom, save));
        EXPECT_TRUE(gb::cartridgeInfo(g.m.get()).find("MBC1, battery") != std::string::npos);
        g.run(1000);
        EXPECT_EQ(g.bus()->peek8(0xA000), 0x5A);
        gb::flushSave(g.m.get());
    }
    std::vector<uint8_t> data;
    {
        std::ifstream f(save, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    ASSERT_EQ(data.size(), 0x2000u);
    EXPECT_EQ(data[0], 0x5A);
    EXPECT_EQ(data[0x1FFF], 0xA5);
    {
        // Loaded again with the cartridge; ejecting writes it back
        Machine g;
        ASSERT(g.insert(cart({0x18, 0xFE}, {}, 0x03, 0x02), save));
        auto* gbp = gb::gameboyOf(g.m.get());
        ASSERT(gbp);
        EXPECT_EQ(gbp->cartRam()[0], 0x5A);
        gbp->cartRam()[1] = 0x11;
        gb::removeCartridge(g.m.get());
        EXPECT_EQ(gb::cartridgeInfo(g.m.get()), std::string(""));
        g.run(1000);                               // with no cartridge it runs on ($FF: RST $38)
    }
    {
        std::ifstream f(save, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(data[1], 0x11);
    EXPECT_EQ(gb::savePathFor("/x/y/Game.v1.gb"), std::string("/x/y/Game.v1.sav"));
    fs::remove_all(dir);
}

TEST_CASE(gameboy_machine_blargg_rom) {
    const char* v = std::getenv("MMEMU_GB_TEST_ROMS");
    const std::string path = std::string(v ? v : "../refs/gameboy-test-roms") + "/blargg/cpu_instrs/individual/06-ld r,r.gb";
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::printf("  (skipped: %s not found)\n", path.c_str()); return; }
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Machine g;
    ASSERT(g.insert(rom));
    Recorder rec;                                  // with an observer, as under the debugger
    g.cpu()->setObserver(&rec);
    auto* gbp = gb::gameboyOf(g.m.get());
    uint64_t t = 0;
    while (t < 10ull * gb::Gameboy::CLOCK_HZ && gbp->serialOutput().find("assed") == std::string::npos &&
           gbp->serialOutput().find("ailed") == std::string::npos) {
        t += (uint64_t)g.m->schedulerStep(*g.m);
        if (rec.steps.size() > 4096) rec.steps.clear();
    }
    g.cpu()->setObserver(nullptr);
    EXPECT_TRUE(gbp->serialOutput().find("Passed") != std::string::npos);
}
