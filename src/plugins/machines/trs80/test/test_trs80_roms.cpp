// The real ROMs and DOSes, when they are there (#354). The ROMs are Tandy's
// and Microsoft's and never in the repository: these run only with them in
// roms/trs80/ (or $MMEMU_TRS80_FIRMWARE), else they say so and pass.
//  - level1.rom, level2.rom, model3.rom: the BASICs come up and answer;
//  - model4p.rom or 8075332.u69: a 4P boot ROM (xtrs's free xtrsrom4p, or
//    Radio Shack's);
//  - model4.rom: Radio Shack's Model 4 ROM (MAME's trs80m4.rom);
//  - ld1-531.dsk, ld3-531.dsk (LDOS 5.3.1), ld4-631.dsk (LS-DOS 6.3.1):
//    Misosys's free images (tim-mann.org/misosys.html); each boots, takes
//    the date and lists its directory;
//  - trsdos13.dsk (TRSDOS 1.3), nd80-m3.dsk (NEWDOS/80 v2.0, Model III),
//    trs621.dsk (TRSDOS 6.2.1): David Keil's images (electrickery.nl);
//    newdos80.dsk (NEWDOS/80 v2.0, Model I: classic-computers.org.nz);
//  - esf.rom: xtrs's public-domain esfrom.z80 assembled with zmac;
//  - rshard.dsk: Misosys's RSHARD hard disk drivers (tim-mann.org).

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

// The 4P's boot ROM: model4p.rom (e.g. xtrs's free one) or MAME's 8075332.u69
bool have4p() {
    for (const char* f : {"model4p.rom", "8075332.u69"}) {
        std::ifstream i(trs80::firmwareDir() + "/" + f);
        if (i) return true;
    }
    std::printf("  (no model4p.rom or 8075332.u69 in %s: skipped)\n", trs80::firmwareDir().c_str());
    return false;
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
    if (!have4p() || !have("ld4-631.dsk")) return;
    bootDos(Model::M4P, "ld4-631.dsk", "Date MM/DD/YY ?", "11/15/86", "TED/CMD");
}

// Radio Shack's Model 4 ROM (model4.rom, MAME's trs80m4.rom): LS-DOS 6.3.1
// in Model 4 mode, and LDOS 5.3.1 for the Model III in Model III mode
TEST_CASE(trs80_dos_lsdos_model4) {
    if (!have("model4.rom") || !have("ld4-631.dsk")) return;
    bootDos(Model::M4, "ld4-631.dsk", "Date MM/DD/YY ?", "11/15/86", "TED/CMD");
}

TEST_CASE(trs80_dos_ldos_model4_as_iii) {
    if (!have("model4.rom") || !have("ld3-531.dsk")) return;
    bootDos(Model::M4, "ld3-531.dsk", "Date ?", "11/15/86", "BASIC/HLP");
}

// The 4P in Model III mode, as its ROM offers it: F3 (start the ROM image)
// and P (pause once it is loaded) held at reset load MODELA/III, the Model
// III ROM image, from TRSDOS 6.2.1 in drive 0; with "The ROM Image Has
// Been Loaded" shown, LDOS 5.3.1 for the Model III goes in its place and
// ENTER starts the image, which boots LDOS as on a Model III
TEST_CASE(trs80_dos_model4p_modela) {
    if (!have4p() || !have("trs621.dsk") || !have("ld3-531.dsk")) return;
    MachineDescriptor* m = makeReal(Model::M4P);
    ASSERT(m != nullptr);
    auto* sys = trs80::systemOf(m);
    const std::string sysDisk = diskCopy("trs621.dsk"), iiiDisk = diskCopy("ld3-531.dsk");
    ASSERT(m->ioRegistry->mountDisk(1, sysDisk) != nullptr);
    ASSERT(sys->keyboard.key("f3", true));
    ASSERT(sys->keyboard.key("p", true));
    runFor(m, 1);
    sys->keyboard.key("f3", false);
    sys->keyboard.key("p", false);
    ASSERT(waitFor(m, "The ROM Image Has Been Loaded", 20));
    ASSERT(m->ioRegistry->mountDisk(1, iiiDisk) != nullptr);
    type(m, "\\n");
    EXPECT_TRUE(waitFor(m, "MISOSYS", 30));
    EXPECT_TRUE(waitFor(m, "Date ?", 30));
    runFor(m, 3);
    type(m, "11/15/86\\n");
    runFor(m, 3);
    type(m, "\\n");
    runFor(m, 5);
    type(m, "DIR\\n");
    EXPECT_TRUE(waitFor(m, "BASIC/HLP", 30));
    unlink(sysDisk.c_str());
    unlink(iiiDisk.c_str());
    delete m;
}

// Radio Shack's and Apparat's DOSes (images from David Keil's collection,
// now at electrickery.nl, and classic-computers.org.nz): each boots, comes
// to its prompt and lists its directory
TEST_CASE(trs80_dos_others) {
    const struct { Model model; const char* rom; const char* disk; const char* ready; const char* dir;
                   const char* listed; } dos[] = {
        {Model::M3, "model3.rom", "trsdos13.dsk", "TRSDOS Ready", "DIR", "MEMTEST/CMD"},
        {Model::M1Level2, "level2.rom", "newdos80.dsk", "NEWDOS/80 READY", "DIR 0", "SUPERZAP/CMD"},
        {Model::M3, "model3.rom", "nd80-m3.dsk", "NEWDOS/80 READY", "DIR", "SUPERZAP/CMD"},
        {Model::M4, "model4.rom", "trs621.dsk", "TRSDOS Ready", "DIR", "TAPE100/CMD"},
        {Model::M4P, nullptr, "trs621.dsk", "TRSDOS Ready", "DIR", "TAPE100/CMD"},
    };
    for (const auto& d : dos) {
        if (!(d.rom ? have(d.rom) : have4p()) || !have(d.disk)) continue;
        std::printf("  %s\n", d.disk);
        MachineDescriptor* m = makeReal(d.model);
        ASSERT(m != nullptr);
        const std::string path = diskCopy(d.disk);
        ASSERT(m->ioRegistry->mountDisk(1, path) != nullptr);
        EXPECT_TRUE(waitFor(m, d.ready, 30));
        runFor(m, 3);
        type(m, std::string(d.dir) + "\\n");
        EXPECT_TRUE(waitFor(m, d.listed, 30));
        unlink(path.c_str());
        delete m;
    }
}

// The Exatron Stringy Floppy with its own ROM (esf.rom: xtrs's public-domain
// esfrom.z80 assembled with zmac) on Level II: SYSTEM /12345 (/1234N, N = 5:
// no load) starts it, then @NEW erases a blank wafer, @SAVE1 writes a
// program, NEW, @LOAD1 reads it back. The ESF's key debounce (with 1.3's,
// about 45 ms) drops a key pressed while BASIC is busy, so each command
// waits for the one before to be typed and done.
TEST_CASE(trs80_rom_stringy) {
    if (!have("level2.rom") || !have("esf.rom")) return;
    MachineDescriptor* m = makeReal(Model::M1Level2);
    ASSERT(m != nullptr);
    std::string err;
    ASSERT(m->expansionSlot("stringy")->install("stringy", "", err));
    auto* sys = trs80::systemOf(m);
    ASSERT(sys->stringy.load(0, trs80::Stringy::create(4), ""));   // 4 KB: @NEW erases it all
    auto line = [&](const std::string& text) {
        type(m, text + "\\n");
        for (int i = 0; i < 100 && sys->keyboard.typing(); ++i) runFor(m, 0.1);
        runFor(m, 0.5);
    };
    ASSERT(waitFor(m, "MEM SIZE?", 5));
    line("");
    ASSERT(waitFor(m, "READY", 5));
    line("SYSTEM");
    ASSERT(waitFor(m, "*?", 5));
    line("/12345");
    ASSERT(waitFor(m, "EXATRON STRINGY FLOPPY VERSION 4.1", 5));
    line("@NEW");
    ASSERT(waitFor(m, "BYTES..DONE", 60));
    line("10 PRINT \"WAFER\";6*7");
    line("@SAVE1");
    ASSERT(waitFor(m, "WRITING..DONE", 30));
    line("NEW");
    line("@LOAD1");
    ASSERT(waitFor(m, "READING..DONE", 30));
    line("RUN");
    EXPECT_TRUE(waitFor(m, "WAFER 42", 10));
    delete m;
}

// The hard disk with Misosys's RSHARD (rshard.dsk, free from tim-mann.org's
// rshard.zip): LS-DOS 6.3.1 on the 4P installs RSHARD6 for drive 2 on a
// blank 20-cylinder, 2-head .hdv, RSFORM6 formats it, HDCHECK6 is copied on
// and, run from the hard disk, reads every sector back
TEST_CASE(trs80_dos_rshard_model4p) {
    if (!have4p() || !have("ld4-631.dsk") || !have("rshard.dsk")) return;
    MachineDescriptor* m = makeReal(Model::M4P);
    ASSERT(m != nullptr);
    auto* sys = trs80::systemOf(m);
    const std::string sysDisk = diskCopy("ld4-631.dsk"), rsDisk = diskCopy("rshard.dsk");
    ASSERT(m->ioRegistry->mountDisk(1, sysDisk) != nullptr);
    ASSERT(m->ioRegistry->mountDisk(2, rsDisk) != nullptr);
    ASSERT(sys->hard.load(0, trs80::HardDisk::create(20, 2), ""));
    // Radio Shack's 4P ROM boots a hard disk first (a blank image's zero
    // sector would run): hold F2, the floppy, through its key scan
    ASSERT(sys->keyboard.key("f2", true));
    runFor(m, 1);
    sys->keyboard.key("f2", false);
    // LS-DOS scans the keys in its 30 Hz interrupt, off during disk I/O:
    // let each command finish before typing the next
    auto line = [&](const std::string& text) {
        runFor(m, 2);
        type(m, text + "\\n");
        for (int i = 0; i < 100 && sys->keyboard.typing(); ++i) runFor(m, 0.1);
        runFor(m, 1);
    };
    ASSERT(waitFor(m, "Date MM/DD/YY ?", 30));
    runFor(m, 3);
    line("11/15/86");
    line("");
    runFor(m, 3);
    line("SYSTEM (DRIVE=2,DISABLE,DRIVER=\"RSHARD6:1\")");
    const struct { const char* prompt; const char* answer; } driver[] = {
        {"Enter drive address", ""}, {"STEP RATE", ""}, {"TRACKS per surface", "20"},
        {"total number of HEADS", "2"}, {"number of heads <1-2>", "2"}, {"number of cylinders", ""},
    };
    for (const auto& q : driver) {
        ASSERT(waitFor(m, q.prompt, 10));
        line(q.answer);
    }
    ASSERT(waitFor(m, "Drive appears to be unformatted", 10));
    line("RSFORM6:1 :2 (NAME=\"RIGID1\",MPW=\"SECRET\")");
    ASSERT(waitFor(m, "format it <Y,N>?", 10));
    line("Y");
    ASSERT(waitFor(m, "Lock out track", 10));
    line("N");
    ASSERT(waitFor(m, "Formatting complete", 60));
    line("COPY HDCHECK6/CMD:1 :2");
    runFor(m, 5);
    line("DIR :2");
    EXPECT_TRUE(waitFor(m, "RIGID1     20 Cyl, Hard", 10));
    EXPECT_TRUE(waitFor(m, "HDCHECK6/CMD", 10));
    line("HDCHECK6:2 :2");
    EXPECT_TRUE(waitFor(m, "Check is complete - No errors found", 120));
    unlink(sysDisk.c_str());
    unlink(rsDisk.c_str());
    delete m;
}


