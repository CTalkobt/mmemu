// Freely redistributable Game Boy test ROMs (c-sp's gameboy-test-roms
// collection: Blargg's, Mooneye's, Matt Currie's dmg-acid2 and cgb-acid2,
// SameSuite, AGE), read from MMEMU_GB_TEST_ROMS or ../refs/gameboy-test-roms
// (skipped when absent).
//
// gb_blargg_tests runs Blargg's cpu_instrs, instr_timing and mem_timing,
// dmg-acid2 and, on the Game Boy Color, cgb-acid2 (each compared with its
// reference picture) and must pass. With MMEMU_GB_SURVEY=1, gb_rom_survey
// also runs the rest of Blargg's DMG tests, Mooneye's acceptance tests and
// AGE's DMG-C ones on the DMG, then Blargg's, Mooneye's, SameSuite's and
// AGE's tests for the CGB (CPU CGB E) on the Game Boy Color, and prints
// what passes of each
// (MMEMU_GB_SHOW=1: the passes' names too).

#include "test_harness.h"
#include "plugins/machines/gameboy/main/gameboy.h"

#include <zlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string romDir() {
    const char* v = std::getenv("MMEMU_GB_TEST_ROMS");
    return v ? v : "../refs/gameboy-test-roms";
}

std::vector<uint8_t> readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

/** A Game Boy with `path` in it, powered on as a DMG (no boot ROM), or the SGB's (SameBoy's SGB boot ROM). */
bool start(gb::Gameboy& g, const std::string& path, bool sgb = false) {
    std::string err;
    auto rom = readAll(path);
    if (rom.empty() || !g.load(rom, err)) return false;
    if (sgb) g.setModel(gb::Gameboy::Model::Sgb);
    g.powerOn();
    return true;
}

/** A Game Boy Color with `path` in it, through SameBoy's CGB boot ROM (as Mesen) to the cartridge's $0100. */
bool startCgb(gb::Gameboy& g, const std::string& path, const std::vector<uint8_t>& bootRom = {}) {
    std::string err;
    auto rom = readAll(path);
    if (rom.empty() || !g.load(rom, err)) return false;
    g.setModel(gb::Gameboy::Model::Cgb);
    if (!bootRom.empty()) g.setBootRom(bootRom);
    g.powerOn();
    const uint64_t end = g.apuCycles() + 10ull * gb::Gameboy::CLOCK_HZ;
    while (!g.bootRomDisabled() && g.apuCycles() < end) g.cpu().exec();
    return g.bootRomDisabled();
}

/** Run until the serial output has "Passed" or "Failed" (Blargg), or `seconds` pass. */
std::string runBlargg(gb::Gameboy& g, double seconds) {
    const uint64_t end = g.apuCycles() + (uint64_t)(seconds * gb::Gameboy::CLOCK_HZ);
    while (g.apuCycles() < end) {
        g.runTo(g.cycles() + 70224);
        // Done once "Passed" or "Failed" and the end of that line are out
        const std::string& s = g.serialOutput();
        size_t at = s.find("Passed");
        if (at == std::string::npos) at = s.find("Failed");
        if (at != std::string::npos && s.find('\n', at) != std::string::npos) break;
    }
    return g.serialOutput();
}

/** Blargg's tests that report through $A000 too: $DE $B0 $61 at $A001 and the status at $A000 (0: passed). */
int blarggMemoryResult(gb::Gameboy& g) {
    if (g.peek(0xA001) != 0xDE || g.peek(0xA002) != 0xB0 || g.peek(0xA003) != 0x61) return -1;
    return g.peek(0xA000);
}

/**
 * Keeps a blargg test's text out of its own code. The text goes to cartridge RAM from $A004
 * with no end check, and past $BFFF that is work RAM, where the test runs: oam_bug's
 * 7-timing_effect prints 20 corrupted-OAM dumps, 10,525 bytes, and overwrites itself on any
 * DMG. Each write to $C000-$CFFF from an instruction that wrote text below $C000 is undone at
 * the CPU's next access, so the test reaches its CRC check (over all of the text) as built.
 */
struct TextSpill : gb::AccessHook {
    gb::Gameboy& g;
    uint16_t textPcs[4] = {};
    int numTextPcs = 0;
    uint16_t undoAddr = 0;
    int undoValue = -1;
    explicit TextSpill(gb::Gameboy& gb) : g(gb) { g.setAccessHook(this); }
    ~TextSpill() override { g.setAccessHook(nullptr); }
    void restore() {
        if (undoValue >= 0) g.poke(undoAddr, (uint8_t)undoValue);
        undoValue = -1;
    }
    bool isTextPc(uint16_t pc) const {
        for (int i = 0; i < numTextPcs; ++i) if (textPcs[i] == pc) return true;
        return false;
    }
    void cpuRead(uint16_t, uint8_t) override { restore(); }
    void cpuWrite(uint16_t a, uint8_t before, uint8_t) override {
        restore();
        const uint16_t pc = g.cpu().state().pc;
        if (a >= 0xA004 && a < 0xC000) {
            if (!isTextPc(pc) && numTextPcs < 4) textPcs[numTextPcs++] = pc;
        } else if (a >= 0xC000 && a < 0xD000 && isTextPc(pc)) {
            undoAddr = a;
            undoValue = before;
        }
    }
};

/** Mooneye's: LD B,B then the Fibonacci numbers in B C D E H L. */
bool mooneyePassed(gb::Gameboy& g, double seconds) {
    const uint64_t end = g.apuCycles() + (uint64_t)(seconds * gb::Gameboy::CLOCK_HZ);
    auto& s = g.cpu().state();
    while (g.apuCycles() < end) {
        g.runTo(g.cycles() + 70224);
        if (s.b == 3 && s.c == 5 && s.d == 8 && s.e == 13 && s.h == 21 && s.l == 34) return true;
        if (s.b == 0x42 && s.c == 0x42 && s.d == 0x42 && s.e == 0x42 && s.h == 0x42 && s.l == 0x42) return false;
    }
    return false;
}

/** An 8-bit RGB, grey or palette PNG's pixels as RGB triples; false if it isn't one. */
bool decodePng(const std::vector<uint8_t>& png, int& w, int& h, std::vector<uint8_t>& rgb) {
    if (png.size() < 33 || std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) != 0) return false;
    size_t p = 8;
    std::vector<uint8_t> idat, plte;
    int depth = 0, type = 0;
    w = h = 0;
    auto be32 = [&](size_t at) { return (uint32_t)png[at] << 24 | png[at + 1] << 16 | png[at + 2] << 8 | png[at + 3]; };
    while (p + 8 <= png.size()) {
        const uint32_t len = be32(p);
        const std::string kind((const char*)&png[p + 4], 4);
        if (p + 12 + len > png.size()) return false;
        const uint8_t* d = &png[p + 8];
        if (kind == "IHDR") { w = (int)be32(p + 8); h = (int)be32(p + 12); depth = d[8]; type = d[9]; }
        else if (kind == "IDAT") idat.insert(idat.end(), d, d + len);
        else if (kind == "PLTE") plte.assign(d, d + len);
        p += 12 + len;
    }
    // 8-bit RGB(A), or grey or palette indices of 1, 2, 4 or 8 bits (dmg-acid2's is 2-bit grey, cgb-acid2's 4-bit palette)
    const int channels = type == 2 ? 3 : type == 6 ? 4 : (type == 0 || type == 3) ? 1 : 0;
    if (!channels || (channels > 1 && depth != 8) || (depth != 1 && depth != 2 && depth != 4 && depth != 8)) return false;
    if (type == 3 && plte.empty()) return false;
    const int bpp = std::max(1, channels * depth / 8);
    const size_t stride = ((size_t)w * channels * depth + 7) / 8;
    std::vector<uint8_t> raw((stride + 1) * h);
    uLongf rawLen = (uLongf)raw.size();
    if (uncompress(raw.data(), &rawLen, idat.data(), (uLong)idat.size()) != Z_OK) return false;
    std::vector<uint8_t> img(stride * h), prev(stride, 0);
    for (int y = 0; y < h; ++y) {
        const uint8_t f = raw[y * (stride + 1)];
        const uint8_t* src = &raw[y * (stride + 1) + 1];
        uint8_t* out = &img[y * stride];
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= (size_t)bpp ? out[x - bpp] : 0, b = prev[x], c = x >= (size_t)bpp ? prev[x - bpp] : 0;
            int v = src[x];
            switch (f) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: {
                const int pp = a + b - c, pa = std::abs(pp - a), pb = std::abs(pp - b), pc = std::abs(pp - c);
                v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            }
            out[x] = (uint8_t)v;
        }
        std::memcpy(prev.data(), out, stride);
    }
    rgb.resize((size_t)w * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t)y * w + x;
            if (channels > 1) {
                for (int k = 0; k < 3; ++k) rgb[i * 3 + k] = img[y * stride + x * channels + k];
            } else {
                const size_t bit = (size_t)x * depth;
                const int v = (img[y * stride + bit / 8] >> (8 - depth - (int)(bit % 8))) & ((1 << depth) - 1);
                if (type == 3) {
                    for (int k = 0; k < 3; ++k) rgb[i * 3 + k] = (size_t)v * 3 + k < plte.size() ? plte[v * 3 + k] : 0;
                } else {
                    rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = (uint8_t)(v * 255 / ((1 << depth) - 1));
                }
            }
        }
    return true;
}

/** The screen against a reference picture in the common DMG palette (#FFFFFF #AAAAAA #555555 #000000); the pixels that differ. */
int compareScreen(gb::Gameboy& g, const std::string& pngPath) {
    int w, h;
    std::vector<uint8_t> rgb;
    if (!decodePng(readAll(pngPath), w, h, rgb) || w != 160 || h != 144) return -1;
    static const uint8_t shade[4] = {0xFF, 0xAA, 0x55, 0x00};
    int diff = 0;
    for (int i = 0; i < 160 * 144; ++i)
        if (std::abs(shade[g.screen()[i] & 3] - rgb[i * 3]) > 8) ++diff;      // oam_bug-dmg.png has a row of $FE/$01
    return diff;
}

/**
 * The CGB's screen against a reference picture, each 5-bit channel X as (X << 3) | (X >> 2)
 * (c-sp's convention); the pixels that differ.
 */
int compareColorScreen(gb::Gameboy& g, const std::string& pngPath, int tolerance = 0) {
    int w, h;
    std::vector<uint8_t> rgb;
    if (!decodePng(readAll(pngPath), w, h, rgb) || w != 160 || h != 144) return -1;
    auto ch = [](uint16_t v, int shift) { const int x = (v >> shift) & 0x1F; return (x << 3) | (x >> 2); };
    auto near = [tolerance](int a, int b) { return std::abs(a - b) <= tolerance; };
    int diff = 0;
    for (int i = 0; i < 160 * 144; ++i) {
        const uint16_t c = g.colorScreen()[i];
        if (!near(ch(c, 0), rgb[i * 3]) || !near(ch(c, 5), rgb[i * 3 + 1]) || !near(ch(c, 10), rgb[i * 3 + 2])) ++diff;
    }
    return diff;
}

std::vector<std::string> listGb(const std::string& dir, bool gbc = false) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if ((n.size() > 3 && n.compare(n.size() - 3, 3, ".gb") == 0) ||
                (gbc && n.size() > 4 && n.compare(n.size() - 4, 4, ".gbc") == 0))
                out.push_back(n);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

/** Mooneye's model suffix (dmg0, dmgABC, mgb, sgb, cgb.., or letters: G = DMG and MGB, S, C, A): one for the DMG-ABC? */
bool forDmgAbc(const std::string& name) {
    const auto dash = name.rfind('-');
    if (dash == std::string::npos) return true;
    const std::string models = name.substr(dash + 1, name.size() - dash - 4);
    if (models.find("dmgABC") != std::string::npos) return true;
    for (char c : models) if (c < 'A' || c > 'Z') return false;
    return models.find('G') != std::string::npos;
}

/** And for the SGB (sgb, or the letter S)? */
bool forSgb(const std::string& name) {
    const auto dash = name.rfind('-');
    if (dash == std::string::npos) return false;
    const std::string models = name.substr(dash + 1, name.size() - dash - 4);
    if (models == "sgb") return true;
    for (char c : models) if (c < 'A' || c > 'Z') return false;
    return models.find('S') != std::string::npos;
}

/**
 * For the CGB we are (CPU CGB E)? By the file name's model suffix: none (all models); Mooneye's
 * letters (C); or lower-case groups (dmgABC, mgb, sgb, cgb0, cgbABCDE, ncmBCE: CGB E in DMG mode)
 * with a bare "cgb", or "cgb"/"ncm" and an E.
 */
bool forCgb(const std::string& name) {
    const auto dot = name.rfind('.');
    const auto dash = name.rfind('-');
    if (dash == std::string::npos) return true;
    const std::string models = name.substr(dash + 1, (dot == std::string::npos ? name.size() : dot) - dash - 1);
    bool upper = !models.empty();
    for (char c : models) if (c < 'A' || c > 'Z') upper = false;
    if (upper) return models.find('C') != std::string::npos;
    for (size_t i = 0; i < models.size();) {
        size_t j = i;
        while (j < models.size() && models[j] >= 'a' && models[j] <= 'z') ++j;
        const std::string group = models.substr(i, j - i);
        size_t k = j;
        while (k < models.size() && !(models[k] >= 'a' && models[k] <= 'z')) ++k;
        const std::string revs = models.substr(j, k - j);
        if ((group == "cgb" && (revs.empty() || revs.find('E') != std::string::npos)) ||
            (group == "ncm" && revs.find('E') != std::string::npos))
            return true;
        if (k == i) break;
        i = k;
    }
    return false;
}

bool haveRoms() {
    std::ifstream f(romDir() + "/blargg/cpu_instrs/cpu_instrs.gb");
    if (!f) std::printf("  (no Game Boy test ROMs in %s: skipped)\n", romDir().c_str());
    return (bool)f;
}

void surveyDmg(const std::string& dir, bool show);
void surveyCgb(const std::string& dir, bool show);

}  // namespace

TEST_CASE(gb_blargg_tests) {
    if (!haveRoms()) return;
    const std::string dir = romDir();
    {
        gb::Gameboy g;
        ASSERT(start(g, dir + "/blargg/cpu_instrs/cpu_instrs.gb"));
        const std::string out = runBlargg(g, 60);
        std::printf("  cpu_instrs: %s\n", out.find("Passed all tests") != std::string::npos ? "passed" : out.c_str());
        EXPECT_TRUE(out.find("Passed all tests") != std::string::npos);
    }
    {
        gb::Gameboy g;
        ASSERT(start(g, dir + "/blargg/instr_timing/instr_timing.gb"));
        const std::string out = runBlargg(g, 5);
        std::printf("  instr_timing: %s\n", out.find("Passed") != std::string::npos ? "passed" : out.c_str());
        EXPECT_TRUE(out.find("Passed") != std::string::npos);
    }
    {
        gb::Gameboy g;
        ASSERT(start(g, dir + "/blargg/mem_timing/mem_timing.gb"));
        const std::string out = runBlargg(g, 10);
        std::printf("  mem_timing: %s\n", out.find("Passed") != std::string::npos ? "passed" : out.c_str());
        EXPECT_TRUE(out.find("Passed") != std::string::npos);
    }
    {
        gb::Gameboy g;
        ASSERT(start(g, dir + "/dmg-acid2/dmg-acid2.gb"));
        for (int i = 0; i < 60; ++i) g.runFrame();
        const int diff = compareScreen(g, dir + "/dmg-acid2/dmg-acid2-dmg.png");
        std::printf("  dmg-acid2: %d pixels differ\n", diff);
        EXPECT_EQ(diff, 0);
    }
    {
        // The Game Boy Color: cgb-acid2 through SameBoy's boot ROM
        gb::Gameboy g;
        ASSERT(startCgb(g, dir + "/cgb-acid2/cgb-acid2.gbc"));
        for (int i = 0; i < 60; ++i) g.runFrame();
        const int diff = compareColorScreen(g, dir + "/cgb-acid2/cgb-acid2.png");
        std::printf("  cgb-acid2: %d pixels differ\n", diff);
        EXPECT_EQ(diff, 0);
    }
}

TEST_CASE(gb_rom_survey) {
    if (!std::getenv("MMEMU_GB_SURVEY")) { std::printf("  (set MMEMU_GB_SURVEY=1 to run)\n"); return; }
    if (!haveRoms()) return;
    const std::string dir = romDir();
    const bool show = std::getenv("MMEMU_GB_SHOW") != nullptr;
    const std::string only = std::getenv("MMEMU_GB_SURVEY");     // "dmg" or "cgb": that half only
    if (only != "cgb") surveyDmg(dir, show);
    if (only != "dmg") surveyCgb(dir, show);
}

namespace {

/** Blargg's DMG tests, Mooneye's acceptance tests (the SGB's on the SGB) and AGE's DMG-C tests on the DMG. */
void surveyDmg(const std::string& dir, bool show) {
    int pass = 0, fail = 0;
    auto report = [&](const std::string& name, bool ok) {
        ok ? ++pass : ++fail;
        if (show || !ok) std::printf("    %s %s\n", ok ? "PASS" : "FAIL", name.c_str());
    };
    // Blargg's single tests
    for (const char* sub : {"cpu_instrs/individual", "mem_timing/individual", "mem_timing-2/rom_singles", "dmg_sound/rom_singles", "oam_bug/rom_singles"}) {
        for (const auto& n : listGb(dir + "/blargg/" + sub)) {
            gb::Gameboy g;
            if (!start(g, dir + "/blargg/" + sub + "/" + n)) { report(std::string(sub) + "/" + n, false); continue; }
            std::unique_ptr<TextSpill> spill;
            if (n == "7-timing_effect.gb") spill.reset(new TextSpill(g));
            const std::string out = runBlargg(g, 40);
            const int mem = blarggMemoryResult(g);
            report(std::string(sub) + "/" + n, out.find("Passed") != std::string::npos || mem == 0);
        }
    }
    for (const char* t : {"halt_bug.gb"}) {
        gb::Gameboy g;
        start(g, dir + "/blargg/" + t);
        runBlargg(g, 5);
        report(t, compareScreen(g, dir + "/blargg/halt_bug-dmg-cgb.png") == 0);
    }
    {
        // oam_bug's eight tests in one, judged by its screen as c-sp's collection does (the
        // one checked on a DMG; rom_singles/7-timing_effect needs TextSpill, above)
        gb::Gameboy g;
        start(g, dir + "/blargg/oam_bug/oam_bug.gb");
        runBlargg(g, 25);
        report("oam_bug.gb", compareScreen(g, dir + "/blargg/oam_bug/oam_bug-dmg.png") == 0);
    }
    // Mooneye's acceptance tests for the DMG (the boot_ ones check what the boot ROM leaves: the post-boot state here)
    for (const char* sub : {"", "/bits", "/instr", "/interrupts", "/oam_dma", "/ppu", "/serial", "/timer"}) {
        const std::string d = dir + "/mooneye-test-suite/acceptance" + sub;
        for (const auto& n : listGb(d)) {
            const bool sgb = !forDmgAbc(n);
            if (sgb && !forSgb(n)) continue;
            gb::Gameboy g;
            report("mooneye" + std::string(sub) + "/" + n + (sgb ? " (SGB)" : ""), start(g, d + "/" + n, sgb) && mooneyePassed(g, 20));
        }
    }
    // AGE's register-checking tests verified on a DMG-C (dmgC in the name), less oam-write-dmgC, which
    // AGE's own emulator leaves out too (build/test-blacklist.txt): its starred row lands on the mode
    // edges and on the hardware changes from line to line, with when the LCD was last turned off
    for (const char* sub : {"halt", "lcd-align-ly", "ly", "oam", "speed-switch", "stat-interrupt", "stat-mode",
                            "stat-mode-sprites", "stat-mode-window", "vram"}) {
        const std::string d = dir + "/age-test-roms/" + sub;
        for (const auto& n : listGb(d)) {
            if (n.find("dmgC") == std::string::npos || n == "oam-write-dmgC.gb") continue;
            gb::Gameboy g;
            report("age-test-roms/" + std::string(sub) + "/" + n, start(g, d + "/" + n) && mooneyePassed(g, 20));
        }
    }
    std::printf("  Game Boy test ROMs: %d PASS, %d FAIL\n", pass, fail);
}

/** The Game Boy Color's (CPU CGB E), through SameBoy's boot ROM as Mesen: Blargg's, the acid tests, Mooneye's, SameSuite's, AGE's. */
void surveyCgb(const std::string& dir, bool show) {
    int cgbPass = 0, cgbFail = 0;
    auto cgbReport = [&](const std::string& name, bool ok) {
        ok ? ++cgbPass : ++cgbFail;
        if (show || !ok) std::printf("    CGB %s %s\n", ok ? "PASS" : "FAIL", name.c_str());
    };
    // Blargg's whole suites, judged by their screens (c-sp's -cgb and -dmg-cgb pictures) after the time
    // c-sp's howto gives them; oam_bug's CGB picture is its result on a CGB (which has no OAM bug),
    // made with X<<3 for each channel (white $F8F8F8, not the howto's $FFFFFF): within 7 for that one
    struct Screen { const char* rom; const char* png; double seconds; int tolerance = 0; };
    for (const Screen& t : {Screen{"cpu_instrs/cpu_instrs.gb", "cpu_instrs/cpu_instrs-dmg-cgb.png", 31},
                            Screen{"instr_timing/instr_timing.gb", "instr_timing/instr_timing-dmg-cgb.png", 1},
                            Screen{"mem_timing/mem_timing.gb", "mem_timing/mem_timing-dmg-cgb.png", 3},
                            Screen{"mem_timing-2/mem_timing.gb", "mem_timing-2/mem_timing-dmg-cgb.png", 4},
                            Screen{"halt_bug.gb", "halt_bug-dmg-cgb.png", 2},
                            Screen{"interrupt_time/interrupt_time.gb", "interrupt_time/interrupt_time-cgb.png", 2},
                            Screen{"cgb_sound/cgb_sound.gb", "cgb_sound/cgb_sound-cgb.png", 37},
                            Screen{"oam_bug/oam_bug.gb", "oam_bug/oam_bug-cgb.png", 21, 7}}) {
        gb::Gameboy g;
        int diff = -1;
        if (startCgb(g, dir + "/blargg/" + t.rom)) {
            const uint64_t end = g.apuCycles() + (uint64_t)(t.seconds * gb::Gameboy::CLOCK_HZ);
            while (g.apuCycles() < end) g.runFrame();
            diff = compareColorScreen(g, dir + "/blargg/" + t.png, t.tolerance);
        }
        cgbReport(std::string("blargg/") + t.rom + (diff ? " (" + std::to_string(diff) + " pixels differ)" : ""), diff == 0);
    }
    // Blargg's single tests: the serial port's or $A000's result
    for (const char* sub : {"cpu_instrs/individual", "mem_timing/individual", "mem_timing-2/rom_singles", "cgb_sound/rom_singles"}) {
        for (const auto& n : listGb(dir + "/blargg/" + sub)) {
            gb::Gameboy g;
            bool ok = startCgb(g, dir + "/blargg/" + sub + "/" + n);
            if (ok) {
                const std::string out = runBlargg(g, 40);
                ok = out.find("Passed") != std::string::npos || blarggMemoryResult(g) == 0;
            }
            cgbReport(std::string("blargg/") + sub + "/" + n, ok);
        }
    }
    // The acid tests and Mooneye's sprite priority (screens)
    for (const Screen& t : {Screen{"cgb-acid2/cgb-acid2.gbc", "cgb-acid2/cgb-acid2.png", 1},
                            Screen{"cgb-acid-hell/cgb-acid-hell.gbc", "cgb-acid-hell/cgb-acid-hell.png", 1},
                            Screen{"mooneye-test-suite/manual-only/sprite_priority.gb", "mooneye-test-suite/manual-only/sprite_priority-cgb.png", 1}}) {
        gb::Gameboy g;
        int diff = -1;
        if (startCgb(g, dir + "/" + t.rom)) {
            const uint64_t end = g.apuCycles() + (uint64_t)(t.seconds * gb::Gameboy::CLOCK_HZ);
            while (g.apuCycles() < end) g.runFrame();
            diff = compareColorScreen(g, dir + "/" + t.png);
        }
        cgbReport(t.rom + (diff ? " (" + std::to_string(diff) + " pixels differ)" : std::string()), diff == 0);
    }
    // Mooneye's (acceptance, and misc's CGB ones), SameSuite's and AGE's register-checking tests for
    // CGB E (by their names' model suffixes; AGE's without one compare screens, not run here)
    struct Dir { std::string path; bool needSuffix; };
    std::vector<Dir> dirs;
    for (const char* sub : {"", "/bits", "/instr", "/interrupts", "/oam_dma", "/ppu", "/serial", "/timer"})
        dirs.push_back({"mooneye-test-suite/acceptance" + std::string(sub), false});
    for (const char* sub : {"", "/bits", "/ppu"}) dirs.push_back({"mooneye-test-suite/misc" + std::string(sub), true});
    for (const char* sub : {"dma", "interrupt", "ppu", "apu", "apu/channel_1", "apu/channel_2", "apu/channel_3", "apu/channel_4"})
        dirs.push_back({"same-suite/" + std::string(sub), false});
    for (const char* sub : {"halt", "lcd-align-ly", "ly", "oam", "speed-switch", "speed-switch/caution", "stat-interrupt",
                            "stat-mode", "stat-mode-sprites", "stat-mode-window", "vram"})
        dirs.push_back({"age-test-roms/" + std::string(sub), true});
    // Mooneye's checks of what Nintendo's CGB boot ROM leaves (DIV's phase after its logo, its chime still
    // sounding in PCM12, the palette indexes): run with your own Nintendo boot ROM, MMEMU_GB_FIRMWARE or
    // roms/gameboy/cgb_boot.bin (2304 bytes), else skipped (SameBoy's built-in one ends sooner and quieter)
    auto nintendoBootOnly = [](const std::string& n) {
        return n == "boot_div-cgbABCDE.gb" || n == "boot_hwio-C.gb" || n == "unused_hwio-C.gb";
    };
    const char* fw = std::getenv("MMEMU_GB_FIRMWARE");
    std::vector<uint8_t> nintendoBoot = readAll((fw ? std::string(fw) : std::string("roms/gameboy")) + "/cgb_boot.bin");
    if (nintendoBoot.size() != 2304) nintendoBoot.clear();
    int cgbSkip = 0;
    for (const auto& d : dirs) {
        for (const auto& n : listGb(dir + "/" + d.path, true)) {
            if (!forCgb(n) || (d.needSuffix && n.find('-') == std::string::npos)) continue;
            const bool nintendo = nintendoBootOnly(n);
            if (nintendo && nintendoBoot.empty()) {
                ++cgbSkip;
                if (show) std::printf("    CGB SKIP %s/%s (needs Nintendo's CGB boot ROM)\n", d.path.c_str(), n.c_str());
                continue;
            }
            gb::Gameboy g;
            cgbReport(d.path + "/" + n + (nintendo ? " (Nintendo's boot ROM)" : ""),
                      startCgb(g, dir + "/" + d.path + "/" + n, nintendo ? nintendoBoot : std::vector<uint8_t>{}) && mooneyePassed(g, 20));
        }
    }
    std::printf("  Game Boy Color test ROMs: %d PASS, %d FAIL", cgbPass, cgbFail);
    if (cgbSkip) std::printf(", %d skipped (they need Nintendo's CGB boot ROM in roms/gameboy/cgb_boot.bin)", cgbSkip);
    std::printf("\n");
}


}  // namespace

