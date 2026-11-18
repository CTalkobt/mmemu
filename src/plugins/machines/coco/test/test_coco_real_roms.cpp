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
//    (32 KB) put up their screens; Mind-Roll (banked) on a CoCo 2;
//  - extbas11 + disk11: Disk BASIC on the Disto Super Controller II lists
//    a disk's file with DIR, then SAVEs, lists, LOADs and RUNs a program.
//  - nitros9/*.dsk (NitrOS-9 v3.3.0's release disks): each build boots to its
//    shell and runs dir (CoCo 1/2/3, 6309, CoCo SDC, Dragon 64), and the
//    Becker builds over DriveWire with HDB-DOS (hdbdw3bc3.rom, hdbdw3bck.rom),
//    and the bit-banger (_dw) builds over DriveWire on the serial port with
//    HDB-DOS built for it (hdbdw3cc3.rom, hdbdw3cc2.rom, hdbdw3cc1.rom).

#include "test_harness.h"
#include "coco_test_rom.h"
#include "plugins/machines/coco/main/coco_paks.h"
#include "plugins/machines/coco/main/coco_storage.h"
#include "plugins/devices/gime/main/gime.h"
#include "libdevices/main/io_registry.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
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

// Burke & Burke's own OS-9 boot ROM (the CoCo XT-RTC's, an XT-ROM as burned:
// A4-A6 scrambled; roms/coco/xt-rtc/) in the controller's BIOS socket boots a
// CoCo 2 from a hard disk image through our interface (#356). The ROM, on a
// cold start: waits for the drive, tests RAM, resets the controller, reads
// physical sector 0 (cylinder 0, head 0) for the drive's geometry ($44-$45
// cylinders, $46 heads), sends it with $0C, checks the drive with $00 and
// reads nine 512-byte sectors of cylinder 128, head 0 to $2600, which must
// start "OS"; then it jumps to $2602. The image here is hand-built.
TEST_CASE(real_burke_xt_rom_boots_from_hard_disk) {
    const std::string rom = coco::firmwareDir() + "/xt-rtc/Burke_and_Burke_CoCo_XT-RTC_OS-9_Boot_ROM_8K.bin";
    if (!haveFile(rom)) return;
    const int heads = 4, cyls = 130, spt = 17;
    const std::string img = "/tmp/mmsim_burke_" + std::to_string((long)getpid()) + ".img";
    std::vector<uint8_t> disk((size_t)cyls * heads * spt * 512, 0);
    disk[0x44] = 0;
    disk[0x45] = (uint8_t)cyls;
    disk[0x46] = (uint8_t)heads;
    const size_t boot = (size_t)128 * heads * spt * 512;
    for (size_t i = 0; i < 9 * 512; ++i) disk[boot + i] = (uint8_t)(i * 7 + (i >> 9));
    const uint8_t code[] = {'O', 'S', 0x86, 0x5A, 0xB7, 0x3A, 0x00, 0x20, 0xFE};   // LDA #$5A; STA $3A00; BRA *
    std::copy(std::begin(code), std::end(code), disk.begin() + (std::ptrdiff_t)boot);
    {
        std::ofstream f(img, std::ios::binary | std::ios::trunc);
        f.write((const char*)disk.data(), (std::streamsize)disk.size());
    }
    // Color BASIC's stand-in sets PIA0 up and starts the cartridge at $C002
    // (after its "DK", as Extended BASIC does)
    coco::Roms roms;
    Asm a(0xA000);
    a.emit({0x10, 0xCE, 0x7F, 0x00});           // LDS #$7F00
    a.ext(0x7F, 0xFF01); a.ext(0x7F, 0xFF00);   // PIA0 A in
    a.emit({0x86, 0x04}); a.ext(0xB7, 0xFF01);
    a.ext(0x7F, 0xFF03);
    a.emit({0x86, 0xFF}); a.ext(0xB7, 0xFF02);  // B out (the keyboard's columns)
    a.emit({0x86, 0x04}); a.ext(0xB7, 0xFF03);
    a.ext(0xB7, 0xFFDD);                        // SAM: 64K (M1)
    a.ext(0x7E, 0xC002);                        // JMP $C002
    a.word(0xBFF2, 0x0100); a.word(0xBFF4, 0x0103); a.word(0xBFF6, 0x010F);   // the RAM vectors BASIC's are
    a.word(0xBFF8, 0x010C); a.word(0xBFFA, 0x0106); a.word(0xBFFC, 0x0109);
    a.word(0xBFFE, 0xA000);
    roms.colorBasic = a.b;
    std::string err;
    MachineDescriptor* m = coco::create(Model::CoCo2, &err, 64, &roms);
    ASSERT(m != nullptr);
    ASSERT(m->expansionSlot("cart")->install("burke", img + "," + rom, err));
    m->onReset(*m);
    IBus* bus = m->buses[0].bus;
    bool booted = false;
    for (int i = 0; i < 60 && !booted; ++i) {
        run(m, 1000000);
        booted = bus->peek8(0x3A00) == 0x5A;
    }
    EXPECT_TRUE(booted);
    bool same = true;
    for (size_t i = 0; i < 9 * 512; ++i) same = same && bus->peek8((uint16_t)(0x2600 + i)) == disk[boot + i];
    EXPECT_TRUE(same);
    auto* bb = dynamic_cast<coco::BurkeBurke*>(coco::systemOf(m)->cart());
    ASSERT(bb != nullptr);
    EXPECT_EQ(bb->controller().cylinders(0), cyls);   // from its $0C
    EXPECT_EQ(bb->controller().heads(0), heads);
    delete m;
    std::remove(img.c_str());
}

// ---------------------------------------------------------------------------
// Disk BASIC 1.1 (disk11.rom) on the Disto Super Controller II
// ---------------------------------------------------------------------------
namespace {

// A 35-track RS-DOS disk with one file, HELLO.TXT (granule 0, 5 bytes)
std::string rsdosDisk() {
    const std::string to = "/tmp/mmsim_coco_" + std::to_string((long)getpid()) + "_rsdos.dsk";
    std::vector<uint8_t> img(35 * 18 * 256, 0xFF);
    auto sec = [](int t, int s) { return (size_t)((t * 18 + s - 1) * 256); };
    std::fill(img.begin() + (long)sec(17, 2) + 68, img.begin() + (long)sec(17, 2) + 256, 0);
    img[sec(17, 2)] = 0xC1;                                 // granule 0: the last, one sector
    const char name[] = "HELLO   TXT";
    for (int i = 0; i < 11; ++i) img[sec(17, 3) + i] = (uint8_t)name[i];
    const uint8_t rest[] = {3, 0xFF, 0, 0, 5};              // ASCII text, granule 0, 5 bytes in the last sector
    for (int i = 0; i < 5; ++i) img[sec(17, 3) + 11 + i] = rest[i];
    std::fill(img.begin() + (long)sec(17, 3) + 16, img.begin() + (long)sec(17, 3) + 32, 0);
    std::ofstream f(to, std::ios::binary | std::ios::trunc);
    f.write((const char*)img.data(), (std::streamsize)img.size());
    return to;
}

}  // namespace

// DIR lists a disk's file through the SC-II in its normal (FD-502) mode, and
// a program SAVEd is listed, LOADed and RUN back
TEST_CASE(real_disk_basic_on_disto_sc2) {
    if (!haveFile(coco::firmwareDir() + "/disk11.rom") || !haveFile(coco::firmwareDir() + "/extbas11.rom")) return;
    const std::string disk = rsdosDisk();
    MachineDescriptor* m = makeReal(Model::CoCo2);
    ASSERT(m != nullptr);
    std::string err;
    ASSERT(m->expansionSlot("cart")->install("sc2", "", err));
    EXPECT_TRUE(m->ioRegistry->mountDisk(1, disk) != nullptr);
    m->onReset(*m);
    EXPECT_TRUE(waitFor(m, "OK", 10));
    EXPECT_TRUE(command(m, "DIR", "HELLO", 30));
    EXPECT_TRUE(onScreen(m, "TXT", "HELLO"));
    std::printf("%s", screen(m).c_str());
    saveListRun(m, "BAS");
    delete m;
    std::remove(disk.c_str());
}

// ---------------------------------------------------------------------------
// NitrOS-9 v3.3.0 (the project's release disks, in roms/coco/nitros9/ when
// they're there; from sourceforge.net/projects/nitros9): each build boots
// with the machine's own Disk BASIC (DOS) or DragonDOS (BOOT) and lists its
// root directory with "dir"
// ---------------------------------------------------------------------------
namespace {

// The VDG's text screen where the SAM shows it (NitrOS-9 Level 1 doesn't use $0400)
std::string vdgText(MachineDescriptor* m) {
    const uint16_t base = coco::systemOf(m)->sam.f();   // F0-F6 as the address (F << 9)
    std::string s;
    IBus* bus = m->buses[0].bus;
    for (int i = 0; i < 512; ++i) {
        const uint8_t c = bus->peek8((uint32_t)(uint16_t)(base + i)) & 0x3F;
        s += (char)(c < 0x20 ? c + 0x40 : c);
        if (i % 32 == 31) s += '\n';
    }
    return s;
}

// The GIME's hi-res text screen (character, attribute pairs) from its video
// start, 24 rows of up to 80 columns, as ASCII
std::string gimeText(MachineDescriptor* m) {
    const Gime* g = coco::systemOf(m)->gime();
    const uint32_t start = (uint32_t)g->reg(0xD) << 11 | (uint32_t)g->reg(0xE) << 3;
    std::string s;
    for (int i = 0; i < 24 * 160; i += 2) {
        const uint8_t c = g->ramByte(start + (uint32_t)i);
        s += (char)(c >= 0x20 && c < 0x7F ? c : ' ');
        if (i % 160 == 158) s += '\n';
    }
    return s;
}

std::string os9Screen(MachineDescriptor* m) { return coco::systemOf(m)->gime() ? gimeText(m) : vdgText(m); }

std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

// `text` on the screen (case aside: Level 1's VDG screen has no lower case)
bool os9OnScreen(MachineDescriptor* m, const std::string& text) {
    return upper(os9Screen(m)).find(upper(text)) != std::string::npos;
}

bool os9WaitFor(MachineDescriptor* m, const std::string& text, double seconds) {
    for (double t = 0; t < seconds; t += 0.25) {
        if (os9OnScreen(m, text)) return true;
        runSeconds(m, 0.25);
    }
    if (os9OnScreen(m, text)) return true;
    std::printf("  waiting for \"%s\"; the screen:\n%s\n", text.c_str(), os9Screen(m).c_str());
    return false;
}

// An OS-9 module named `name` in the 64K map (its header: $87 $CD, size,
// name offset; the name's last letter has bit 7 set)
bool os9Module(MachineDescriptor* m, const std::string& name) {
    IBus* bus = m->buses[0].bus;
    for (uint32_t a = 0; a + 9 < 0x10000; ++a) {
        if (bus->peek8(a) != 0x87 || bus->peek8(a + 1) != 0xCD) continue;
        const uint32_t n = a + (uint32_t)(bus->peek8(a + 4) << 8 | bus->peek8(a + 5));
        bool same = n + name.size() <= 0x10000;
        for (size_t i = 0; same && i < name.size(); ++i) {
            const uint8_t c = bus->peek8((uint32_t)(n + i));
            const bool last = i + 1 == name.size();
            same = (c & 0x7F) == (uint8_t)name[i] && ((c & 0x80) != 0) == last;
        }
        if (same) return true;
    }
    return false;
}

bool os9WaitForModule(MachineDescriptor* m, const std::string& name, double seconds, bool quiet = false) {
    for (double t = 0; t < seconds; t += 0.5) {
        if (os9Module(m, name)) return true;
        runSeconds(m, 0.5);
    }
    const bool found = os9Module(m, name);
    if (!found && !quiet) {
        std::printf("  no module \"%s\" in memory; there are:", name.c_str());
        IBus* bus = m->buses[0].bus;
        for (uint32_t a = 0; a + 9 < 0x10000; ++a) {
            if (bus->peek8(a) != 0x87 || bus->peek8(a + 1) != 0xCD) continue;
            uint32_t n = a + (uint32_t)(bus->peek8(a + 4) << 8 | bus->peek8(a + 5));
            std::string nm;
            for (int i = 0; i < 12 && n < 0x10000; ++i, ++n) {
                const uint8_t c = bus->peek8(n);
                if ((c & 0x7F) < 0x20) break;
                nm += (char)(c & 0x7F);
                if (c & 0x80) break;
            }
            std::printf(" %s", nm.c_str());
        }
        std::printf("\n");
    }
    return found;
}

// Boot `disk` (in roms/coco/nitros9/) on `model` with `cart`, then "dir";
// `prompt` is the shell's, `boot` (its case as the screen shows it) the
// root directory's OS9Boot
void nitros9(Model model, bool hd6309, const std::string& cart, const std::string& cartOpt, const std::string& disk,
             const std::string& command, const std::string& prompt, const std::string& boot,
             const std::function<void(MachineDescriptor*)>& then = nullptr, const std::string& serial = "") {
    const std::string path = coco::firmwareDir() + "/nitros9/" + disk;
    if (!haveFile(path)) return;
    if (isDragon(model) ? !haveDragon64() : !haveFile(coco::firmwareDir() + "/disk11.rom")) return;
    std::string err;
    MachineDescriptor* m = coco::create(model, &err, model == Model::CoCo3 ? 512 : 64, nullptr, coco::Tv::Default, hd6309);
    ASSERT(m != nullptr);
    ASSERT(m->expansionSlot("cart")->install(cart, cartOpt, err));
    if (!serial.empty()) ASSERT(coco::systemOf(m)->rs232.attachSerial(serial));   // `serial drivewire`
    // A copy: NitrOS-9 writes to its disk
    const std::string copy = "/tmp/mmsim_nitros9_" + std::to_string((long)getpid()) + ".dsk";
    {
        std::ifstream in(path, std::ios::binary);
        std::ofstream out(copy, std::ios::binary | std::ios::trunc);
        out << in.rdbuf();
    }
    EXPECT_TRUE(m->ioRegistry->mountDisk(1, copy) != nullptr);
    m->onReset(*m);
    EXPECT_TRUE(waitFor(m, "OK", 10));
    runSeconds(m, 1.0);              // (HDB-DOS drops a key typed as soon as OK shows)
    type(m, command + "\\n");
    if (prompt.empty()) {
        // The Dragon 64's text is drawn on a graphics screen: the modules
        // say instead. OS9Boot (with the shell) loaded, then "dir" (typed
        // until the shell runs it: the first may answer "Time ?") loaded
        // from CMDS
        EXPECT_TRUE(os9WaitForModule(m, "Shell", 90));
        bool dir = false;
        for (int i = 0; i < 30 && !dir; ++i) {
            type(m, "dir\\n");
            dir = os9WaitForModule(m, "Dir", 5, i < 29);
        }
        EXPECT_TRUE(dir);
        delete m;
        std::remove(copy.c_str());
        return;
    }
    EXPECT_TRUE(os9WaitFor(m, "NitrOS-9", 60));
    // "Time ?" (when there's no clock to read): Return until the shell's prompt
    bool shell = false;
    for (int i = 0; i < 20 && !shell; ++i) {
        type(m, "\\n");
        for (double t = 0; t < 3 && !shell; t += 0.25) {
            runSeconds(m, 0.25);
            shell = os9OnScreen(m, prompt);
        }
    }
    EXPECT_TRUE(os9WaitFor(m, prompt, 0.25));
    type(m, "dir\\n");
    EXPECT_TRUE(os9WaitFor(m, boot, 30));
    if (then) then(m);
    std::printf("%s", os9Screen(m).c_str());
    delete m;
    std::remove(copy.c_str());
}

}  // namespace

TEST_CASE(real_nitros9_level2_coco3) {
    nitros9(Model::CoCo3, false, "fd502", "", "nos96809l2v030300coco3_40d_1.dsk", "DOS", "/DD:", "OS9Boot");
}
TEST_CASE(real_nitros9_level2_coco3_6309) {
    nitros9(Model::CoCo3, true, "fd502", "", "nos96309l2v030300coco3_40d_1.dsk", "DOS", "/DD:", "OS9Boot");
}
TEST_CASE(real_nitros9_level1_coco2) {
    nitros9(Model::CoCo2, false, "fd502", "", "nos96809l1v030300coco2_40d_1.dsk", "DOS", "OS9:", "OS9BOOT");
}
TEST_CASE(real_nitros9_level1_coco1) {
    nitros9(Model::CoCo1, false, "fd500", "", "nos96809l1v030300coco1_40d_1.dsk", "DOS", "OS9:", "OS9BOOT");
}
// DragonDOS's BOOT waits for the disk's FIRQ with SYNC and I and F set (a
// wait, not the end of a program); its screen is graphics (no prompt to read)
TEST_CASE(real_nitros9_level1_dragon64) {
    if (!haveFile(coco::dragonFirmwareDir() + "/ddos12a.rom")) return;
    nitros9(Model::Dragon64, false, "dragondos", coco::dragonFirmwareDir() + "/ddos12a.rom",
            "nos96809l1v030300d64_40d_1.dsk", "BOOT", "", "");
}
// The CoCo SDC with stock Disk BASIC in its flash: DOS loads the boot track
// through its FDC emulation, then NitrOS-9's SDC driver reads the hard disk
// image by LSN in command mode
TEST_CASE(real_nitros9_level2_coco3_sdc) {
    const std::string disk = coco::firmwareDir() + "/nitros9/nos96809l2v030300coco3_cocosdc.dsk";
    if (!haveFile(disk)) return;
    const std::string card = coco::firmwareDir() + "/nitros9";
    nitros9(Model::CoCo3, false, "sdc", card + "," + coco::firmwareDir() + "/disk11.rom",
            "nos96809l2v030300coco3_cocosdc.dsk", "DOS", "/DD:", "OS9Boot");
}
// The Becker port and the built-in DriveWire server: HDB-DOS built for the
// Becker port (hdbdw3bc3.rom for the CoCo 3, hdbdw3bck.rom for the CoCo 1/2;
// toolshed's hdbdos/, see doc/README-COCO.md) loads the boot track from
// DriveWire drive 0 with DOS, then NitrOS-9's rbdw/dwio (Becker build) read
// the rest by LSN and poll the server's virtual serial channels
namespace {
void nitros9Becker(Model model, bool hd6309, const std::string& rom, const std::string& disk, const std::string& prompt,
                   const std::string& boot, const std::function<void(MachineDescriptor*)>& then = nullptr) {
    const std::string hdb = coco::firmwareDir() + "/" + rom;
    if (!haveFile(hdb)) return;
    nitros9(model, hd6309, "becker", hdb, disk, "DOS", prompt, boot, then);
}
}  // namespace
TEST_CASE(real_nitros9_level2_coco3_becker) {
    // The clock: sysgo sets the build date, clock2_dw asks the server (OP_TIME)
    // when the minute turns over; then date shows the host's year
    nitros9Becker(Model::CoCo3, false, "hdbdw3bc3.rom", "nos96809l2v030300coco3_becker.dsk", "/DD:", "OS9Boot",
                  [](MachineDescriptor* m) {
                      auto* becker = dynamic_cast<coco::BeckerCart*>(coco::systemOf(m)->cart());
                      ASSERT(becker);
                      EXPECT_TRUE(becker->server().stats().serialPolls > 0);   // dwio polls the virtual channels
                      runSeconds(m, 61);
                      const std::time_t now = std::time(nullptr);
                      std::tm t{};
                      localtime_r(&now, &t);
                      type(m, "date\\n");
                      EXPECT_TRUE(os9WaitFor(m, ", " + std::to_string(1900 + t.tm_year), 10));
                  });
}
TEST_CASE(real_nitros9_level2_coco3_6309_becker) {
    nitros9Becker(Model::CoCo3, true, "hdbdw3bc3.rom", "nos96309l2v030300coco3_becker.dsk", "/DD:", "OS9Boot");
}
TEST_CASE(real_nitros9_level1_coco2_becker) {
    nitros9Becker(Model::CoCo2, false, "hdbdw3bck.rom", "nos96809l1v030300coco2_becker.dsk", "OS9:", "OS9BOOT");
}

// DriveWire over the bit banger: HDB-DOS built for the serial port
// (hdbdw3cc3.rom: the CoCo 3 at 115200 baud, 1.79 MHz; hdbdw3cc2.rom: the
// CoCo 2 at 57600; hdbdw3cc1.rom: 38400; toolshed's hdbdos/, see
// doc/README-COCO.md) in a ROM socket with no controller (`dosrom`), the
// built-in server on the serial port (`serial drivewire`); DOS loads the
// boot track, then NitrOS-9's own bit-banger rbdw/dwio (the _dw builds)
// read the rest, every bit cycle-counted on PIA1 PA1 / PB0
namespace {
void nitros9BitBanger(Model model, bool hd6309, const std::string& rom, const std::string& disk, const std::string& prompt,
                      const std::string& boot, const std::string& serial = "drivewire",
                      const std::function<void(MachineDescriptor*)>& then = nullptr) {
    const std::string hdb = coco::firmwareDir() + "/" + rom;
    if (!haveFile(hdb)) return;
    nitros9(model, hd6309, "dosrom", hdb, disk, "DOS", prompt, boot, then, serial);
}
}  // namespace
TEST_CASE(real_nitros9_level2_coco3_bitbanger) {
    nitros9BitBanger(Model::CoCo3, false, "hdbdw3cc3.rom", "nos96809l2v030300coco3_dw.dsk", "/DD:", "OS9Boot", "drivewire",
                     [](MachineDescriptor* m) {
                         coco::System* sys = coco::systemOf(m);
                         EXPECT_EQ(sys->rs232.baud(), 115200);
                         const auto& st = sys->rs232.driveWire()->stats();
                         EXPECT_TRUE(st.reads > 20);
                         EXPECT_EQ((int)st.crcErrors, 0);
                         EXPECT_TRUE(st.serialPolls > 0);              // dwio polls the virtual channels
                     });
}
TEST_CASE(real_nitros9_level2_coco3_6309_bitbanger) {
    nitros9BitBanger(Model::CoCo3, true, "hdbdw3cc3.rom", "nos96309l2v030300coco3_dw.dsk", "/DD:", "OS9Boot");
}
TEST_CASE(real_nitros9_level1_coco2_bitbanger) {
    nitros9BitBanger(Model::CoCo2, false, "hdbdw3cc2.rom", "nos96809l1v030300coco2_dw.dsk", "OS9:", "OS9BOOT", "drivewire",
                     [](MachineDescriptor* m) {
                         coco::System* sys = coco::systemOf(m);
                         EXPECT_EQ(sys->rs232.baud(), 57600);
                         EXPECT_EQ((int)sys->rs232.driveWire()->stats().crcErrors, 0);
                     });
}
// The CoCo 1 builds' driver and HDB-DOS's hdbdw3cc1 run at 38400
TEST_CASE(real_nitros9_level1_coco1_bitbanger) {
    nitros9BitBanger(Model::CoCo1, false, "hdbdw3cc1.rom", "nos96809l1v030300coco1_dw.dsk", "OS9:", "OS9BOOT",
                     "drivewire:38400");
}

// The headless builds (_becker_headless, _dw_headless): no screen or
// keyboard driver; the console, /Term, is DriveWire virtual channel 0. The
// shell's prompt and dir's listing come out of the server's channel and the
// commands go in through it. Then a second shell on /N1 (a channel with no
// far end but the test), and NitrOS-9's dw utility asks the server's
// virtual modem (on the next free /N) for its drives.
namespace {

std::string channelText(DriveWireServer* dw, int port) { return upper(dw->channelOutput(port)); }

bool channelWaitFor(MachineDescriptor* m, DriveWireServer* dw, int port, const std::string& text, double seconds) {
    for (double t = 0; t < seconds; t += 0.25) {
        if (channelText(dw, port).find(upper(text)) != std::string::npos) return true;
        runSeconds(m, 0.25);
    }
    if (channelText(dw, port).find(upper(text)) != std::string::npos) return true;
    std::string out = dw->channelOutput(port);
    for (char& c : out) if (c == '\r') c = '\n';
    std::printf("  waiting for \"%s\" on /%s; it has:\n%s\n", text.c_str(), DriveWireServer::portName(port).c_str(),
                out.c_str());
    return false;
}

void nitros9Headless(Model model, bool hd6309, const std::string& rom, const std::string& disk, const std::string& prompt,
                     const std::string& boot, const std::string& serial = "") {
    const std::string path = coco::firmwareDir() + "/nitros9/" + disk;
    const std::string hdb = coco::firmwareDir() + "/" + rom;
    if (!haveFile(path) || !haveFile(hdb)) return;
    std::string err;
    MachineDescriptor* m = coco::create(model, &err, model == Model::CoCo3 ? 512 : 64, nullptr, coco::Tv::Default, hd6309);
    ASSERT(m != nullptr);
    coco::System* sys = coco::systemOf(m);
    if (serial.empty()) {
        ASSERT(m->expansionSlot("cart")->install("becker", hdb, err));
    } else {
        ASSERT(m->expansionSlot("cart")->install("dosrom", hdb, err));
        ASSERT(sys->rs232.attachSerial(serial));
    }
    const std::string copy = "/tmp/mmsim_nitros9_hl_" + std::to_string((long)getpid()) + ".dsk";
    {
        std::ifstream in(path, std::ios::binary);
        std::ofstream out(copy, std::ios::binary | std::ios::trunc);
        out << in.rdbuf();
    }
    EXPECT_TRUE(m->ioRegistry->mountDisk(1, copy) != nullptr);
    m->onReset(*m);
    auto* becker = dynamic_cast<coco::BeckerCart*>(sys->cart());
    DriveWireServer* dw = serial.empty() ? (becker ? &becker->server() : nullptr) : sys->rs232.driveWire();
    ASSERT(dw != nullptr);
    EXPECT_TRUE(waitFor(m, "OK", 10));
    runSeconds(m, 1.0);
    type(m, "DOS\\n");
    // The console is DriveWire's channel 0: the banner, then the shell's prompt
    EXPECT_TRUE(channelWaitFor(m, dw, 0, "NitrOS-9", 60));
    bool shell = false;
    for (int i = 0; i < 20 && !shell; ++i) {
        for (double t = 0; t < 3 && !shell; t += 0.25) {
            runSeconds(m, 0.25);
            shell = channelText(dw, 0).find(upper(prompt)) != std::string::npos;
        }
        if (!shell) dw->hostWrite(0, "\r");                 // ("Time ?" without a clock)
    }
    EXPECT_TRUE(channelWaitFor(m, dw, 0, prompt, 0.25));
    EXPECT_TRUE(dw->channelOpen(0));
    dw->clearChannelOutput(0);
    dw->hostWrite(0, "dir\r");
    EXPECT_TRUE(channelWaitFor(m, dw, 0, boot, 30));
    EXPECT_TRUE(channelWaitFor(m, dw, 0, prompt, 10));
    // A second shell on /N1, driven through its channel
    std::string err2;
    ASSERT(dw->attachChannel(1, "none", err2));             // (not the virtual modem: a shell)
    dw->clearChannelOutput(0);
    dw->hostWrite(0, "shell i=/n1&\r");
    EXPECT_TRUE(channelWaitFor(m, dw, 0, prompt, 20));
    for (double t = 0; t < 20 && !dw->channelOpen(1); t += 0.25) runSeconds(m, 0.25);
    EXPECT_TRUE(dw->channelOpen(1));
    dw->hostWrite(1, "dir /dd/cmds\r");
    EXPECT_TRUE(channelWaitFor(m, dw, 1, "Shell", 30));
    // The dw utility: /N (the next free one, N2) to the virtual modem. (Level
    // 1 prints the reply but hangs when the channel's close is announced: not
    // checked there)
    dw->clearChannelOutput(0);
    dw->hostWrite(0, "dw disk show\r");
    EXPECT_TRUE(channelWaitFor(m, dw, 0, "mmsim_nitros9_hl_", 30));
    if (model == Model::CoCo3) {
        EXPECT_TRUE(channelWaitFor(m, dw, 0, prompt, 20));     // dw returned when the channel closed
        EXPECT_TRUE(!dw->channelOpen(2));
    }
    for (int port : {0, 1}) {
        std::string out;
        for (char c : dw->channelOutput(port)) if (c) out += c == '\r' ? '\n' : c;
        std::printf("/%s:\n%s\n", DriveWireServer::portName(port).c_str(), out.c_str());
    }
    delete m;
    std::remove(copy.c_str());
}

}  // namespace

TEST_CASE(real_nitros9_level2_coco3_becker_headless) {
    nitros9Headless(Model::CoCo3, false, "hdbdw3bc3.rom", "nos96809l2v030300coco3_becker_headless.dsk", "/DD:", "OS9Boot");
}
TEST_CASE(real_nitros9_level2_coco3_dw_headless) {
    nitros9Headless(Model::CoCo3, false, "hdbdw3cc3.rom", "nos96809l2v030300coco3_dw_headless.dsk", "/DD:", "OS9Boot",
                    "drivewire");
}
TEST_CASE(real_nitros9_level1_coco2_becker_headless) {
    nitros9Headless(Model::CoCo2, false, "hdbdw3bck.rom", "nos96809l1v030300coco2_becker_headless.dsk", "OS9:", "OS9BOOT");
}
