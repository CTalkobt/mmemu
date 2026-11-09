// The NES test ROMs (github.com/christopherpow/nes-test-roms: blargg's
// suites and others), when present: MMEMU_NES_TEST_ROMS, default
// ../refs/nes-test-roms. Skipped when the directory is missing.
//
// blargg's protocol: $6001-$6003 hold DE B0 61 once the test is running;
// $6000 is $80 while running, $81 when it wants the console reset, else the
// result (0 = passed); a text report is at $6004.

#include "test_harness.h"
#include "plugins/machines/nes/main/machine_nes.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/ibus.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string romDir() {
    const char* v = std::getenv("MMEMU_NES_TEST_ROMS");
    return v ? v : "../refs/nes-test-roms";
}

struct Result {
    int code = -1;             // -1: never started / timed out
    std::string text;
};

Result runBlargg(const std::string& path, uint64_t maxCycles = 60ull * 29781 * 60) {
    Result r;
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string err;
    MachineDescriptor* m = nes::create(false, &err);
    if (!nes::insertCartridge(m, img, path, err)) { r.text = err; delete m; return r; }
    IBus* bus = m->buses[0].bus;
    auto started = [&]() { return bus->peek8(0x6001) == 0xDE && bus->peek8(0x6002) == 0xB0 && bus->peek8(0x6003) == 0x61; };
    uint64_t t = 0, resetAt = 0;
    while (t < maxCycles) {
        t += (uint64_t)m->schedulerStep(*m);
        if ((t & 0x3FF) > 8) continue;
        if (!started()) continue;
        const uint8_t s = bus->peek8(0x6000);
        if (s == 0x81) {
            // Press reset ~100 ms after asked
            if (!resetAt) resetAt = t + 180000;
            else if (t >= resetAt) {
                resetAt = 0;
                // a soft reset: the CPU and chips, not RAM or the cartridge's work RAM
                m->onReset(*m);
            }
            continue;
        }
        if (s < 0x80) {
            r.code = s;
            for (uint16_t a = 0x6004; a < 0x7000; ++a) {
                const uint8_t c = bus->peek8(a);
                if (!c) break;
                r.text += (char)c;
            }
            break;
        }
    }
    delete m;
    return r;
}

std::string oneLine(std::string s) {
    for (auto& c : s) if (c == '\n') c = '|';
    while (!s.empty() && s.back() == '|') s.pop_back();
    return s;
}

}  // namespace

TEST_CASE(nes_test_roms_survey) {
    const std::string dir = romDir();
    if (!fs::exists(dir)) { std::printf("  (skipped: %s not found)\n", dir.c_str()); return; }
    if (!std::getenv("MMEMU_NES_SURVEY")) { std::printf("  (set MMEMU_NES_SURVEY=1 to run)\n"); return; }
    const char* subdirs[] = {"instr_test-v5/rom_singles", "instr_misc/rom_singles", "instr_timing/rom_singles",
                             "cpu_interrupts_v2/rom_singles", "ppu_vbl_nmi/rom_singles", "apu_test/rom_singles",
                             "mmc3_test_2/rom_singles", "ppu_open_bus", "ppu_read_buffer", "oam_read", "oam_stress",
                             "cpu_dummy_reads", "cpu_dummy_writes", "cpu_exec_space", "sprite_overflow_tests",
                             "apu_reset", "cpu_reset", "read_joy3", "dmc_dma_during_read4", "sprdma_and_dmc_dma",
                             "instr_test-v3/rom_singles"};
    for (const char* sd : subdirs) {
        const fs::path p = fs::path(dir) / sd;
        if (!fs::exists(p)) continue;
        std::vector<fs::path> roms;
        for (auto& e : fs::directory_iterator(p)) if (e.path().extension() == ".nes") roms.push_back(e.path());
        std::sort(roms.begin(), roms.end());
        const std::string filter = std::getenv("MMEMU_NES_SURVEY");
        for (auto& rom : roms) {
            if (filter != "1" && rom.string().find(filter) == std::string::npos) continue;
            Result r = runBlargg(rom.string());
            std::printf("  %-4s %s/%s: %s\n", r.code == 0 ? "ok" : (r.code < 0 ? "??" : "FAIL"), sd,
                        rom.filename().string().c_str(), oneLine(r.text).substr(0, 160).c_str());
        }
    }
}
