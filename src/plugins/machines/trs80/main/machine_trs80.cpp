// The TRS-80 Model I, III, 4 and 4P as machines: ROMs, what is fitted
// (expansion slots), the frontends' hooks and /CMD programs.

#include "trs80_system.h"
#include "plugins/z80/main/cpu_z80.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "util/path_util.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>

namespace trs80 {

namespace {

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

/** The first file of `names` in `dir` whose size is in [minSize, maxSize]. */
bool loadOne(const std::string& dir, const std::vector<std::string>& names, size_t minSize, size_t maxSize,
             std::vector<uint8_t>& out) {
    for (const auto& n : names)
        if (readFile(dir + "/" + n, out) && out.size() >= minSize && out.size() <= maxSize) return true;
    out.clear();
    return false;
}

/** A ROM in parts (MAME's chip dumps), each at its offset. */
bool loadParts(const std::string& dir, const std::vector<std::pair<std::vector<std::string>, size_t>>& parts,
               std::vector<uint8_t>& out) {
    out.clear();
    for (const auto& [names, size] : parts) {
        std::vector<uint8_t> p;
        if (!loadOne(dir, names, size, size, p)) { out.clear(); return false; }
        out.insert(out.end(), p.begin(), p.end());
    }
    return true;
}

bool loadRom(Model model, std::vector<uint8_t>& rom, std::string& missing) {
    const std::string dir = firmwareDir();
    const std::string where = " in " + dir + "/ (or set MMEMU_TRS80_FIRMWARE to its directory)";
    switch (model) {
    case Model::M1Level1:
        if (loadOne(dir, {"level1.rom", "trs80l1.rom"}, 0x1000, 0x1000, rom)) return true;
        missing = "TRS-80 Level I ROM not found: put level1.rom (4 KB)" + where;
        return false;
    case Model::M1Level2:
        if (loadOne(dir, {"level2.rom", "trs80l2.rom"}, 0x3000, 0x3800, rom)) return true;
        if (loadParts(dir, {{{"rom-a.z1"}, 0x1000}, {{"rom-b.z2"}, 0x1000}, {{"rom-c.z3"}, 0x1000}}, rom)) return true;
        missing = "TRS-80 Model I Level II ROM not found: put level2.rom (12 KB; or rom-a.z1, rom-b.z2, rom-c.z3)" + where;
        return false;
    case Model::M4:
        if (loadOne(dir, {"model4.rom", "trs80m4.rom"}, 0x3000, 0x3800, rom)) return true;
        [[fallthrough]];
    case Model::M3:
        if (loadOne(dir, {"model3.rom", "trs80m3.rom"}, 0x3000, 0x3800, rom)) return true;
        if (loadParts(dir, {{{"8041364.u104"}, 0x2000}, {{"8040332.u105"}, 0x1000},
                            {{"8040316c.u106", "8040316b.u106"}, 0x0800}}, rom))
            return true;
        missing = model == Model::M4
            ? "TRS-80 Model 4 ROM not found: put model4.rom (14 KB; or a Model III's model3.rom)" + where
            : "TRS-80 Model III ROM not found: put model3.rom (14 KB; or 8041364.u104, 8040332.u105, 8040316c.u106)" + where;
        return false;
    case Model::M4P:
        if (loadOne(dir, {"model4p.rom", "8075332.u69"}, 0x0800, 0x1000, rom)) return true;
        missing = "TRS-80 Model 4P boot ROM not found: put model4p.rom (4 KB)" + where;
        return false;
    }
    return false;
}

void loadCharGen(Model model, std::vector<uint8_t>& gen) {
    const std::string dir = firmwareDir();
    if (isModel1(model)) loadOne(dir, {"mcm6670p.z29", "trs80m1.chr"}, 0x400, 0x800, gen);
    else if (model == Model::M4P) loadOne(dir, {"8049007.u103", "8044316a.u36"}, 0x800, 0x800, gen);
    else loadOne(dir, {"8044316a.u36", "trs80m3.chr"}, 0x800, 0x800, gen);
}

// Without a ROM: a stand-in that says so on the screen and waits
std::vector<uint8_t> placeholderRom(Model model, const std::string& file) {
    std::vector<uint8_t> r(model == Model::M1Level1 || model == Model::M4P ? 0x1000 : 0x3000, 0xFF);
    const uint8_t code[] = {
        0xF3,                       // DI
        0x31, 0x00, 0x4F,           // LD SP,$4F00
        0x21, 0x00, 0x3C,           // LD HL,$3C00
        0x11, 0x01, 0x3C,           // LD DE,$3C01
        0x01, 0xFF, 0x03,           // LD BC,$03FF
        0x36, 0x20,                 // LD (HL),' '
        0xED, 0xB0,                 // LDIR
        0x21, 0x00, 0x01,           // LD HL,message
        0x11, 0x00, 0x3C,           // LD DE,$3C00
        0x7E,                       // loop: LD A,(HL)
        0xB7,                       // OR A
        0x28, 0x05,                 // JR Z,done
        0x12,                       // LD (DE),A
        0x23,                       // INC HL
        0x13,                       // INC DE
        0x18, 0xF7,                 // JR loop
        0x18, 0xFE,                 // done: JR done
    };
    std::memcpy(r.data(), code, sizeof code);
    std::string msg = "NO ROM: PUT " + file + " IN ROMS/TRS80/ (SEE DOC/README-TRS80.MD)";
    for (auto& c : msg) c = (char)std::toupper((unsigned char)c);
    for (size_t i = 0; i < msg.size() && 0x100 + i < r.size() - 1; ++i) r[0x100 + i] = (uint8_t)msg[i];
    r[0x100 + msg.size()] = 0;
    return r;
}

Config defaultConfig(Model model) {
    Config c;
    switch (model) {
    case Model::M1Level1: c.ramKb = 4; c.expansion = false; break;
    case Model::M1Level2: c.ramKb = 48; c.expansion = true; break;
    case Model::M3: c.ramKb = 48; c.rs232 = true; break;
    case Model::M4: case Model::M4P: c.ramKb = 128; c.rs232 = true; break;
    }
    // The hard disk interface answers only with a drive mounted (as xtrs)
    c.hardDisk = model != Model::M1Level1;
    return c;
}

/** An expansion slot holding one optional device that is a Config flag. */
MachineDescriptor::ExpansionSlot flagSlot(System* sys, const std::string& name, const std::string& desc,
                                          const std::string& dev, const std::string& devDesc, bool Config::*flag,
                                          bool needsEi = false) {
    MachineDescriptor::ExpansionSlot s;
    s.name = name;
    s.description = desc;
    s.devices = {{dev, devDesc, {}, ""}};
    s.install = [sys, dev, flag, needsEi](const std::string& device, const std::string&, std::string& err) {
        if (!device.empty() && device != dev) { err = "the slot takes '" + dev + "'"; return false; }
        if (!device.empty() && needsEi && !sys->cfg().expansion) { err = "it needs the Expansion Interface (expansion ei ei)"; return false; }
        Config c = sys->cfg();
        c.*flag = !device.empty();
        sys->configure(c);
        return true;
    };
    s.info = [sys, dev, flag]() { return sys->cfg().*flag ? dev : std::string(); };
    return s;
}

// The Model I RS-232 board's sense switches (xtrs's trs_uart.h): bits 2-0
// the baud rate, 3 no parity, 4 two stop bits, 6-5 the word length, 7 even
const double BAUDS[8] = {110, 150, 300, 600, 1200, 2400, 4800, 9600};

std::string describeSwitches(uint8_t v) {
    static const int WORD[4] = {5, 7, 6, 8};
    char b[32];
    std::snprintf(b, sizeof b, "%g-%d%c%d", BAUDS[v & 7], WORD[(v >> 5) & 3],
                  (v & 0x08) ? 'N' : (v & 0x80) ? 'E' : 'O', (v & 0x10) ? 2 : 1);
    return b;
}

bool parseSwitches(std::string s, int& out) {
    if (!s.empty() && (s[0] == '$' || (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')))) {
        char* end = nullptr;
        const long v = std::strtol(s.c_str() + (s[0] == '$' ? 1 : 2), &end, 16);
        if (!end || *end || v < 0 || v > 255) return false;
        out = (int)v;
        return true;
    }
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    const auto dash = s.find('-');
    if (dash == std::string::npos || s.size() != dash + 4) return false;
    const double baud = std::atof(s.substr(0, dash).c_str());
    int b = -1;
    for (int i = 0; i < 8; ++i) if (BAUDS[i] == baud) b = i;
    const char w = s[dash + 1], par = s[dash + 2], stop = s[dash + 3];
    static const char WORDS[4] = {'5', '7', '6', '8'};
    int wi = -1;
    for (int i = 0; i < 4; ++i) if (WORDS[i] == w) wi = i;
    if (b < 0 || wi < 0 || (par != 'N' && par != 'E' && par != 'O') || (stop != '1' && stop != '2')) return false;
    out = b | (par == 'N' ? 0x08 : 0) | (stop == '2' ? 0x10 : 0) | (wi << 5) | (par == 'E' ? 0x80 : 0);
    return true;
}

}  // namespace

std::string firmwareDir() {
    const char* v = std::getenv("MMEMU_TRS80_FIRMWARE");
    return v ? v : PathUtil::findResource("roms/trs80");
}

MachineDescriptor* create(Model model, std::string* err, const Roms* roms) {
    std::vector<uint8_t> rom, gen;
    std::string missing;
    if (roms) {
        rom = roms->rom;
        gen = roms->charGen;
        if (rom.empty()) { if (err) *err = "no ROM image"; return nullptr; }
    } else {
        if (!loadRom(model, rom, missing)) {
            if (err) *err = missing;
            std::fprintf(stderr, "%s\n", missing.c_str());
            const char* file = model == Model::M1Level1 ? "level1.rom" : model == Model::M1Level2 ? "level2.rom"
                             : model == Model::M3 ? "model3.rom" : model == Model::M4 ? "model4.rom" : "model4p.rom";
            rom = placeholderRom(model, file);
        }
        loadCharGen(model, gen);
    }

    auto* sys = new System(model, defaultConfig(model), std::move(rom));
    if (!gen.empty()) sys->setCharGen(gen);
    if (isModel1(model) && !roms) {
        // The Exatron Stringy Floppy's ROM (optional; at $3000-$37DF)
        std::vector<uint8_t> esf;
        if (loadOne(firmwareDir(), {"esf.rom", "esfrom.bin"}, 0x100, 0x7E0, esf)) sys->setStringyRom(esf);
    }

    auto* desc = new MachineDescriptor();
    switch (model) {
    case Model::M1Level1: desc->machineId = "trs80m1l1"; desc->displayName = "TRS-80 Model I (Level I BASIC)"; break;
    case Model::M1Level2: desc->machineId = "trs80m1"; desc->displayName = "TRS-80 Model I (Level II BASIC)"; break;
    case Model::M3: desc->machineId = "trs80m3"; desc->displayName = "TRS-80 Model III"; break;
    case Model::M4: desc->machineId = "trs80m4"; desc->displayName = "TRS-80 Model 4"; break;
    case Model::M4P: desc->machineId = "trs80m4p"; desc->displayName = "TRS-80 Model 4P"; break;
    }
    desc->licenseClass = "proprietary";
    desc->uiPanesSet = true;
    desc->uiPanes = model == Model::M1Level1 ? std::vector<std::string>{"tape"}
                  : model == Model::M4P ? std::vector<std::string>{"drive"}
                  : std::vector<std::string>{"tape", "drive"};
    desc->clockHz = sys->masterHz() / sys->baseTicks();
    if (!missing.empty()) desc->warnings.push_back(missing);

    auto* cpu = new Z80();
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
    if (model == Model::M1Level2) io->registerHandler(&sys->tape2);
    io->registerHandler(&sys->uart);
    io->registerHandler(&sys->printer);
    io->registerHandler(&sys->disks);
    io->registerHandler(&sys->hard);
    if (isModel1(model)) io->registerHandler(&sys->stringy);

    desc->onKey = [sys](const std::string& name, bool down) {
        if (name == "reset") { if (down) sys->resetButton(); return true; }
        return sys->keyboard.key(name, down);
    };
    desc->screenButtons = {{"RESET", {"reset"}, isModel1(model)
        ? "The RESET button (an NMI): Level II restarts; memory stays (hold BREAK to skip the disk boot)."
        : "The RESET button: the Z80 restarts; memory stays (hold BREAK to skip the disk boot)."}};

    // RAM
    {
        MachineDescriptor::ExpansionSlot s;
        s.name = "ram";
        std::vector<std::string> sizes;
        std::string def;
        switch (model) {
        case Model::M1Level1: sizes = {"4K", "16K"}; def = "4K";
            s.description = "RAM in the keyboard unit: 4K or 16K"; break;
        case Model::M1Level2: sizes = {"4K", "16K", "32K", "48K"}; def = "48K";
            s.description = "RAM: 4K or 16K in the keyboard unit, 32K or 48K with the Expansion Interface's"; break;
        case Model::M3: sizes = {"16K", "32K", "48K"}; def = "48K"; s.description = "RAM: 16K, 32K or 48K"; break;
        default: sizes = {"64K", "128K"}; def = "128K"; s.description = "RAM: 64K, or 128K (port $84's banks)"; break;
        }
        s.devices = {{"ram", "The RAM fitted", sizes, def}};
        s.canEmpty = false;
        s.install = [sys, sizes, def](const std::string& device, const std::string& option, std::string& err) {
            if (device != "ram") { err = "the ram slot takes 'ram'"; return false; }
            const uint64_t kb = MachineDescriptor::parseMemorySize(option.empty() ? def : option) / 1024;
            bool ok = false;
            for (const auto& z : sizes) ok |= MachineDescriptor::parseMemorySize(z) / 1024 == kb;
            if (!ok) { err = "sizes: "; for (const auto& z : sizes) err += z + " "; return false; }
            if (isModel1(sys->model()) && kb > 16 && !sys->cfg().expansion) { err = "over 16K needs the Expansion Interface"; return false; }
            Config c = sys->cfg();
            c.ramKb = (int)kb;
            sys->configure(c);
            return true;
        };
        s.info = [sys]() { return "ram (" + std::to_string(sys->cfg().ramKb) + "K)"; };
        desc->expansionSlots.push_back(std::move(s));
    }
    if (model == Model::M1Level2) {
        desc->expansionSlots.push_back(flagSlot(sys, "ei", "The Expansion Interface (26-1140)", "ei",
            "RAM to 48K, the FD1771 floppy controller ($37EC), printer port ($37E8), 40 Hz clock interrupt",
            &Config::expansion));
        {
            MachineDescriptor::ExpansionSlot s;
            s.name = "doubler";
            s.description = "A double-density adapter in the Expansion Interface";
            s.devices = {
                {"percom", "Percom Doubler: command $FE selects the FD1771, $FF its double-density 1791", {}, ""},
                {"tandy", "Radio Shack doubler (26-1143): sector register $80 double density, $A0 single", {}, ""},
                {"both", "Answers both ways (as xtrs does)", {}, ""},
            };
            s.install = [sys](const std::string& device, const std::string&, std::string& err) {
                Config c = sys->cfg();
                if (device.empty()) c.doubler = Doubler::None;
                else if (!c.expansion) { err = "the doubler goes in the Expansion Interface (expansion ei ei)"; return false; }
                else if (device == "percom") c.doubler = Doubler::Percom;
                else if (device == "tandy") c.doubler = Doubler::Tandy;
                else if (device == "both") c.doubler = Doubler::Both;
                else { err = "percom, tandy or both"; return false; }
                sys->configure(c);
                return true;
            };
            s.info = [sys]() -> std::string {
                switch (sys->cfg().doubler) {
                case Doubler::Percom: return "percom";
                case Doubler::Tandy: return "tandy";
                case Doubler::Both: return "both";
                default: return "";
                }
            };
            desc->expansionSlots.push_back(std::move(s));
        }
    }
    if (isModel1(model))
        desc->expansionSlots.push_back(flagSlot(sys, "lowercase", "The lower case modification", "lowercase",
            "Video RAM's D6 fitted: lower case shown (and stored) as written", &Config::lowercase));
    if (model == Model::M3)
        desc->expansionSlots.push_back(flagSlot(sys, "fdc", "The disk controller board", "fdc",
            "FD1793 at $F0-$F3, drive select $F4, INTRQ to NMI, DRQ ending WAIT; 4 drives", &Config::fdc));
    if (model == Model::M1Level2) {
        // The Model I's board has sense switches ($E9): the option sets them
        MachineDescriptor::ExpansionSlot s;
        s.name = "rs232";
        s.description = "The RS-232 interface (in the Expansion Interface)";
        s.devices = {{"rs232", "TR1602 UART and BR1941 baud rate generator at $E8-$EB (the serial command); "
                      "option: the sense switches, as baud-bits-parity-stop (9600-8N1) or a byte ($6F)",
                      {"9600-8N1", "300-8N1", "1200-7E1", "$6F"}, "9600-8N1"}};
        s.install = [sys](const std::string& device, const std::string& option, std::string& err) {
            Config c = sys->cfg();
            if (device.empty()) { c.rs232 = false; sys->configure(c); return true; }
            if (device != "rs232") { err = "the slot takes 'rs232'"; return false; }
            if (!c.expansion) { err = "it needs the Expansion Interface (expansion ei ei)"; return false; }
            int sw = -1;
            if (!parseSwitches(option.empty() ? "9600-8N1" : option, sw)) {
                err = "switches: baud-bits-parity-stop (e.g. 9600-8N1, 300-7E2) or a byte ($6F)";
                return false;
            }
            c.rs232 = true;
            c.switches = (uint8_t)sw;
            sys->configure(c);
            return true;
        };
        s.info = [sys]() -> std::string {
            if (!sys->cfg().rs232) return "";
            char b[48];
            std::snprintf(b, sizeof b, "rs232 (switches $%02X: %s)", sys->cfg().switches, describeSwitches(sys->cfg().switches).c_str());
            return b;
        };
        desc->expansionSlots.push_back(std::move(s));
    } else if (model != Model::M1Level1) {
        desc->expansionSlots.push_back(flagSlot(sys, "rs232", "The RS-232 interface", "rs232",
            "TR1602 UART and BR1941 baud rate generator at $E8-$EB (the serial command)", &Config::rs232));
    }
    desc->expansionSlots.push_back(flagSlot(sys, "orchestra", isModel1(model) ? "The Orchestra-85 music card" : "The Orchestra-90 music card",
        "orchestra", isModel1(model) ? "Two 8-bit DACs: left $B9, right $B5 (heard mixed, with the cassette motor off)"
                                     : "Two 8-bit DACs: left $79, right $75 (heard mixed, with the cassette motor off)",
        &Config::orchestra));
    if (model != Model::M1Level1)
        desc->expansionSlots.push_back(flagSlot(sys, "harddisk", "The Radio Shack hard disk interface", "harddisk",
            "WD1010 at $C8-$CF, write protect $C0, control $C1; up to 4 drives, Reed .hdv images (disk mount 1-4 x.hdv hard)",
            &Config::hardDisk, model == Model::M1Level2));
    if (model == Model::M1Level2)
        desc->expansionSlots.push_back(flagSlot(sys, "stringy", "The Exatron Stringy Floppy", "stringy",
            "8 wafer drives at $F0-$F7 (.esf: disk mount 1-8 x.esf stringy), its ROM at $3000 (esf.rom)",
            &Config::stringy));
    if (!isModel1(model)) {
        MachineDescriptor::ExpansionSlot s;
        s.name = "hires";
        s.description = "A hi-res graphics board (640 x 240)";
        s.devices = {
            {"tandy", "Radio Shack's (26-1126): ports $80-$83, the Model 4's offsets and overlay at $8C-$8E", {}, ""},
            {"grafyx", "Micro-Labs' Grafyx Solution: ports $80-$83 (mode bit 1 text off); on the Model III also through port $FF and video RAM", {}, ""},
        };
        s.install = [sys](const std::string& device, const std::string&, std::string& err) {
            Config c = sys->cfg();
            if (device.empty()) c.hires = HiresBoard::None;
            else if (device == "tandy") c.hires = HiresBoard::Tandy;
            else if (device == "grafyx") c.hires = HiresBoard::MicroLabs;
            else { err = "tandy or grafyx"; return false; }
            sys->configure(c);
            return true;
        };
        s.info = [sys]() -> std::string {
            switch (sys->cfg().hires) {
            case HiresBoard::Tandy: return "tandy";
            case HiresBoard::MicroLabs: return "grafyx";
            default: return "";
            }
        };
        desc->expansionSlots.push_back(std::move(s));
    }

    if (isModel4(model)) {
        // `speed 2` / `speed 4` (warp is the frontends' max speed)
        auto warp = std::make_shared<bool>(true);
        desc->getWarp = [warp]() { return *warp; };
        desc->setWarp = [warp](bool on) { *warp = on; };
        desc->cpuSpeeds = {2.02752, 4.05504};
        desc->cpuSpeedMHz = [sys]() { return sys->fast() ? 4.05504 : 2.02752; };
        desc->setCpuSpeedMHz = [sys](double mhz) {
            if (mhz != 2.02752 && mhz != 4.05504 && mhz != 2.0 && mhz != 4.0) return false;
            sys->setFast(mhz > 3.0);
            return true;
        };
    }

    desc->symbolFiles.push_back(PathUtil::findResource("roms/trs80/hardware.sym"));
    desc->onReset = [sys](MachineDescriptor&) { sys->powerOn(); };
    desc->schedulerStep = [sys](MachineDescriptor&) -> int { return sys->step(); };
    desc->deviceCycles = [sys]() { return sys->deviceCycles(); };
    desc->onReset(*desc);
    return desc;
}

bool loadCmd(MachineDescriptor* m, const std::vector<uint8_t>& b, std::string& err) {
    System* sys = systemOf(m);
    if (!sys) { err = "not a TRS-80"; return false; }
    // Records: type, length, data. 01 load block (address, bytes; a length
    // of 0-2 means 256-258), 02 transfer address, others skipped (05 name)
    size_t i = 0;
    bool loaded = false;
    while (i + 2 <= b.size()) {
        const uint8_t type = b[i];
        size_t len = b[i + 1];
        i += 2;
        if (type == 0x01) {
            if (len < 3) len += 256;
            if (i + len > b.size()) { err = "truncated load block"; return false; }
            const uint16_t addr = (uint16_t)(b[i] | b[i + 1] << 8);
            for (size_t k = 2; k < len; ++k) sys->write8((uint16_t)(addr + k - 2), b[i + k]);
            loaded = true;
        } else if (type == 0x02) {
            if (i + 2 > b.size()) break;
            m->cpus[0].cpu->setPc((uint16_t)(b[i] | b[i + 1] << 8));
            return true;
        } else if (len == 0) {
            len = 256;
        }
        i += len;
    }
    if (!loaded) { err = "not a /CMD file"; return false; }
    return true;
}

}  // namespace trs80
