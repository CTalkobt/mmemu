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
#include <filesystem>
#include <unistd.h>
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

// Type a .DO file into BASIC (CR line ends, blank lines dropped) and wait for the typing to end
void typeProgram(MachineDescriptor* m, const std::string& file) {
    auto* sys = m100::systemOf(m);
    std::ifstream f(m100::firmwareDir() + "/" + file, std::ios::binary);
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
}

IOHandler* handler(MachineDescriptor* m, const std::string& name) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs)
        if (name == h->name()) return h;
    return nullptr;
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
    typeProgram(m, "rxctst.do");
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

// RXCMGR, the REXCPM's manager, installed as its documentation says: rxcini.DO
// (REXCPM INITIALIZE v3) run from BASIC loads RXC_12.BR from a TPDD (here
// `serial tpdd:DIR` serving a host directory) into the card and sets up its
// directory; after a power cycle CALL 63012 starts RXCMGR and adds it to the
// menu. RXCMGR is then run from the menu. Needs m100.rom, rxcini.do and
// RXC_12.BR (all from REXCPMV21_b19.ZIP) in roms/m100/.
TEST_CASE(m100_rexcpm_rxcmgr) {
    if (!have("m100.rom") || !have("rxcini.do") || !have("RXC_12.BR")) return;
    const auto dir = std::filesystem::temp_directory_path() / ("mmsim_rxcmgr_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::filesystem::copy_file(m100::firmwareDir() + "/RXC_12.BR", dir / "RXC_12.BR");
    std::string err;
    MachineDescriptor* m = m100::create(Model::M100, &err);
    ASSERT(m != nullptr);
    auto* sys = m100::systemOf(m);
    ASSERT(m->expansionSlot("rex")->install("rexcpm", "", err));
    ASSERT(handler(m, "RS-232")->attachSerial("tpdd:" + dir.string()));
    sys->powerOn();
    ASSERT(waitLcd(m, "Bytes free", 10));
    runFor(m, 2);
    sys->keyboard.enqueueText("\\n");
    ASSERT(waitLcd(m, "Ok", 5));
    typeProgram(m, "rxcini.do");
    sys->keyboard.enqueueText("RUN\\n");
    ASSERT(waitLcd(m, "choice:<enter>", 20));
    sys->keyboard.enqueueText(" \\n");                // Load REXCPM code? Yes
    runFor(m, 1);
    sys->keyboard.enqueueText(" \\n");                // Init REXCPM dir? Yes
    ASSERT(waitLcd(m, "sure? (y/n)", 10));
    sys->keyboard.enqueueText("Y");
    ASSERT(waitLcd(m, "Name  :", 30));
    runFor(m, 1);
    sys->keyboard.enqueueText("RXC_12\\n");
    ASSERT(waitLcd(m, "Complete!!", 120));
    ASSERT(lcdText(m).find("24576") != std::string::npos);   // all of RXC_12.BR came over the TPDD
    runFor(m, 2);
    // Then, as its documentation says: power off and on (the card's default
    // state), and CALL 63012 from BASIC starts RXCMGR and puts it in the menu
    sys->powerSwitch();
    runFor(m, 1);
    sys->powerSwitch();
    ASSERT(waitLcd(m, "Select:", 5));
    sys->keyboard.enqueueText("\\n");                 // BASIC
    ASSERT(waitLcd(m, "Ok", 5));
    sys->keyboard.enqueueText("CALL 63012\\n");
    ASSERT(waitLcd(m, "RXCMGR", 10));
    ASSERT(waitLcd(m, "No ROM", 5));                  // the menu's REX status
    runFor(m, 2);
    sys->keyboard.enqueueText("RXCMGR\\n");
    ASSERT(waitLcd(m, "REXCPM2.1   Group<TAB>:RAM", 10));
    auto tap = [&](const char* k) {
        sys->keyboard.pressKeyByName(k, true);
        runFor(m, 0.1);
        sys->keyboard.pressKeyByName(k, false);
        runFor(m, 1);
    };
    tap("tab");                                       // the ROM group
    ASSERT(waitLcd(m, "Group<TAB>:ROM", 3));
    tap("tab");
    ASSERT(waitLcd(m, "Group<TAB>:RAM", 3));
    tap("f1");                                        // help
    ASSERT(waitLcd(m, "Laptop Main Menu Quick commands", 3));
    tap("escape");
    ASSERT(waitLcd(m, "Help Load", 3));
    tap("f8");                                        // exit to the menu
    ASSERT(waitLcd(m, "Select:", 3));
    // Ctrl-B (back up RAM) with no RAM image made yet
    sys->keyboard.pressKeyByName("ctrl", true);
    runFor(m, 0.1);
    tap("b");
    sys->keyboard.pressKeyByName("ctrl", false);
    EXPECT_TRUE(waitLcd(m, "No Active Block!", 3));
    delete m;
    std::filesystem::remove_all(dir);
}
