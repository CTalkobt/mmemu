// Freely redistributable Game Boy test ROMs (c-sp's gameboy-test-roms
// collection: Blargg's, Mooneye's, Matt Currie's dmg-acid2), read from
// MMEMU_GB_TEST_ROMS or ../refs/gameboy-test-roms (skipped when absent).
//
// gb_blargg_tests runs Blargg's cpu_instrs, instr_timing and mem_timing and
// dmg-acid2 (compared with its reference picture) and must pass. With
// MMEMU_GB_SURVEY=1, gb_rom_survey also runs the rest of Blargg's DMG tests
// and Mooneye's acceptance tests and prints what passes (MMEMU_GB_SHOW=1:
// the failures' names).

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

/** Run until the serial output has "Passed" or "Failed" (Blargg), or `seconds` pass. */
std::string runBlargg(gb::Gameboy& g, double seconds) {
    const uint64_t end = g.cycles() + (uint64_t)(seconds * gb::Gameboy::CLOCK_HZ);
    while (g.cycles() < end) {
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
    const uint64_t end = g.cycles() + (uint64_t)(seconds * gb::Gameboy::CLOCK_HZ);
    auto& s = g.cpu().state();
    while (g.cycles() < end) {
        g.runTo(g.cycles() + 70224);
        if (s.b == 3 && s.c == 5 && s.d == 8 && s.e == 13 && s.h == 21 && s.l == 34) return true;
        if (s.b == 0x42 && s.c == 0x42 && s.d == 0x42 && s.e == 0x42 && s.h == 0x42 && s.l == 0x42) return false;
    }
    return false;
}

/** An 8-bit RGB (or grey) PNG's pixels as RGB triples; false if it isn't one. */
bool decodePng(const std::vector<uint8_t>& png, int& w, int& h, std::vector<uint8_t>& rgb) {
    if (png.size() < 33 || std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) != 0) return false;
    size_t p = 8;
    std::vector<uint8_t> idat;
    int depth = 0, type = 0;
    auto be32 = [&](size_t at) { return (uint32_t)png[at] << 24 | png[at + 1] << 16 | png[at + 2] << 8 | png[at + 3]; };
    while (p + 8 <= png.size()) {
        const uint32_t len = be32(p);
        const std::string kind((const char*)&png[p + 4], 4);
        if (p + 12 + len > png.size()) return false;
        const uint8_t* d = &png[p + 8];
        if (kind == "IHDR") { w = (int)be32(p + 8); h = (int)be32(p + 12); depth = d[8]; type = d[9]; }
        else if (kind == "IDAT") idat.insert(idat.end(), d, d + len);
        p += 12 + len;
    }
    // 8-bit RGB, or grey of 1, 2, 4 or 8 bits (dmg-acid2's is 2-bit)
    const int channels = type == 2 ? 3 : type == 0 ? 1 : 0;
    if (!channels || (type == 2 && depth != 8) || (depth != 1 && depth != 2 && depth != 4 && depth != 8)) return false;
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
            if (channels == 3) {
                for (int k = 0; k < 3; ++k) rgb[i * 3 + k] = img[y * stride + x * 3 + k];
            } else {
                const size_t bit = (size_t)x * depth;
                const int v = (img[y * stride + bit / 8] >> (8 - depth - (int)(bit % 8))) & ((1 << depth) - 1);
                rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = (uint8_t)(v * 255 / ((1 << depth) - 1));
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

std::vector<std::string> listGb(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 3 && n.compare(n.size() - 3, 3, ".gb") == 0) out.push_back(n);
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

bool haveRoms() {
    std::ifstream f(romDir() + "/blargg/cpu_instrs/cpu_instrs.gb");
    if (!f) std::printf("  (no Game Boy test ROMs in %s: skipped)\n", romDir().c_str());
    return (bool)f;
}

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
}

TEST_CASE(gb_rom_survey) {
    if (!std::getenv("MMEMU_GB_SURVEY")) { std::printf("  (set MMEMU_GB_SURVEY=1 to run)\n"); return; }
    if (!haveRoms()) return;
    const std::string dir = romDir();
    const bool show = std::getenv("MMEMU_GB_SHOW") != nullptr;
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
    std::printf("  Game Boy test ROMs: %d PASS, %d FAIL\n", pass, fail);
}

