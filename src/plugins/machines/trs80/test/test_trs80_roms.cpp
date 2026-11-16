// The real ROMs and DOSes, when they are there (#354). The ROMs are Tandy's
// and Microsoft's and never in the repository: these run only with them in
// roms/trs80/ (or $MMEMU_TRS80_FIRMWARE), else they say so and pass.
//  - level1.rom, level2.rom, model3.rom: the BASICs come up and answer;
//  - model4p.rom: a 4P boot ROM (e.g. xtrs's free xtrsrom4p);
//  - ld1-531.dsk, ld3-531.dsk (LDOS 5.3.1), ld4-631.dsk (LS-DOS 6.3.1):
//    Misosys's free images (tim-mann.org/misosys.html); each boots, takes
//    the date and lists its directory.

#include "test_harness.h"
#include "trs80_test_rom.h"
#include "libdevices/main/io_registry.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace trs80test;
using trs80::Model;

namespace {

bool have(const std::string& file) {
    std::ifstream f(trs80::firmwareDir() + "/" + file);
    if (!f) std::printf("  (no %s in %s: skipped)\n", file.c_str(), trs80::firmwareDir().c_str());
    return (bool)f;
}

// The screen's text: video RAM with bit 7 (graphics) off
std::string screen(MachineDescriptor* m) {
    std::string s;
    auto* sys = trs80::systemOf(m);
    for (int i = 0; i < 2048; ++i) s += (char)(sys->videoByte(i) & 0x7F);
    return s;
}

// Run until `text` is on the screen (true) or `seconds` pass
bool waitFor(MachineDescriptor* m, const std::string& text, double seconds) {
    for (double t = 0; t < seconds; t += 0.1) {
        if (screen(m).find(text) != std::string::npos) return true;
        runFor(m, 0.1);
    }
    const bool found = screen(m).find(text) != std::string::npos;
    if (!found) std::printf("  waiting for \"%s\"; the screen:\n%s\n", text.c_str(), screen(m).c_str());
    return found;
}

void type(MachineDescriptor* m, const std::string& text) { trs80::systemOf(m)->keyboard.enqueueText(text); }

// A copy of a disk image to boot from (writes go back to the file)
std::string diskCopy(const std::string& file) {
    const std::string to = "/tmp/mmsim_trs80_" + std::to_string((long)getpid()) + "_" + file;
    std::ifstream i(trs80::firmwareDir() + "/" + file, std::ios::binary);
    std::ofstream o(to, std::ios::binary | std::ios::trunc);
    o << i.rdbuf();
    return to;
}

MachineDescriptor* makeReal(Model model) {
    std::string err;
    MachineDescriptor* m = trs80::create(model, &err);
    if (m && !err.empty()) std::printf("  %s\n", err.c_str());
    return m;
}

// Boot a DOS disk: answer the date `prompt`, then DIR; `listed` a file the
// directory shows
void bootDos(Model model, const std::string& disk, const std::string& prompt, const std::string& date,
             const std::string& listed) {
    MachineDescriptor* m = makeReal(model);
    ASSERT(m != nullptr);
    const std::string path = diskCopy(disk);
    ASSERT(m->ioRegistry->mountDisk(1, path) != nullptr);
    EXPECT_TRUE(waitFor(m, "MISOSYS", 30));
    EXPECT_TRUE(waitFor(m, prompt, 30));
    runFor(m, 3);                                    // the DOS settles into its input
    type(m, date + "\\n");
    runFor(m, 3);
    type(m, "\\n");                                  // the time: none
    runFor(m, 5);
    type(m, "DIR\\n");
    EXPECT_TRUE(waitFor(m, listed, 30));
    unlink(path.c_str());
    delete m;
}

}  // namespace

TEST_CASE(trs80_rom_level1) {
    if (!have("level1.rom")) return;
    MachineDescriptor* m = makeReal(Model::M1Level1);
    ASSERT(m != nullptr);
    ASSERT(waitFor(m, "READY", 5));
    type(m, "PRINT 6*7\\n");
    EXPECT_TRUE(waitFor(m, "42", 5));
    delete m;
}

TEST_CASE(trs80_rom_level2) {
    if (!have("level2.rom")) return;
    MachineDescriptor* m = makeReal(Model::M1Level2);
    ASSERT(m != nullptr);
    ASSERT(waitFor(m, "MEM SIZE?", 5));
    type(m, "\\n");
    ASSERT(waitFor(m, "READY", 5));
    type(m, "PRINT 2+2\\n");
    EXPECT_TRUE(waitFor(m, " 4 ", 5));
    delete m;
}

TEST_CASE(trs80_rom_model3) {
    if (!have("model3.rom")) return;
    MachineDescriptor* m = makeReal(Model::M3);
    ASSERT(m != nullptr);
    ASSERT(waitFor(m, "Cass?", 5));
    type(m, "\\n");
    ASSERT(waitFor(m, "Memory Size?", 5));
    type(m, "\\n");
    ASSERT(waitFor(m, "READY", 5));
    type(m, "PRINT 2+2\\n");
    EXPECT_TRUE(waitFor(m, " 4 ", 5));
    delete m;
}

TEST_CASE(trs80_dos_ldos_model1) {
    if (!have("level2.rom") || !have("ld1-531.dsk")) return;
    bootDos(Model::M1Level2, "ld1-531.dsk", "DATE ?", "11/15/86", "TED/CMD");
}

TEST_CASE(trs80_dos_ldos_model3) {
    if (!have("model3.rom") || !have("ld3-531.dsk")) return;
    bootDos(Model::M3, "ld3-531.dsk", "Date ?", "11/15/86", "BASIC/HLP");
}

// LS-DOS 6.3.1 takes 1980-2011 dates
TEST_CASE(trs80_dos_lsdos_model4p) {
    if (!have("model4p.rom") || !have("ld4-631.dsk")) return;
    bootDos(Model::M4P, "ld4-631.dsk", "Date MM/DD/YY ?", "11/15/86", "TED/CMD");
}
