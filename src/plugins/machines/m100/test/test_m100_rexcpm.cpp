// The REXCPM on a Model 100 with the real ROM and Steve Adolph's own test,
// REXCPM TEST v2 (rxctst.DO from bitchin100.com's REXCPMV21_b19.ZIP), when
// m100.rom and rxctst.do are in roms/m100/ (else it says so and passes).
// The test pokes its machine code from BASIC and then checks the RAM
// (every block), the window registers and write protection.

#include "test_harness.h"
#include "plugins/machines/m100/main/machine_m100.h"
#include "plugins/machines/m100/main/m100_system.h"
#include "plugins/machines/m100/test/m100_test_rom.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace m100test;
using m100::Model;

namespace {

bool have(const std::string& file) {
    std::ifstream f(m100::firmwareDir() + "/" + file);
    if (!f) std::printf("  (no %s in %s: skipped)\n", file.c_str(), m100::firmwareDir().c_str());
    return (bool)f;
}

std::string lcdText(MachineDescriptor* m) {
    std::string s;
    for (int i = 0; i < 320; ++i) {
        const uint8_t c = m->buses[0].bus->peek8((uint32_t)(0xFE00 + i));
        s += (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
        if (i % 40 == 39) s += '\n';
    }
    return s;
}

bool waitLcd(MachineDescriptor* m, const std::string& text, double seconds) {
    for (double t = 0; t < seconds; t += 0.5) {
        if (lcdText(m).find(text) != std::string::npos) return true;
        runFor(m, 0.5);
    }
    if (lcdText(m).find(text) != std::string::npos) return true;
    std::printf("  waiting for \"%s\":\n%s", text.c_str(), lcdText(m).c_str());
    return false;
}

}  // namespace

TEST_CASE(m100_rexcpm_real_test) {
    if (!have("m100.rom") || !have("rxctst.do")) return;
    std::string err;
    MachineDescriptor* m = m100::create(Model::M100, &err);
    ASSERT(m != nullptr);
    auto* sys = m100::systemOf(m);
    ASSERT(m->expansionSlot("rex")->install("rexcpm", "", err));
    sys->powerOn();                                    // RAM is the REXCPM's: a cold start
    ASSERT(waitLcd(m, "Bytes free", 10));
    runFor(m, 2);
    sys->keyboard.enqueueText("\\n");                  // BASIC
    ASSERT(waitLcd(m, "Ok", 5));
    // Type the program (CR line ends, blank lines dropped)
    std::ifstream f(m100::firmwareDir() + "/rxctst.do", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string prog, line;
    for (char c : ss.str()) {
        if (c == '\r' || c == '\n') { if (!line.empty()) prog += line + "\\n"; line.clear(); }
        else if (c != 0x1A) line += c;
    }
    if (!line.empty()) prog += line + "\\n";
    sys->keyboard.enqueueText(prog);
    for (int i = 0; i < 4000 && sys->keyboard.typing(); ++i) runFor(m, 0.25);
    runFor(m, 1);
    sys->keyboard.enqueueText("RUN\\n");
    ASSERT(waitLcd(m, "choice:<enter>", 20));
    sys->keyboard.enqueueText("\\n");                  // Skip x16: Off
    runFor(m, 1);
    sys->keyboard.enqueueText("\\n");                  // End Addr: C000
    ASSERT(waitLcd(m, "sure? (y/n)", 10));
    sys->keyboard.enqueueText("Y");
    const bool passed = waitLcd(m, "Passed", 600);
    std::printf("%s", lcdText(m).c_str());
    EXPECT_TRUE(passed);
    delete m;
}
