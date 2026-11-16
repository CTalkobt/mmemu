// The real ROMs, when they are there (#357). Micro Color BASIC is
// Microsoft's and Tandy's, MCX BASIC Darren Atkinson's: never in the
// repository. These run only with them in roms/mc10/ (or
// $MMEMU_MC10_FIRMWARE), else they say so and pass.
//  - mc10.rom: Micro Color BASIC 1.0 boots on `mc10` and `mc10-4k`, finds
//    its RAM, runs a program, CSAVEs and CLOADs it back, LLISTs it;
//  - mcx128bas.rom / alice128bas.rom: MCX BASIC 2.1 (the MCX-128's EPROM,
//    QWERTY and AZERTY) boots on `mc10` and `alice` and finds its RAM;
//  - alice32.rom and ef9345.rom (the EF9345's characters): the Alice 32
//    and 90 boot, run a program, CSAVE and CLOAD.

#include "test_harness.h"
#include "plugins/machines/mc10/main/machine_mc10.h"
#include "plugins/machines/mc10/main/mc10_system.h"
#include "plugins/6801/main/cpu6801.h"
#include "libcore/main/machine_desc.h"
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

namespace {

bool have(const std::string& file) {
    std::ifstream f(mc10::firmwareDir() + "/" + file);
    if (!f) std::printf("  (no %s in %s: skipped)\n", file.c_str(), mc10::firmwareDir().c_str());
    return (bool)f;
}

struct Real {
    std::unique_ptr<MachineDescriptor> desc;
    mc10::System* sys = nullptr;
    Real(mc10::Model model, mc10::Expansion exp) {
        std::string err;
        desc.reset(mc10::create(model, &err, exp));
        sys = mc10::systemOf(desc.get());
    }
    void runFor(double seconds) {
        const uint64_t end = sys->cycles() + (uint64_t)(seconds * sys->masterHz() / mc10::TICKS_PER_CYCLE);
        while (sys->cycles() < end) sys->step();
    }
    // The text screen: 32 x 16 at $4000, as the VDG's characters (the
    // Alice 32/90: the EF9345's 25 rows)
    std::string screen() const {
        std::string s;
        if (mc10::hasEf9345(sys->model())) {
            for (int y = 0; y < 25; ++y) s += sys->ef.rowText(y) + "\n";
            return s;
        }
        for (int i = 0; i < 512; ++i) {
            const uint8_t c = sys->ram((uint16_t)i) & 0x3F;
            s += (char)(c < 0x20 ? '@' + c : c);
            if (i % 32 == 31) s += '\n';
        }
        return s;
    }
    // `text` on the screen after `after`
    bool waitAfter(const std::string& after, const std::string& text, double seconds) {
        auto seen = [&]() {
            const std::string sc = screen();
            const size_t at = sc.find(after);
            return at != std::string::npos && sc.find(text, at + after.size()) != std::string::npos;
        };
        for (double t = 0; t < seconds && !seen(); t += 0.1) runFor(0.1);
        if (!seen()) std::printf("  waiting for \"%s\" after \"%s\"; the screen:\n%s\n", text.c_str(), after.c_str(), screen().c_str());
        return seen();
    }
    bool waitFor(const std::string& text, double seconds) {
        for (double t = 0; t < seconds; t += 0.1) {
            if (screen().find(text) != std::string::npos) return true;
            runFor(0.1);
        }
        const bool found = screen().find(text) != std::string::npos;
        if (!found) std::printf("  waiting for \"%s\"; the screen:\n%s\n", text.c_str(), screen().c_str());
        return found;
    }
    void type(const std::string& text) { sys->keyboard.enqueueText(text); }
};

}  // namespace

// Boots, finds the 16K pack (19526 free; 3142 without it), runs a program
TEST_CASE(mc10_rom_boot) {
    if (!have("mc10.rom")) return;
    Real m(mc10::Model::MC10, mc10::Expansion::RamPack16K);
    ASSERT(m.sys != nullptr);
    ASSERT(m.waitFor("MICROCOLOR BASIC 1.0", 5));
    ASSERT(m.waitFor("OK", 5));
    m.runFor(1);
    m.type("PRINT MEM\\n");
    EXPECT_TRUE(m.waitFor(" 19526", 5));
    m.type("10 FOR I=1 TO 3:PRINT I*7;:NEXT\\nRUN\\n");
    EXPECT_TRUE(m.waitFor(" 7  14  21", 10));
}

TEST_CASE(mc10_rom_boot_4k) {
    if (!have("mc10.rom")) return;
    Real m(mc10::Model::MC10, mc10::Expansion::None);
    ASSERT(m.sys != nullptr);
    ASSERT(m.waitFor("OK", 5));
    m.runFor(1);
    m.type("PRINT MEM\\n");
    EXPECT_TRUE(m.waitFor(" 3142", 5));
}

// CSAVE to the recorder, NEW, CLOAD it back from the .c10 and RUN
TEST_CASE(mc10_rom_csave_cload) {
    if (!have("mc10.rom")) return;
    const std::string tape = "/tmp/mmsim_mc10_" + std::to_string((long)getpid()) + ".c10";
    {
        Real m(mc10::Model::MC10, mc10::Expansion::RamPack16K);
        ASSERT(m.waitFor("OK", 5));
        m.runFor(1);
        m.type("10 PRINT \"SAVED\";6*7\\n");
        m.runFor(3);
        ASSERT(m.sys->tapeDeck.startTapeRecord());
        m.type("CSAVE \"T\"\\n");
        ASSERT(m.waitAfter("CSAVE", "OK", 30));
        m.sys->tapeDeck.stopTapeRecord();
        ASSERT(m.sys->tapeDeck.saveTapeRecording(tape));
    }
    Real m(mc10::Model::MC10, mc10::Expansion::RamPack16K);
    ASSERT(m.waitFor("OK", 5));
    m.runFor(1);
    ASSERT(m.sys->tapeDeck.mountTape(tape));
    m.type("CLOAD\\n");
    m.runFor(1);
    m.sys->tapeDeck.controlTape("play");
    EXPECT_TRUE(m.waitFor("T", 20));
    m.runFor(10);
    m.type("RUN\\n");
    EXPECT_TRUE(m.waitFor("SAVED 42", 30));
    std::remove(tape.c_str());
}

// LLIST: BASIC's printer routine bit-bangs 600 baud on P20 (P23 ready)
TEST_CASE(mc10_rom_llist) {
    if (!have("mc10.rom")) return;
    Real m(mc10::Model::MC10, mc10::Expansion::RamPack16K);
    ASSERT(m.waitFor("OK", 5));
    m.runFor(1);
    m.type("10 REM HELLO\\nLLIST\\n");
    m.runFor(5);
    const std::string got(m.sys->serial.sent.begin(), m.sys->serial.sent.end());
    if (got.find("10 REM HELLO") == std::string::npos) std::printf("  printed: \"%s\"\n%s", got.c_str(), m.screen().c_str());
    EXPECT_TRUE(got.find("10 REM HELLO") != std::string::npos);
}

// MCX BASIC 2.1's boot menu: Micro Color BASIC with 32K, MCX BASIC, MCX
// BASIC (large: the 128K banks)
TEST_CASE(mc10_rom_mcx128) {
    if (!have("mcx128bas.rom")) return;
    const struct { const char* key; const char* banner; const char* mem; } choices[] = {
        {"0", "MICROCOLOR BASIC 1.0", " 31558"}, {"1", "MCX BASIC 2.1", " 28703"}, {"2", "MCX BASIC 2.1", " 47454"},
    };
    for (const auto& c : choices) {
        Real m(mc10::Model::MC10, mc10::Expansion::Mcx128);
        ASSERT(m.sys != nullptr);
        ASSERT(m.waitFor("BOOT MENU", 5));
        m.type(c.key);
        ASSERT(m.waitFor(c.banner, 5));
        m.runFor(1);
        m.type("PRINT MEM\\n");
        EXPECT_TRUE(m.waitFor(c.mem, 5));
        m.type("10 PRINT 6*7\\nRUN\\n");
        EXPECT_TRUE(m.waitFor(" 42", 10));
    }
}

// The AZERTY build on the Alice (no Alice BASIC needed: the EPROM has all)
TEST_CASE(mc10_rom_alice_mcx128) {
    if (!have("alice128bas.rom")) return;
    Real m(mc10::Model::Alice, mc10::Expansion::Mcx128);
    ASSERT(m.sys != nullptr);
    ASSERT(m.waitFor("BOOT MENU", 5));
    m.type("1");
    ASSERT(m.waitFor("MCX BASIC 2.1 AZERTY", 5));
    m.runFor(1);
    m.type("PRINT MEM\\n");                   // typed by legend: the AZERTY positions
    EXPECT_TRUE(m.waitFor(" 28703", 5));
}

// The Alice 32 (8K) and 90 (32K): Micro Color BASIC 1.0 at $C000 (16 KB,
// the same ROM in both), the screen the EF9345's
TEST_CASE(mc10_rom_alice32) {
    if (!have("alice32.rom") || !have("ef9345.rom")) return;
    const struct { mc10::Model model; const char* mem; } models[] = {
        {mc10::Model::Alice32, " 7238"}, {mc10::Model::Alice90, " 31814"},
    };
    for (const auto& c : models) {
        Real m(c.model, mc10::Expansion::None);
        ASSERT(m.sys != nullptr);
        ASSERT(m.waitFor("MICROCOLOR BASIC 1.0", 5));
        ASSERT(m.waitFor("OK", 5));
        m.runFor(1);
        m.type("PRINT MEM\\n");                   // typed by legend: the AZERTY positions
        EXPECT_TRUE(m.waitFor(c.mem, 5));
        m.type("10 FOR I=1 TO 3:PRINT I*7;:NEXT\\nRUN\\n");
        EXPECT_TRUE(m.waitFor(" 7  14  21", 10));
        // The picture: BASIC's 32 x 16 in the middle of the 40 x 25, black
        // characters on green (row 4, the banner's, is the 6th cell row)
        EXPECT_EQ(m.sys->display.getDimensions().width, Ef9345::W40);
        EXPECT_EQ(m.sys->ef.pixel(2, 2), 0);
        bool green = false;
        for (int x = 40; x < 296 && !green; ++x) green = m.sys->ef.pixel(x, 55) == 2;
        EXPECT_TRUE(green);
    }
}

// CSAVE on an Alice 32 and CLOAD from the .k7 (the .c10 format)
TEST_CASE(mc10_rom_alice32_csave_cload) {
    if (!have("alice32.rom")) return;
    const std::string tape = "/tmp/mmsim_alice32_" + std::to_string((long)getpid()) + ".k7";
    {
        Real m(mc10::Model::Alice32, mc10::Expansion::None);
        ASSERT(m.waitFor("OK", 5));
        m.runFor(1);
        m.type("10 PRINT \"SAVED\";6*7\\n");
        m.runFor(3);
        ASSERT(m.sys->tapeDeck.startTapeRecord());
        m.type("CSAVE \"T\"\\n");
        ASSERT(m.waitAfter("CSAVE", "OK", 30));
        m.sys->tapeDeck.stopTapeRecord();
        ASSERT(m.sys->tapeDeck.saveTapeRecording(tape));
    }
    Real m(mc10::Model::Alice32, mc10::Expansion::None);
    ASSERT(m.waitFor("OK", 5));
    m.runFor(1);
    ASSERT(m.sys->tapeDeck.mountTape(tape));
    m.type("CLOAD\\n");
    m.runFor(1);
    m.sys->tapeDeck.controlTape("play");
    m.runFor(15);
    m.type("RUN\\n");
    EXPECT_TRUE(m.waitFor("SAVED 42", 30));
    std::remove(tape.c_str());
}
