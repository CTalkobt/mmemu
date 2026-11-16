// The real Dragon ROMs, DOS cartridges and bank-switched ROM Paks, when
// they are there (#355). They are Microsoft's, Dragon Data's, Tandy's and
// others' and never in the repository: these run only with them in
// roms/dragon/ (or $MMEMU_DRAGON_FIRMWARE) and roms/coco/ (or
// $MMEMU_COCO_FIRMWARE), else they say so and pass.
//  - d64_1 / d64rom1 (+ d64_2): Dragon 64 BASIC; the 200-E falls back to them;
//  - ddos12a, dplus49b, sdose6: each DOS signs on, DSKINITs a blank VDK,
//    SAVEs a program, lists it with DIR and LOADs and RUNs it back (the
//    other versions with MMEMU_DOS_SWEEP set); deltados (Delta 2.0) lists
//    a blank disk's directory (it has no format command);
//  - coco3.rom / extbas11.rom and paks/*.ccc (from the Color Computer
//    Archive): RoboCop and Predator (banked), Super Pitfall and Rampage!
//    (32 KB) put up their screens; Mind-Roll (banked) on a CoCo 2.

#include "test_harness.h"
#include "coco_test_rom.h"
#include "plugins/machines/coco/main/coco_paks.h"
#include "plugins/devices/gime/main/gime.h"
#include "libdevices/main/io_registry.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace cocotest;
using coco::Model;

namespace {

bool haveFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) std::printf("  (no %s: skipped)\n", path.c_str());
    return (bool)f;
}

// The 32 x 16 text screen at $0400 as ASCII (the VDG's 6-bit codes; inverse is upper case)
std::string screen(MachineDescriptor* m) {
    std::string s;
    IBus* bus = m->buses[0].bus;
    for (int i = 0; i < 512; ++i) {
        uint8_t c = bus->peek8((uint32_t)(0x400 + i)) & 0x7F;
        if (c & 0x40) c &= 0x3F;            // normal video
        else c &= 0x3F;
        s += (char)(c < 0x20 ? c + 0x40 : c);
        if (i % 32 == 31) s += '\n';
    }
    return s;
}

void runSeconds(MachineDescriptor* m, double seconds) { run(m, (uint64_t)(seconds * 890000.0)); }

// `text` on the screen, after `after` if that's given (a command's echo)
// (the echo may have scrolled away, or the command cleared the screen: then
// anywhere, unless `strict`)
bool onScreen(MachineDescriptor* m, const std::string& text, const std::string& after, bool strict = false) {
    const std::string s = screen(m);
    size_t from = after.empty() ? std::string::npos : s.find(after);
    if (from == std::string::npos && strict) return false;
    from = from == std::string::npos ? 0 : from + after.size();
    return s.find(text, from) != std::string::npos;
}

bool waitFor(MachineDescriptor* m, const std::string& text, double seconds, const std::string& after = "") {
    for (double t = 0; t < seconds; t += 0.1) {
        if (onScreen(m, text, after)) return true;
        runSeconds(m, 0.1);
    }
    const bool found = onScreen(m, text, after);
    if (!found && seconds > 0) std::printf("  waiting for \"%s\"; the screen:\n%s\n", text.c_str(), screen(m).c_str());
    return found;
}

void type(MachineDescriptor* m, const std::string& text) { coco::systemOf(m)->keyboard.enqueueText(text); }

MachineDescriptor* makeReal(Model model) {
    std::string err;
    MachineDescriptor* m = coco::create(model, &err, model == Model::CoCo3 ? 128 : 64);
    if (m && !err.empty()) std::printf("  %s\n", err.c_str());
    return m;
}

bool haveDragon64() {
    const std::string d = coco::dragonFirmwareDir();
    for (const char* n : {"d64_1.rom", "d64rom1.rom", "dragrom.rom"})
        if (std::ifstream(d + "/" + n)) return true;
    std::printf("  (no Dragon 64 BASIC in %s: skipped)\n", d.c_str());
    return false;
}

// A blank 40-track single-sided VDK
std::string blankVdk() {
    const std::string to = "/tmp/mmsim_dragon_" + std::to_string((long)getpid()) + "_blank.vdk";
    std::vector<uint8_t> img = {'d', 'k', 12, 0, 0x10, 0x10, 'M', 0, 40, 1, 0, 0};
    img.resize(12 + 40 * 18 * 256, 0x00);
    std::ofstream f(to, std::ios::binary | std::ios::trunc);
    f.write((const char*)img.data(), (std::streamsize)img.size());
    return to;
}

// On a cleared screen, `line` typed; true once `expect` follows its echo
bool command(MachineDescriptor* m, const std::string& line, const std::string& expect, double seconds) {
    // A DOS busy after its last command (the motor running down) drops keys
    // typed meanwhile: wait for the motor to stop, clear the screen, and type
    // again until the line's echo shows
    const std::string echo = line.substr(0, line.find('\\'));
    coco::Cart* cart = coco::systemOf(m)->cart();
    for (int tries = 0; tries < 5; ++tries) {
        int track = 0;
        bool motor = true;
        for (double t = 0; t < 10 && cart && cart->diskStatus(0, track, motor) && motor; t += 0.1) runSeconds(m, 0.1);
        for (int k = 0; k < 20; ++k) {
            type(m, "CLS\\n");
            runSeconds(m, 1.0);
            std::string s = screen(m);
            s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
            s.erase(std::remove(s.begin(), s.end(), '\n'), s.end());
            if (s == "OK") break;
        }
        type(m, line + "\\n");
        bool echoed = false;
        for (double t = 0; t < 3 && !echoed; t += 0.05) {
            runSeconds(m, 0.05);
            echoed = onScreen(m, echo, "", true);
        }
        if (echoed) break;
    }
    return waitFor(m, expect, seconds, echo);
}

// A Dragon 64 with the DOS cartridge `device` (its ROM `rom`) and `disk` in
// drive 1, signed on (`banner`; `key` answers its prompt); nullptr (skipped)
// without the ROMs
MachineDescriptor* dosMachine(const std::string& device, const std::string& rom, const std::string& banner,
                              const std::string& key, const std::string& disk) {
    const std::string path = coco::dragonFirmwareDir() + "/" + rom;
    if (!haveDragon64() || !haveFile(path)) return nullptr;
    MachineDescriptor* m = makeReal(Model::Dragon64);
    if (!m) return nullptr;
    std::string err;
    EXPECT_TRUE(m->expansionSlot("cart")->install(device, path, err));
    EXPECT_TRUE(m->ioRegistry->mountDisk(1, disk) != nullptr);
    m->onReset(*m);
    EXPECT_TRUE(waitFor(m, banner, 10));
    if (!key.empty()) {
        runSeconds(m, 1.0);
        type(m, key);
    }
    EXPECT_TRUE(waitFor(m, "OK", 10));
    return m;
}

// DSKINIT the disk: OK after the command (DragonDOS, SuperDOS) or after its
// track count (DOSplus, which asks SURE? first)
bool dskinit(MachineDescriptor* m) {
    command(m, "DSKINIT", "OK", 0);
    bool asked = false;
    for (double t = 0; t < 120; t += 0.5) {
        runSeconds(m, 0.5);
        if (!asked && onScreen(m, "SURE", "DSKINIT", true)) {
            type(m, "Y");
            asked = true;
        }
        if (onScreen(m, "OK", "DSKINIT", true) || onScreen(m, "OK", "CHECKING", true)) return true;
    }
    return false;
}

// A program saved, DIR lists it with the free space (`free`), and it loads and runs back
void saveListRun(MachineDescriptor* m, const std::string& free) {
    type(m, "10 PRINT 6*7\\n");
    EXPECT_TRUE(command(m, "SAVE\"HELLO\"", "OK", 30));
    EXPECT_TRUE(command(m, "DIR", free, 30));
    EXPECT_TRUE(onScreen(m, "HELLO", ""));
    std::printf("%s", screen(m).c_str());
    type(m, "NEW\\n");
    EXPECT_TRUE(command(m, "LOAD\"HELLO\"", "OK", 30));
    EXPECT_TRUE(command(m, "RUN", "42", 30));
}

// A DOS formats a blank disk itself, then saves, lists and runs
void formatsItsOwn(const std::string& device, const std::string& rom, const std::string& banner, const std::string& key,
                   const std::string& free) {
    const std::string disk = blankVdk();
    if (MachineDescriptor* m = dosMachine(device, rom, banner, key, disk)) {
        EXPECT_TRUE(dskinit(m));
        saveListRun(m, free);
        delete m;
    }
    std::remove(disk.c_str());
}

}  // namespace

TEST_CASE(real_dragon200e_falls_back_to_dragon64_basic) {
    if (!haveDragon64()) return;
    MachineDescriptor* m = makeReal(Model::Dragon200E);
    ASSERT(m != nullptr);
    EXPECT_TRUE(waitFor(m, "OK", 10));
    type(m, "PRINT 6*7\\n");
    EXPECT_TRUE(waitFor(m, "42", 5));
    delete m;
}

TEST_CASE(real_dragondos_formats_and_lists) { formatsItsOwn("dragondos", "ddos12a.rom", "DRAGON", "", "FREE"); }
TEST_CASE(real_superdos_formats_and_lists) { formatsItsOwn("superdos", "sdose6.rom", "DOS", "", "FREE"); }
TEST_CASE(real_dosplus_formats_and_lists) { formatsItsOwn("dosplus", "dplus49b.rom", "DOSPLUS", " ", "FREE"); }

// The other versions too, with MMEMU_DOS_SWEEP set (each takes a while)
TEST_CASE(real_dos_versions_sweep) {
    if (!std::getenv("MMEMU_DOS_SWEEP")) return;
    for (const char* r : {"ddos10.rom", "ddos11c.rom", "ddos40.rom", "ddos42.rom"}) formatsItsOwn("dragondos", r, "DOS", "", "FREE");
    for (const char* r : {"dplus48.rom", "dplus50.rom"}) formatsItsOwn("dosplus", r, "DOSPLUS", " ", "FREE");
    formatsItsOwn("superdos", "sdose8.rom", "DOS", "", "FREE");
}

// Delta has no format command in its ROM: a blank disk's (empty) directory
TEST_CASE(real_deltados_lists_a_blank_disk) {
    const std::string disk = blankVdk();
    if (MachineDescriptor* m = dosMachine("deltados", "deltados.rom", "DELTA SYSTEM", " ", disk)) {
        EXPECT_TRUE(command(m, "DIR", "DOMAINS FREE", 30));
        EXPECT_TRUE(onScreen(m, "DISK DIRECTORY", ""));
        delete m;
    }
    std::remove(disk.c_str());
}

// ---------------------------------------------------------------------------
// CoCo 3 ROM Paks over 16 KB
// ---------------------------------------------------------------------------
namespace {

// The CoCo 3 (or `model`) with `pak` (from roms/coco/paks/) inserted as `cart` does: its
// type auto-detected (`kind`); it starts and puts a picture up in the CoCo
// 3's own graphics modes; a banked one switches banks
void coco3Pak(const std::string& pak, const std::string& kind, Model model = Model::CoCo3) {
    const std::string dir = coco::firmwareDir();
    if (!haveFile(dir + (model == Model::CoCo3 ? "/coco3.rom" : "/extbas11.rom")) || !haveFile(dir + "/paks/" + pak)) return;
    std::ifstream f(dir + "/paks/" + pak, std::ios::binary);
    std::vector<uint8_t> image((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    MachineDescriptor* m = makeReal(model);
    ASSERT(m != nullptr);
    std::string err;
    ASSERT(coco::insertRomPak(m, image, pak, err));
    coco::System* sys = coco::systemOf(m);
    ASSERT_EQ(sys->cart()->kind(), kind);
    m->onReset(*m);
    auto* banked = dynamic_cast<coco::BankedPak*>(sys->cart());
    std::vector<bool> banks(64, false);
    bool graphics = false;
    for (int k = 0; k < 120; ++k) {                 // 12 s
        runSeconds(m, 0.1);
        if (banked) banks[(banked->bankOffset() >> 14) & 63] = true;
        if (sys->gime()) graphics = graphics || (!sys->gime()->coco() && (sys->gime()->reg(8) & 0x80));
        else graphics = graphics || (sys->pia1.portB() & 0x80);      // the VDG's A/G
    }
    if (!sys->gime()) {                             // a CoCo 2: the VDG's graphics, the banks
        int n = 0;
        for (bool b : banks) n += b;
        std::printf("  %s: %d banks seen\n", pak.c_str(), n);
        EXPECT_TRUE(graphics);
        if (banked) EXPECT_TRUE(n >= 2);
        delete m;
        return;
    }
    if (const char* shots = std::getenv("MMEMU_PAK_SHOTS")) {   // a picture to look at
        std::ofstream o(std::string(shots) + "/" + pak + ".pgm", std::ios::binary);
        o << "P5 " << Gime::OUT_W << " " << Gime::OUT_H << " 63\n";
        o.write((const char*)sys->gime()->frame(), Gime::OUT_W * Gime::OUT_H);
    }
    EXPECT_TRUE(graphics);                         // GIME mode, VMODE BP (graphics)
    int colours = 0;
    std::vector<bool> seen(64, false);
    for (int y = 0; y < Gime::OUT_H; ++y)
        for (int x = 0; x < Gime::OUT_W; ++x) {
            const uint8_t c = sys->gime()->pixel(x, y) & 63;
            if (!seen[c]) { seen[c] = true; ++colours; }
        }
    EXPECT_TRUE(colours >= 2);
    int n = 0;
    for (bool b : banks) n += b;
    std::printf("  %s: %d colours", pak.c_str(), colours);
    if (banked) std::printf(", %d banks seen", n);
    std::printf("\n");
    if (banked) EXPECT_TRUE(n >= 2);
    delete m;
}

}  // namespace

TEST_CASE(real_coco3_robocop_banked) { coco3Pak("Robocop (1988) (26-3164) (Data East) (Coco 3).ccc", "gmc"); }
TEST_CASE(real_coco3_predator_banked) { coco3Pak("Predator (1989) (26-3165) (Tandy) (Coco 3).ccc", "gmc"); }
TEST_CASE(real_coco3_super_pitfall_32k) { coco3Pak("Super Pitfall (1988) (26-3171) (Tandy) (Coco 3).ccc", "rompak"); }
TEST_CASE(real_coco3_rampage_32k) { coco3Pak("Rampage! (1989) (26-3174) (Tandy) (Coco 3).ccc", "rompak"); }
// Mind-Roll: 32 KB, banked (XRoar's database), on a CoCo 2
TEST_CASE(real_coco2_mind_roll_banked) {
    coco3Pak("Mind-Roll (1988) (26-3100) (Tandy) (Coco 1-2) (Coco 3).ccc", "gmc", Model::CoCo2);
}
