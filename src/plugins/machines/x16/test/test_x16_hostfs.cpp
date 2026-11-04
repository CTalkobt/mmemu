#include "test_harness.h"
#include "plugins/machines/x16/main/machine_x16.h"
#include "plugins/machines/x16/main/x16_hostfs.h"
#include "plugins/devices/vera/main/vera.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/intercept.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include <cstdio>
#include <filesystem>
#include <unistd.h>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct X16 {
    MachineDescriptor* m = nullptr;
    Vera* vera = nullptr;
    IKeyboardMatrix* kbd = nullptr;
    X16HostFs* hostfs = nullptr;

    bool boot(std::string& err) {
        m = x16::create(&err);
        if (!m) return false;
        std::vector<IOHandler*> hs;
        m->ioRegistry->enumerate(hs);
        for (auto* h : hs) {
            if (auto* v = dynamic_cast<Vera*>(h)) vera = v;
            if (auto* k = dynamic_cast<IKeyboardMatrix*>(h)) kbd = k;
        }
        hostfs = dynamic_cast<X16HostFs*>(findIntercept(*m, "x16.hostfs"));
        return true;
    }
    ~X16() { delete m; }
    void run(long steps) { for (long s = 0; s < steps; s++) m->schedulerStep(*m); }
    // Run until `text` is on the screen (or `maxSteps`); true if it appeared
    bool runUntil(const std::string& text, long maxSteps = 80000000) {
        for (long s = 0; s < maxSteps; s += 250000) {
            run(250000);
            if (screenHas(text)) return true;
        }
        return false;
    }
    // Type a command on a cleared screen and run until BASIC is READY again
    bool type(const std::string& t) {
        clearScreen();
        kbd->enqueueText(t);
        bool ok = runUntil("READY.");
        run(250000);
        return ok;
    }
    void clearScreen() {
        // the text screen at $1B000: spaces, so a check sees only new output
        for (uint32_t a = 0x1B000; a < 0x1F000; a += 2) vera->vramPoke(a, 0x20);
    }
    // The screen as text (rows of the 128-cell map), to print when a check fails
    std::string screenText() {
        std::string out;
        for (int row = 0; row < 60; row++) {
            std::string line;
            for (int col = 0; col < 80; col++) {
                uint8_t c = vera->vramPeek(0x1B000 + (uint32_t)row * 256 + (uint32_t)col * 2);
                line += (c >= 1 && c <= 26) ? (char)('@' + c) : (c >= 0x20 && c < 0x40) ? (char)c : '.';
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            if (!line.empty()) out += line + "\n";
        }
        return out;
    }
    // Text (upper case, digits, punctuation) as screen codes on the text screen
    bool screenHas(const std::string& text) {
        std::vector<uint8_t> codes;
        for (char c : text) codes.push_back((c >= '@' && c <= 'Z') ? (uint8_t)(c - '@') : (uint8_t)c);
        for (uint32_t a = 0x1B000; a < 0x1F000; a += 2) {
            bool ok = true;
            for (size_t k = 0; k < codes.size() && ok; k++) ok = vera->vramPeek(a + 2 * (uint32_t)k) == codes[k];
            if (ok) return true;
        }
        return false;
    }
};

std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

fs::path tempFolder(const char* tag) {
    fs::path d = fs::temp_directory_path() / (std::string("mmemu-x16-hostfs-") + tag + "-" + std::to_string(::getpid()));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

} // namespace

// The machine installs "x16.hostfs" on unit 8, and the CLI's settings work
TEST_CASE(x16_hostfs_installed_with_options) {
    std::string err;
    X16 x;
    if (!x.boot(err)) { printf("  (skipped: %s)\n", err.c_str()); return; }
    ASSERT(x.hostfs != nullptr);
    ASSERT_EQ(x.hostfs->unit(), 8);
    ASSERT(x.hostfs->setOption("unit", "9"));
    ASSERT_EQ(x.hostfs->unit(), 9);
    ASSERT(!x.hostfs->setOption("unit", "40"));
    ASSERT(x.hostfs->setOption("path", "/tmp"));
    ASSERT(x.hostfs->describe().find("unit 9: /tmp") != std::string::npos);
    ASSERT(!x.hostfs->setOption("nonsense", "1"));
}

// R49 with a host folder as unit 8: SAVE, NEW, LOAD and RUN, DOS"$", a DOS
// command (scratch), the error channel. Skipped without roms/x16/rom.bin.
TEST_CASE(x16_hostfs_save_load_dir_scratch) {
    std::string err;
    X16 x;
    if (!x.boot(err)) { printf("  (skipped: %s)\n", err.c_str()); return; }
    fs::path dir = tempFolder("basic");
    ASSERT(x.hostfs->setOption("path", dir.string()));
    ASSERT(x.runUntil("READY."));

    // SAVE a typed program: the host file is its load address and the program
    x.type("10 PRINT 6*7+100\nSAVE\"PROG\"\n");
    ASSERT(fs::exists(dir / "PROG"));
    std::vector<uint8_t> want = { 0x01, 0x08,                     // load address $0801
                                  0x0F, 0x08, 0x0A, 0x00,         // link $080F, line 10
                                  0x99, ' ', '6', 0xAC, '7', 0xAA, '1', '0', '0', 0x00,  // PRINT 6*7+100
                                  0x00, 0x00 };                   // end of program
    std::vector<uint8_t> got = readFile(dir / "PROG");
    ASSERT_EQ(got.size(), want.size());
    ASSERT(got == want);

    // NEW, LOAD it back, RUN
    x.type("NEW\n");
    x.type("LOAD\"PROG\"\n");
    ASSERT(!x.screenHas("NOT FOUND"));
    x.type("RUN\n");
    ASSERT(x.screenHas(" 142"));

    // A file the host put there, and the directory
    { std::ofstream f(dir / "HOSTFILE.TXT", std::ios::binary); f << "HELLO"; }
    x.type("DOS\"$\"\n");
    ASSERT(x.screenHas("\"PROG\""));
    ASSERT(x.screenHas("\"HOSTFILE.TXT\""));
    ASSERT(x.screenHas("BLOCKS FREE."));

    // A missing file: the error channel says so
    x.type("OPEN 1,8,2,\"MISSING\":CLOSE 1\n");
    x.type("DOS\n");
    ASSERT(x.screenHas("62, FILE NOT FOUND,00,00"));

    // Scratch
    x.type("DOS\"S:PROG\"\n");
    ASSERT(!fs::exists(dir / "PROG"));
    ASSERT(x.screenHas("01, FILES SCRATCHED,00,00"));

    // MD and CD: a subdirectory, then a SAVE into it
    x.type("DOS\"MD:SUB\"\n");
    ASSERT(fs::is_directory(dir / "SUB"));
    x.type("DOS\"CD:SUB\"\n");
    x.type("SAVE\"INNER\"\n");
    ASSERT(fs::exists(dir / "SUB" / "INNER"));
    x.type("DOS\"CD:..\"\n");
    // Rename (in the root again)
    x.type("DOS\"R:NEWNAME=HOSTFILE.TXT\"\n");
    ASSERT(fs::exists(dir / "NEWNAME"));
    ASSERT(!fs::exists(dir / "HOSTFILE.TXT"));

    fs::remove_all(dir);
}

// PRINT# and GET# through OPEN/CLOSE on a sequential file
TEST_CASE(x16_hostfs_open_print_get) {
    std::string err;
    X16 x;
    if (!x.boot(err)) { printf("  (skipped: %s)\n", err.c_str()); return; }
    fs::path dir = tempFolder("seq");
    ASSERT(x.hostfs->setOption("path", dir.string()));
    ASSERT(x.runUntil("READY."));
    x.type("OPEN 2,8,2,\"DATA,S,W\":PRINT#2,\"ABC\";:CLOSE 2\n");
    std::vector<uint8_t> got = readFile(dir / "DATA");
    ASSERT_EQ(got.size(), (size_t)3);
    ASSERT(got[0] == 'A' && got[1] == 'B' && got[2] == 'C');
    x.type("10 OPEN 2,8,2,\"DATA\":FOR I=1 TO 3:GET#2,A$:PRINT ASC(A$);:NEXT:CLOSE 2\nRUN\n");
    ASSERT(x.screenHas(" 65  66  67"));
    fs::remove_all(dir);
}

// An SD card attached: HostFS on unit 8 steps aside (the SD card serves unit 8)
TEST_CASE(x16_hostfs_steps_aside_for_sd_card) {
    std::string err;
    X16 x;
    if (!x.boot(err)) { printf("  (skipped: %s)\n", err.c_str()); return; }
    fs::path dir = tempFolder("sd");
    ASSERT(x.hostfs->setOption("path", dir.string()));
    // A blank image: attached, but no file system on it
    fs::path img = dir / "blank.img";
    { std::ofstream f(img, std::ios::binary); std::vector<char> z(1024 * 1024, 0); f.write(z.data(), z.size()); }
    ASSERT(x.runUntil("READY."));
    ASSERT(x.vera->mountDisk(8, img.string()));
    ASSERT(x.hostfs->describe().find("inactive") != std::string::npos);
    x.type("10 REM\nSAVE\"NOTHERE\"\n");
    ASSERT(!fs::exists(dir / "NOTHERE"));

    // Ejected: HostFS serves unit 8 again
    x.vera->ejectDisk(8);
    x.type("SAVE\"HERE\"\n");
    ASSERT(fs::exists(dir / "HERE"));
    fs::remove_all(dir);
}
