// The NES test ROMs (github.com/christopherpow/nes-test-roms: blargg's
// suites and others), when present: MMEMU_NES_TEST_ROMS, default
// ../refs/nes-test-roms. Skipped when the directory is missing.
//
// blargg's protocol: $6001-$6003 hold DE B0 61 once the test is running;
// $6000 is $80 while running, $81 when it wants the console reset, else the
// result (0 = passed); a text report is at $6004. Tests without it (the
// older ones, read_joy3, dmc_dma_during_read4, sprite_overflow_tests) are
// read from the screen: the first nametable as ASCII, for "Passed",
// "Failed" or "Error".

#include "test_harness.h"
#include "plugins/machines/nes/main/machine_nes.h"
#include "plugins/machines/nes/main/nes_ppu.h"
#include "libdevices/main/io_registry.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/ibus.h"
#include <cctype>
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

/** The first nametable as text (the tests' fonts put ASCII at tile n). */
std::string screenText(MachineDescriptor* m) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    nes::Ppu* ppu = nullptr;
    for (auto* h : hs) if ((ppu = dynamic_cast<nes::Ppu*>(h))) break;
    std::string t;
    if (!ppu) return t;
    for (int r = 0; r < 30; ++r) {
        std::string line;
        for (int c = 0; c < 32; ++c) {
            const uint8_t ch = ppu->peekVram((uint16_t)(0x2000 + r * 32 + c));
            line += (ch >= 0x20 && ch < 0x7F) ? (char)ch : ' ';
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (!line.empty()) t += line + "\n";
    }
    return t;
}

// Tests that print only a CRC of their output: the values the source
// gives for a NES (several, by the CPU/PPU alignment at power-on)
const struct { const char* rom; const char* crcs[4]; } CRC_ONLY[] = {
    {"dma_2007_read.nes", {"159A7A8F", "5E3DF9C4"}},
    {"double_2007_read.nes", {"85CFD627", "F018C287", "440EF923", "E52F41A5"}},
};

int screenVerdict(const std::string& rom, const std::string& t) {
    for (const auto& c : CRC_ONLY) {
        if (rom.find(c.rom) == std::string::npos) continue;
        bool crc = false;
        for (const char* v : c.crcs) {
            if (!v) continue;
            if (t.find(v) != std::string::npos) return 0;
        }
        // Done (a CRC printed) but not one of them?
        for (size_t i = 0; i + 8 <= t.size(); ++i)
            if (std::isxdigit((unsigned char)t[i]) && t.find_first_not_of("0123456789ABCDEF", i) >= i + 8) crc = true;
        return crc ? 1 : -1;
    }
    // blargg_nes_cpu_test5: done, with a line per failing opcode ("6B ARR #n")
    if (t.find("All tests complete") != std::string::npos) {
        size_t p = 0;
        while (p < t.size()) {
            const size_t e = t.find('\n', p);
            const std::string line = t.substr(p, e == std::string::npos ? std::string::npos : e - p);
            const size_t b = line.find_first_not_of(' ');
            if (b != std::string::npos && line.size() >= b + 3 && std::isxdigit((unsigned char)line[b]) &&
                std::isxdigit((unsigned char)line[b + 1]) && line[b + 2] == ' ')
                return 1;
            if (e == std::string::npos) break;
            p = e + 1;
        }
        return 0;
    }
    // blargg's 2005 tests print only a result code: $01 passed
    {
        const size_t b = t.find_first_not_of(" \n"), e = t.find_last_not_of(" \n");
        if (b != std::string::npos && e == b + 2 && t[b] == '$') return t.compare(b, 3, "$01") == 0 ? 0 : 1;
    }
    if (t.find("Passed") != std::string::npos || t.find("PASSED") != std::string::npos) return 0;
    if (t.find("Failed") != std::string::npos || t.find("FAILED") != std::string::npos ||
        t.find("Error ") != std::string::npos || t.find("ERROR") != std::string::npos) return 1;
    return -1;
}

// Other MMC3 revisions than the one emulated (MMC3B): failing is expected
bool otherRevision(const std::string& rom) {
    return rom.find("MMC3_alt") != std::string::npos || rom.find("6-MMC6") != std::string::npos ||
           rom.find("MMC3_rev_A") != std::string::npos;
}

// No verdict: they need buttons pressed, or count DMC read errors (which a
// NES has too)
bool informational(const std::string& rom) {
    return rom.find("test_buttons") != std::string::npos || rom.find("count_errors") != std::string::npos;
}

Result runBlargg(const std::string& path, uint64_t maxCycles = 60ull * 29781 * 60) {
    Result r;
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string err;
    MachineDescriptor* m = nes::create(false, &err);
    if (!nes::insertCartridge(m, img, path, err)) { r.text = err; delete m; return r; }
    IBus* bus = m->buses[0].bus;
    auto started = [&]() { return bus->peek8(0x6001) == 0xDE && bus->peek8(0x6002) == 0xB0 && bus->peek8(0x6003) == 0x61; };
    uint64_t t = 0, resetAt = 0, lastReset = 0;
    uint64_t nextLook = 29781 * 30;
    while (t < maxCycles) {
        t += (uint64_t)m->schedulerStep(*m);
        if (t >= nextLook && !started()) {
            // Not blargg's $6000 protocol: look at the screen every half second
            nextLook = t + 29781 * 30;
            const std::string text = screenText(m);
            const int v = screenVerdict(path, text);
            if (v >= 0) { r.code = v; r.text = text; break; }
            r.text = text;
            continue;
        }
        if ((t & 0x3FF) > 8) continue;
        if (!started()) continue;
        const uint8_t s = bus->peek8(0x6000);
        if (s == 0x81) {
            // Press reset ~100 ms after asked; $6000 stays $81 while the
            // test's reset code runs (4017_timing measures for 0.25 s), so
            // not again within a second
            if (t < lastReset + 1789773) continue;
            if (!resetAt) resetAt = t + 180000;
            else if (t >= resetAt) {
                resetAt = 0;
                lastReset = t;
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
                             "instr_test-v3/rom_singles", "cpu_reset", "nes_instr_test/rom_singles",
                             "blargg_nes_cpu_test5", "cpu_timing_test6", "branch_timing_tests",
                             "blargg_apu_2005.07.30", "blargg_ppu_tests_2005.09.15b", "vbl_nmi_timing",
                             "sprite_hit_tests_2005.10.05", "mmc3_test", "mmc3_irq_tests"};
    for (const char* sd : subdirs) {
        const fs::path p = fs::path(dir) / sd;
        if (!fs::exists(p)) continue;
        std::vector<fs::path> roms;
        for (auto& e : fs::directory_iterator(p)) if (e.path().extension() == ".nes") roms.push_back(e.path());
        std::sort(roms.begin(), roms.end());
        const std::string filter = std::getenv("MMEMU_NES_SURVEY");
        for (auto& rom : roms) {
            if (filter != "1" && rom.string().find(filter) == std::string::npos) continue;
            if (informational(rom.string())) {
                std::printf("  --   %s/%s: (informational)\n", sd, rom.filename().string().c_str());
                continue;
            }
            // blargg_nes_cpu_test5 checksums every instruction: minutes
            const uint64_t limit = std::string(sd).find("cpu_test5") != std::string::npos ? 300ull * 29781 * 60
                                                                                           : 60ull * 29781 * 60;
            Result r = runBlargg(rom.string(), limit);
            if (r.code != 0 && std::getenv("MMEMU_NES_SHOW")) std::printf("%s\n", r.text.c_str());
            const char* verdict = r.code == 0 ? "ok" : (r.code < 0 ? "??" : (otherRevision(rom.string()) ? "rev" : "FAIL"));
            std::printf("  %-4s %s/%s: %s\n", verdict, sd,
                        rom.filename().string().c_str(), oneLine(r.text).substr(0, 160).c_str());
        }
    }
}
