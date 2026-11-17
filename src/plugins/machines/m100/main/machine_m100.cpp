// The Tandy Model 100, 102 and 200, the NEC PC-8201A, the Olivetti M10 and
// the Kyotronic 85 as machines: ROMs, RAM, the option ROM socket, the
// frontends' hooks.

#include "m100_system.h"
#include "plugins/8085/main/cpu8085.h"
#include "plugins/devices/tpdd/main/tpdd.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "util/path_util.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>

namespace m100 {

namespace {

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

/** The first of `names` in `dir` whose size is in [minSize, maxSize]. */
bool loadOne(const std::string& dir, const std::vector<std::string>& names, size_t minSize, size_t maxSize,
             std::vector<uint8_t>& out) {
    for (const auto& n : names)
        if (readFile(dir + "/" + n, out) && out.size() >= minSize && out.size() <= maxSize) return true;
    out.clear();
    return false;
}

const char* romFile(Model m) {
    switch (m) {
    case Model::M100: return "m100.rom";
    case Model::M102: return "m102.rom";
    case Model::PC8201: return "pc8201a.rom";
    case Model::M10: return "m10.rom";
    case Model::KC85: return "kc85.rom";
    default: return "t200.rom";
    }
}

bool loadRom(Model model, std::vector<uint8_t>& rom, std::string& missing) {
    const std::string dir = firmwareDir();
    const std::string where = " in " + dir + "/ (or set MMEMU_M100_FIRMWARE to its directory)";
    switch (model) {
    case Model::M100:
        if (loadOne(dir, {"m100.rom", "m100rom.m12", "M100rom.bin", "m100rom.bin"}, 0x8000, 0x8000, rom)) return true;
        missing = "Model 100 ROM not found: put m100.rom (32 KB)" + where;
        return false;
    case Model::M102:
        if (loadOne(dir, {"m102.rom", "m102rom.m12", "M102rom.bin", "m102rom.bin"}, 0x8000, 0x8000, rom)) return true;
        missing = "Model 102 ROM not found: put m102.rom (32 KB)" + where;
        return false;
    case Model::M200:
        // VirtualT's layout: the 40 KB main ROM then the 32 KB second ROM
        if (loadOne(dir, {"t200.rom", "T200rom.bin", "t200rom.bin"}, 0x12000, 0x12000, rom)) return true;
        // MAME's region (0x18000: main 0-$9FFF, second at $10000)
        if (loadOne(dir, {"t200.rom", "tandy200.rom"}, 0x18000, 0x18000, rom)) {
            std::vector<uint8_t> r(rom.begin(), rom.begin() + 0xA000);
            r.insert(r.end(), rom.begin() + 0x10000, rom.end());
            rom = r;
            return true;
        }
        // MAME's three chips
        {
            std::vector<uint8_t> a, b, c;
            if (loadOne(dir, {"rom 1-1.m15"}, 0x8000, 0x8000, a) && loadOne(dir, {"rom 1-2.m13"}, 0x2000, 0x2000, b) &&
                loadOne(dir, {"rom 2.m14"}, 0x8000, 0x8000, c)) {
                rom = a;
                rom.insert(rom.end(), b.begin(), b.end());
                rom.insert(rom.end(), c.begin(), c.end());
                return true;
            }
        }
        missing = "Tandy 200 ROM not found: put t200.rom (72 KB; or MAME's \"rom 1-1.m15\", \"rom 1-2.m13\", \"rom 2.m14\")" + where;
        return false;
    case Model::PC8201:
        if (loadOne(dir, {"pc8201a.rom", "pc8201rom.rom", "PC8201rom.bin", "pc8201rom.bin"}, 0x8000, 0x8000, rom))
            return true;
        missing = "NEC PC-8201A ROM not found: put pc8201a.rom (32 KB)" + where;
        return false;
    case Model::M10:
        // VirtualT's M10rom.bin has 16 bytes past the 32 KB
        if (loadOne(dir, {"m10.rom", "m10rom.m12", "M10rom.bin", "m10rom.bin"}, 0x8000, 0x8010, rom)) {
            rom.resize(0x8000);
            return true;
        }
        missing = "Olivetti M10 ROM not found: put m10.rom (32 KB)" + where;
        return false;
    case Model::KC85:
        if (loadOne(dir, {"kc85.rom", "kc85rom.bin", "KC85rom.bin"}, 0x8000, 0x8000, rom)) return true;
        missing = "Kyotronic 85 ROM not found: put kc85.rom (32 KB)" + where;
        return false;
    }
    return false;
}

/** A tiny 8085 emitter for the stand-in ROM: bytes at an address, jumps by label. */
struct Emit {
    std::vector<uint8_t>& b;
    uint16_t pc;
    void op(std::initializer_list<int> v) { for (int x : v) b[pc++] = (uint8_t)x; }
    void op16(int o, uint16_t a) { op({o, a & 0xFF, a >> 8}); }
};

/** `lines` drawn in the 5x7 font into a w x h panel of dots (6 dots a character). */
std::vector<uint8_t> drawText(const std::vector<std::string>& lines, int w, int h) {
    std::vector<uint8_t> dots((size_t)w * h, 0);
    for (size_t l = 0; l < lines.size(); ++l)
        for (size_t i = 0; i < lines[l].size() && (int)(i * 6 + 6) <= w; ++i) {
            const int c = std::toupper((unsigned char)lines[l][i]);
            if (c < 0x20 || c >= 0x80) continue;
            for (int y = 0; y < 7; ++y)
                for (int x = 0; x < 5; ++x)
                    if ((Hd61830::FONT_5X7[c - 0x20][y] >> x) & 1) {
                        const int py = 4 + (int)l * 10 + y, px = (int)i * 6 + x;
                        if (py < h) dots[(size_t)py * w + px] = 1;
                    }
        }
    return dots;
}

std::vector<std::string> wrap(const std::string& s, size_t width) {
    std::vector<std::string> out;
    std::string line;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        const std::string word = s.substr(i, j - i);
        if (!line.empty() && line.size() + 1 + word.size() > width) { out.push_back(line); line.clear(); }
        line += (line.empty() ? "" : " ") + word;
        i = j + 1;
    }
    if (!line.empty()) out.push_back(line);
    return out;
}

/**
 * The add-ons' expansion slots (issue #358): `sysbus` (the system bus
 * connector: a RAMPAC / Node Datapac) and `rex` (a REX / REX# flash in the
 * option ROM socket). Each takes the host file that holds its memory as
 * the option (`expansion sysbus rampac FILE`, `expansion rex rex FILE`).
 */
// The Disk/Video Interface's two ROMs: the boot ROM (M40; its first 1 KB is
// used) and the character generator (M17; its first 2 KB), the user's files
bool loadDviRoms(std::vector<uint8_t>& rom, std::vector<uint8_t>& cg, std::string& err) {
    const std::string dir = firmwareDir();
    if (!loadOne(dir, {"dvi.rom", "M40_P.bin", "m40_p.bin", "M40 P.bin"}, 0x400, 0x1000, rom)) {
        err = "Disk/Video Interface boot ROM not found: put dvi.rom (the M40 P-ROM, 1-4 KB) in " + dir + "/";
        return false;
    }
    if (!loadOne(dir, {"dvichar.rom", "M17_CG.bin", "m17_cg.bin", "M17 CG.bin"}, 0x800, 0x1000, cg)) {
        err = "Disk/Video Interface character ROM not found: put dvichar.rom (the M17 P-ROM, 2-4 KB) in " + dir + "/";
        return false;
    }
    return true;
}

bool fitDvi(System* sys, std::string& err) {
    if (sys->dvi()) return true;
    std::vector<uint8_t> rom, cg;
    return loadDviRoms(rom, cg, err) && sys->fitDvi(std::move(rom), std::move(cg), err);
}

/**
 * The Disk/Video Interface's drives as the frontends' disk units 1 and 2
 * (drives 0 and 1). Mounting a disk fits the DVI when it isn't there.
 */
class DviDrives : public IOHandler {
public:
    explicit DviDrives(System* s) : m_sys(s) {}
    const char* name() const override { return "DVI drives"; }
    uint32_t baseAddr() const override { return 0x80; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    bool mountDisk(int unit, const std::string& path) override {
        if (unit < 1 || unit > 2) return false;
        std::string err;
        if (!fitDvi(m_sys, err)) return false;
        return m_sys->dvi()->mountDisk(unit - 1, path, &err);
    }
    void ejectDisk(int unit) override {
        if (m_sys->dvi() && unit >= 1 && unit <= 2) m_sys->dvi()->ejectDisk(unit - 1);
    }
    std::string getMountedDiskPath(int unit) override {
        return (m_sys->dvi() && unit >= 1 && unit <= 2) ? m_sys->dvi()->diskPath(unit - 1) : "";
    }
    bool getDiskStatus(int unit, int& track, int& sector, bool& led) override {
        if (!m_sys->dvi() || unit < 1 || unit > 2) return false;
        Wd17xx& f = m_sys->dvi()->fdc();
        return f.getDiskStatus(unit - 1, track, sector, led);
    }

private:
    System* m_sys;
};

void addAddonSlots(MachineDescriptor* desc, System* sys) {
    {
        MachineDescriptor::ExpansionSlot s;
        s.name = "sysbus";
        s.description = "The 40-pin system bus: a RAMPAC / Node Datapac (256 KB battery-backed RAM disk at "
                        "ports $81/$83; the option is the host file that holds it), or the Disk/Video "
                        "Interface (ports $80-$83; the option is a disk image for drive 0)";
        s.devices = {{"rampac", "RAMPAC: 256 KB RAM disk, saved to the file given", {}, ""},
                     {"datapac", "Node Datapac (as the RAMPAC)", {}, ""},
                     {"dvi", "Disk/Video Interface (26-3806): 5.25\" drives and a 40/80-column monitor", {}, ""}};
        s.install = [sys](const std::string& device, const std::string& option, std::string& err) {
            if (device.empty()) { sys->removeRampac(); sys->removeDvi(); return true; }
            if (device == "dvi") {
                sys->removeDvi();
                if (!fitDvi(sys, err)) return false;
                if (!option.empty() && !sys->dvi()->mountDisk(0, option, &err)) {
                    if (err.empty()) err = "can't read the disk image '" + option + "'";
                    return false;
                }
                return true;
            }
            if (device != "rampac" && device != "datapac") {
                err = "the sysbus slot takes rampac, datapac or dvi";
                return false;
            }
            sys->removeDvi();
            return sys->fitRampac(option, err);
        };
        s.info = [sys]() -> std::string {
            if (Dvi* d = sys->dvi()) {
                const std::string p0 = d->diskPath(0), p1 = d->diskPath(1);
                return "dvi (drive 0: " + (p0.empty() ? std::string("empty") : p0) +
                       (p1.empty() ? std::string("") : ", drive 1: " + p1) + ")";
            }
            if (!sys->rampac()) return "";
            const std::string& p = sys->rampac()->path();
            return "rampac (256K, " + (p.empty() ? std::string("not saved") : p) + ")";
        };
        s.save = [sys](const std::string& path, std::string& err) {
            if (!sys->rampac()) { err = "no RAMPAC fitted"; return false; }
            if (!sys->rampac()->saveTo(path)) { err = "can't write '" + path + "'"; return false; }
            return true;
        };
        desc->expansionSlots.push_back(std::move(s));
    }
    {
        MachineDescriptor::ExpansionSlot s;
        s.name = "rex";
        s.description = "REX / REX# (1 MB flash) or the REXCPM (4 MB RAM, Model 100/102) in the option ROM "
                        "socket, replacing any option ROM while fitted (the REXCPM also takes the RAM's "
                        "place); the option is the host image file (written back as it changes)";
        s.devices = {{"rex", "REX (Ken Pettit's)", {}, ""}, {"rexsharp", "REX# (Steve Adolph's)", {}, ""},
                     {"rexcpm", "REXCPM (Steve Adolph's): 4 MB RAM, RAM and option ROM windows", {}, ""}};
        s.install = [sys](const std::string& device, const std::string& option, std::string& err) {
            if (device.empty()) { sys->removeRex(); sys->removeRexCpm(); return true; }
            if (device == "rexcpm") return sys->fitRexCpm(option, err);
            if (device != "rex" && device != "rexsharp" && device != "rex#") {
                err = "the rex slot takes rex, rexsharp or rexcpm";
                return false;
            }
            return sys->fitRex(device == "rex" ? Rex::Kind::Rex : Rex::Kind::RexSharp, option, err);
        };
        s.info = [sys]() -> std::string {
            if (RexCpm* c = sys->rexCpm())
                return "rexcpm (" + (c->path().empty() ? std::string("not saved") : c->path()) + ")";
            if (!sys->rex()) return "";
            const std::string& p = sys->rex()->path();
            return std::string(sys->rex()->kind() == Rex::Kind::Rex ? "rex" : "rexsharp") + " (" +
                   (p.empty() ? std::string("not saved") : p) + ")";
        };
        s.save = [sys](const std::string& path, std::string& err) {
            if (RexCpm* c = sys->rexCpm()) {
                if (!c->saveTo(path)) { err = "can't write '" + path + "'"; return false; }
                return true;
            }
            if (!sys->rex()) { err = "no REX fitted"; return false; }
            if (!sys->rex()->saveTo(path)) { err = "can't write '" + path + "'"; return false; }
            return true;
        };
        desc->expansionSlots.push_back(std::move(s));
    }
}

}  // namespace

std::string firmwareDir() {
    const char* v = std::getenv("MMEMU_M100_FIRMWARE");
    return v ? v : PathUtil::findResource("roms/m100");
}

std::vector<uint8_t> placeholderRom(Model model, const std::string& message) {
    const bool t200 = model == Model::M200;
    std::vector<uint8_t> r(t200 ? 0x12000 : 0x8000, 0xFF);
    const int h = t200 ? 128 : 64;
    const auto dots = drawText(wrap(message, 40), 240, h);
    Emit e{r, 0};
    e.op16(0xC3, 0x0040);                              // JMP start
    e.pc = 0x24;
    e.op({0xF3, 0x76});                                // TRAP: DI, HLT
    e.pc = 0x40;
    e.op({0xF3});                                      // DI
    e.op16(0x31, 0x0000);                              // LXI SP,0 (the top of RAM)
    const uint16_t table = 0x0100;
    if (!t200) {
        // Ten HD44102s: [port A, port B, 4 pages x 50 columns] per chip
        uint16_t p = table;
        for (int chip = 0; chip < 10; ++chip) {
            const uint16_t sel = (uint16_t)(1 << chip);
            r[p++] = (uint8_t)(sel & 0xFF);
            r[p++] = (uint8_t)(sel >> 8);
            for (int page = 0; page < 4; ++page)
                for (int col = 0; col < 50; ++col) {
                    uint8_t b = 0;
                    const int x = M100Lcd::chipX(chip) + col;
                    for (int bit = 0; bit < 8; ++bit) {
                        const int y = M100Lcd::chipY(chip) + page * 8 + bit;
                        if (x < 240 && dots[(size_t)y * 240 + x]) b |= (uint8_t)(1 << bit);
                    }
                    r[p++] = b;
                }
        }
        e.op({0x3E, 0x03, 0xD3, 0xB0});                // MVI A,$03; OUT $B0: ports A and B out
        e.op16(0x21, table);                           // LXI H,table
        e.op({0x0E, 10});                              // MVI C,10
        const uint16_t chipLoop = e.pc;
        e.op({0x7E, 0x23, 0xD3, 0xB1});                // MOV A,M; INX H; OUT $B1
        e.op({0x7E, 0x23, 0xD3, 0xB2});                // MOV A,M; INX H; OUT $B2
        e.op({0x3E, 0x39, 0xD3, 0xF0});                // display on
        e.op({0x3E, 0x3B, 0xD3, 0xF0});                // count up
        e.op({0x3E, 0x3E, 0xD3, 0xF0});                // display start page 0
        e.op({0x1E, 0x00});                            // MVI E,0 (page << 6)
        const uint16_t pageLoop = e.pc;
        e.op({0x7B, 0xD3, 0xF0});                      // MOV A,E; OUT $F0: page, column 0
        e.op({0x06, 50});                              // MVI B,50
        const uint16_t byteLoop = e.pc;
        e.op({0x7E, 0x23, 0xD3, 0xF1, 0x05});          // MOV A,M; INX H; OUT $F1; DCR B
        e.op16(0xC2, byteLoop);                        // JNZ
        e.op({0x7B, 0xC6, 0x40, 0x5F});                // MOV A,E; ADI $40; MOV E,A
        e.op16(0xD2, pageLoop);                        // JNC
        e.op({0x0D});                                  // DCR C
        e.op16(0xC2, chipLoop);                        // JNZ
    } else {
        // The HD61830 in graphic mode, 40 bytes of 6 dots x 128 lines
        uint16_t p = table;
        for (int y = 0; y < 128; ++y)
            for (int c = 0; c < 40; ++c) {
                uint8_t b = 0;
                for (int bit = 0; bit < 6; ++bit)
                    if (dots[(size_t)y * 240 + c * 6 + bit]) b |= (uint8_t)(1 << bit);
                r[p++] = b;
            }
        auto reg = [&](int ir, int v) {
            e.op({0x3E, ir, 0xD3, 0xF1, 0x3E, v, 0xD3, 0xF0});   // MVI A,ir; OUT $F1; MVI A,v; OUT $F0
        };
        reg(0x00, 0x32);                               // mode: display on, master, graphic
        reg(0x01, 0x75);                               // 8 lines, 6 dots
        reg(0x02, 39);                                 // 40 bytes a line
        reg(0x03, 63);                                 // 64 time divisions
        reg(0x08, 0); reg(0x09, 0);                    // display start 0
        reg(0x0A, 0); reg(0x0B, 0);                    // cursor 0
        e.op({0x3E, 0x0C, 0xD3, 0xF1});                // write display data
        e.op16(0x21, table);                           // LXI H,table
        e.op16(0x11, 128 * 40);                        // LXI D,5120
        const uint16_t loop = e.pc;
        e.op({0x7E, 0xD3, 0xF0, 0x23, 0x1B, 0x7A, 0xB3});   // MOV A,M; OUT $F0; INX H; DCX D; MOV A,D; ORA E
        e.op16(0xC2, loop);                            // JNZ
    }
    e.op({0xF3, 0x76});                                // DI; HLT
    return r;
}

bool insertOptionRom(MachineDescriptor* m, const std::vector<uint8_t>& image, const std::string& label,
                     std::string& err) {
    System* sys = systemOf(m);
    if (!sys) { err = "not a Model 100 or one of its siblings"; return false; }
    if (image.size() > 0x8000) { err = "an option ROM is at most 32 KB"; return false; }
    sys->setOptionRom(image, label);
    return true;
}

MachineDescriptor* create(Model model, std::string* err, const Roms* roms, int ramKb) {
    Tpdd::registerSchemes();
    std::vector<uint8_t> rom, opt;
    std::string missing;
    if (roms) {
        rom = roms->rom;
        opt = roms->optRom;
        if (rom.empty()) { if (err) *err = "no ROM image"; return nullptr; }
    } else if (!loadRom(model, rom, missing)) {
        if (err) *err = missing;
        std::fprintf(stderr, "%s\n", missing.c_str());
        rom = placeholderRom(model, std::string("NO ROM: PUT ") + romFile(model) +
                                        " IN ROMS/M100/ (SEE DOC/README-M100.MD)");
    }

    auto* sys = new System(model, std::move(rom));
    sys->setRamKb(ramKb > 0 ? ramKb : model == Model::M200 ? 72 : model == Model::PC8201 ? 96 : 32);
    if (!opt.empty()) sys->setOptionRom(opt, "option ROM");

    auto* desc = new MachineDescriptor();
    switch (model) {
    case Model::M100: desc->machineId = "model100"; desc->displayName = "Tandy Model 100"; break;
    case Model::M102: desc->machineId = "model102"; desc->displayName = "Tandy Model 102"; break;
    case Model::M200: desc->machineId = "model200"; desc->displayName = "Tandy 200"; break;
    case Model::PC8201: desc->machineId = "pc8201a"; desc->displayName = "NEC PC-8201A"; break;
    case Model::M10: desc->machineId = "m10"; desc->displayName = "Olivetti M10"; break;
    case Model::KC85: desc->machineId = "kc85"; desc->displayName = "Kyocera Kyotronic 85"; break;
    }
    desc->licenseClass = "proprietary";
    desc->uiPanesSet = true;
    desc->uiPanes = {"tape"};
    desc->clockHz = CPU_HZ;
    if (!missing.empty()) desc->warnings.push_back(missing);

    auto* cpu = new I8085();
    cpu->setDataBus(sys);
    cpu->setIoBus(sys->ioBus());
    sys->setCpu(cpu);
    desc->buses.push_back({"system", sys});
    desc->buses.push_back({"io", sys->ioBus()});
    desc->cpus.push_back({"main", cpu, sys, sys, sys->ioBus(), true, 1});

    auto* io = new IORegistry();
    desc->ioRegistry = io;
    io->registerHandler(&sys->display);
    io->registerHandler(&sys->keyboard);
    io->registerHandler(&sys->sound);
    io->registerHandler(&sys->tape);
    io->registerHandler(&sys->printer);       // before the RS-232 port: it takes `printer:`
    if (sys->hasModem()) io->registerHandler(&sys->modemPort);    // `modem:SPEC`
    io->registerHandler(&sys->barcodePort);   // `barcode:...`
    io->registerHandler(&sys->serial);
    if (!sys->isPc8201() && model != Model::M10 && model != Model::KC85) {
        auto* drives = new DviDrives(sys);
        io->registerHandler(drives);
        desc->deleters.push_back([drives]() { delete drives; });
    }

    desc->onKey = [sys](const std::string& name, bool down) {
        if (name == "reset") { if (down) sys->resetButton(); return true; }
        if (name == "power") { if (down) sys->powerSwitch(); return true; }
        return sys->keyboard.key(name, down);
    };
    desc->screenButtons = {
        {"POWER", {"power"}, "The power switch: off raises TRAP (the ROM saves its state and powers down); "
                             "on restarts with RAM kept. Also how to wake it after an automatic power-off."},
        {"RESET", {"reset"}, "The reset button on the back: the 8085 restarts, RAM kept (the ROM's warm start)."}};

    // RAM
    {
        MachineDescriptor::ExpansionSlot s;
        s.name = "ram";
        std::vector<std::string> sizes;
        std::string def;
        switch (model) {
        case Model::M100: sizes = {"8K", "16K", "24K", "32K"}; def = "32K";
            s.description = "RAM: 8K to 32K in 8K modules, ending at $FFFF"; break;
        case Model::M102: sizes = {"24K", "32K"}; def = "32K"; s.description = "RAM: 24K or 32K, ending at $FFFF"; break;
        case Model::M200: sizes = {"24K", "48K", "72K"}; def = "72K";
            s.description = "RAM: one, two or three 24K banks at $A000-$FFFF (port $D0 bits 2-3)"; break;
        case Model::PC8201: sizes = {"16K", "32K", "64K", "96K"}; def = "96K";
            s.description = "RAM: 16K or 32K inside, ending at $FFFF; 64K and 96K add 32K banks 2 and 3 "
                            "(port $A0: bits 0-1 for $0000-$7FFF, 2-3 for $8000-$FFFF)"; break;
        case Model::M10:
        case Model::KC85: sizes = {"8K", "16K", "24K", "32K"}; def = "32K";
            s.description = "RAM: 8K to 32K in 8K modules, ending at $FFFF"; break;
        }
        s.devices = {{"ram", "The RAM fitted", sizes, def}};
        s.canEmpty = false;
        s.install = [sys, sizes, def](const std::string& device, const std::string& option, std::string& err) {
            if (device != "ram") { err = "the ram slot takes 'ram'"; return false; }
            const uint64_t kb = MachineDescriptor::parseMemorySize(option.empty() ? def : option) / 1024;
            bool ok = false;
            for (const auto& z : sizes) ok |= MachineDescriptor::parseMemorySize(z) / 1024 == kb;
            if (!ok) { err = "sizes: "; for (const auto& z : sizes) err += z + " "; return false; }
            sys->setRamKb((int)kb);
            sys->powerOn();
            return true;
        };
        s.info = [sys]() { return "ram (" + std::to_string(sys->ramKb()) + "K)"; };
        desc->expansionSlots.push_back(std::move(s));
    }

    desc->romSockets.push_back({"optrom",
        model == Model::M200
            ? "The option ROM socket: up to 32 KB, at $0000-$7FFF when port $D0 selects ROM bank 2"
        : model == Model::PC8201
            ? "The option ROM socket: up to 32 KB, at $0000-$7FFF when port $A0 selects ROM bank 1"
            : "The option ROM socket: up to 32 KB, at $0000-$7FFF when port $E0 bit 0 is set "
              "(Multiplan, T-Base, Ultimate ROM II, ...; CALL 63012 in BASIC)",
        [desc](const std::vector<uint8_t>& image, const std::string& label, std::string& e) {
            return insertOptionRom(desc, image, label, e);
        },
        [sys]() { return sys->optionRomLabel(); }});
    addAddonSlots(desc, sys);

    desc->symbolFiles.push_back(PathUtil::findResource("roms/m100/hardware.sym"));
    desc->onReset = [sys](MachineDescriptor&) { sys->resetButton(); };
    desc->schedulerStep = [sys](MachineDescriptor&) -> int { return sys->step(); };
    desc->deviceCycles = [sys]() { return sys->ticks(); };
    sys->powerOn();
    return desc;
}

}  // namespace m100
