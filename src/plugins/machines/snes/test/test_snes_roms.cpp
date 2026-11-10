// krom's (Peter Lemon's) SNES CPU and SPC700 instruction tests
// (github.com/PeterLemon/SNES, CPUTest/), when present: MMEMU_SNES_TEST_ROMS,
// default ../refs/snes-krom. Each test prints a table of results with PASS
// or FAIL into BG1's tilemap (ASCII tile numbers); the survey runs each ROM
// for a few seconds and counts them. Skipped when the directory is missing;
// runs with MMEMU_SNES_SURVEY=1 (about 25 s). MMEMU_SNES_SHOTS=<dir> saves a
// PNG of each PPU demo.

#include "test_harness.h"
#include "plugins/machines/snes/main/machine_snes.h"
#include "plugins/machines/snes/main/snes_ppu.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/iaudio_output.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string romDir() {
    const char* v = std::getenv("MMEMU_SNES_TEST_ROMS");
    return v ? v : "../refs/snes-krom";
}

/** The text in VRAM (the tests' font has tile n = ASCII n): every 1 KB page with results in it. */
std::string screenText(MachineDescriptor* m, snes::Ppu* ppu) {
    (void)m;
    std::string s;
    for (int base = 0; base < 0x8000; base += 0x400) {
        std::string t;
        for (int i = 0; i < 0x400; ++i) {
            const int c = ppu->vram((uint16_t)(base + i)) & 0xFF;
            t += (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
            if ((i & 31) == 31) t += '\n';
        }
        for (const char* w : {"Test", "PASS", "Pass", "FAIL", "Fail"})
            if (t.find(w) != std::string::npos) { s += t; break; }
    }
    return s;
}

int count(const std::string& s, const char* w) {
    int n = 0;
    for (size_t p = s.find(w); p != std::string::npos; p = s.find(w, p + 1)) ++n;
    return n;
}

}  // namespace

TEST_CASE(snes_krom_cpu_tests) {
    const fs::path dir = fs::path(romDir()) / "CPUTest";
    if (!fs::exists(dir)) { std::printf("  (skipped: %s not found)\n", dir.string().c_str()); return; }
    if (!std::getenv("MMEMU_SNES_SURVEY")) { std::printf("  (set MMEMU_SNES_SURVEY=1 to run)\n"); return; }
    std::vector<fs::path> roms;
    // The 65816 and SPC700 tests, and the Super FX's (CHIP/GSU/GSUTest)
    for (const fs::path& d : {dir, fs::path(romDir()) / "CHIP" / "GSU" / "GSUTest"}) {
        if (!fs::exists(d)) continue;
        for (auto& e : fs::recursive_directory_iterator(d))
            if (e.path().extension() == ".sfc") roms.push_back(e.path());
    }
    std::sort(roms.begin(), roms.end());
    int totalPass = 0, totalFail = 0, silent = 0;
    for (auto& rom : roms) {
        std::ifstream f(rom, std::ios::binary);
        std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string err;
        MachineDescriptor* m = snes::create(false, &err);
        if (!snes::insertCartridge(m, img, rom.filename().string(), err)) { std::printf("  %s: %s\n", rom.string().c_str(), err.c_str()); delete m; continue; }
        std::vector<IOHandler*> hs;
        m->ioRegistry->enumerate(hs);
        snes::Ppu* ppu = nullptr;
        for (auto* h : hs) if ((ppu = dynamic_cast<snes::Ppu*>(h))) break;
        uint64_t t = 0;
        while (t < 21477272ull * 4) t += (uint64_t)m->schedulerStep(*m);    // 4 seconds
        const std::string text = screenText(m, ppu);
        const int pass = count(text, "PASS") + count(text, "Pass"), fail = count(text, "FAIL") + count(text, "Fail");
        totalPass += pass;
        totalFail += fail;
        if (!pass && !fail) ++silent;
        std::printf("  %-4s %-16s %3d pass %3d fail\n", fail ? "FAIL" : (pass ? "ok" : "??"), rom.stem().string().c_str(), pass, fail);
        if (fail && std::getenv("MMEMU_SNES_SHOW")) std::printf("%s\n", text.c_str());
        delete m;
    }
    std::printf("  %d PASS, %d FAIL, %d ROMs without results\n", totalPass, totalFail, silent);
    EXPECT_EQ(totalFail, 0);
}

// krom's SPC700 music demo: the 65816 uploads a song to the SPC700 through
// the IPL ROM, which plays it on the DSP; the output must not be silent
TEST_CASE(snes_krom_spc_music) {
    const fs::path rom = fs::path(romDir()) / "SPC700" / "Twinkle" / "Twinkle.sfc";
    if (!fs::exists(rom)) { std::printf("  (skipped: %s not found)\n", rom.string().c_str()); return; }
    std::ifstream f(rom, std::ios::binary);
    std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string err;
    MachineDescriptor* m = snes::create(false, &err);
    ASSERT(snes::insertCartridge(m, img, "Twinkle.sfc", err));
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    IAudioOutput* audio = nullptr;
    for (auto* h : hs) if ((audio = dynamic_cast<IAudioOutput*>(h))) break;
    ASSERT(audio != nullptr);
    std::vector<float> buf(4096);
    double energy = 0;
    int samples = 0;
    for (int sec = 0; sec < 3; ++sec) {
        uint64_t t = 0;
        while (t < 21477272ull) {
            t += (uint64_t)m->schedulerStep(*m);
            int n;
            while ((n = audio->pullSamples(buf.data(), (int)buf.size())) > 0) {
                for (int i = 0; i < n; ++i) energy += (double)buf[i] * buf[i];
                samples += n;
            }
        }
    }
    const double rms = samples ? std::sqrt(energy / samples) : 0;
    std::printf("  %d samples, RMS %.4f\n", samples, rms);
    EXPECT_TRUE(samples > 44100 * 2);
    EXPECT_TRUE(rms > 0.01);
    delete m;
}

// krom's PPU and Super FX demos, run for 3 seconds each and saved as PNGs into
// MMEMU_SNES_SHOTS (a directory) to look at; skipped without it. MMEMU_SNES_SHOTS_ONLY=<text>
// runs only those whose name contains it
TEST_CASE(snes_krom_ppu_shots) {
    const char* out = std::getenv("MMEMU_SNES_SHOTS");
    const fs::path dir = fs::path(romDir()) / "PPU";
    if (!out || !fs::exists(dir)) { std::printf("  (set MMEMU_SNES_SHOTS=<dir> to run)\n"); return; }
    std::vector<fs::path> roms;
    // The PPU demos, and the Super FX's drawing demos (CHIP/GSU, not the instruction tests)
    for (const fs::path& d : {dir, fs::path(romDir()) / "CHIP" / "GSU"}) {
        if (!fs::exists(d)) continue;
        for (auto& e : fs::recursive_directory_iterator(d))
            if (e.path().extension() == ".sfc" && e.path().string().find("GSUTest") == std::string::npos) roms.push_back(e.path());
    }
    std::sort(roms.begin(), roms.end());
    const char* only = std::getenv("MMEMU_SNES_SHOTS_ONLY");     // a part of the name, to run fewer
    for (auto& rom : roms) {
        if (only && rom.filename().string().find(only) == std::string::npos) continue;
        std::ifstream f(rom, std::ios::binary);
        std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string err;
        MachineDescriptor* m = snes::create(false, &err);
        if (!snes::insertCartridge(m, img, rom.filename().string(), err)) { delete m; continue; }
        std::vector<IOHandler*> hs;
        m->ioRegistry->enumerate(hs);
        snes::Ppu* ppu = nullptr;
        for (auto* h : hs) if ((ppu = dynamic_cast<snes::Ppu*>(h))) break;
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t t = 0;
        while (t < 21477272ull * 3) t += (uint64_t)m->schedulerStep(*m);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const auto d = ppu->getDimensions();
        const std::string png = (fs::path(out) / (rom.stem().string() + ".png")).string();
        ppu->exportPng(png);
        std::printf("  %-40s %dx%d %5.1fx real time\n", rom.stem().string().c_str(), d.width, d.height, 3.0 / secs);
        delete m;
    }
}
