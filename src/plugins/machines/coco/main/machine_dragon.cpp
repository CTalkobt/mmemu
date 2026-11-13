// The Dragon 32 and Dragon 64: the CoCo's board as Dragon Data built it.
// The wiring after XRoar (dragon/dragon.c, dragon32.c, dragon64.c, dkbd.c;
// GPL-3.0-or-later) and the Dragon's circuit diagrams; mmsim's own code.

#include "coco_system.h"
#include "plugins/devices/acia6551/main/acia6551.h"
#include "util/path_util.h"
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace coco {

namespace {

constexpr double ACK_SECONDS = 7e-6;        // the printer's /ACK pulse

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// The first of `names` (each tried as .rom, then .bin) of `size` bytes in `dir`
bool findRom(const std::string& dir, const std::vector<std::string>& names, size_t size, std::vector<uint8_t>& out) {
    for (const auto& n : names)
        for (const char* ext : {".rom", ".bin", ".dgn"})
            if (readFile(dir + "/" + n + ext, out) && out.size() == size) return true;
    out.clear();
    return false;
}

}  // namespace

std::string dragonFirmwareDir() {
    const char* v = std::getenv("MMEMU_DRAGON_FIRMWARE");
    return v ? v : PathUtil::findResource("roms/dragon");
}

bool loadDragonRoms(Model model, std::vector<uint8_t>& basic, std::vector<uint8_t>& basic64k, std::string& missing) {
    const std::string dir = dragonFirmwareDir();
    basic64k.clear();
    if (model == Model::Dragon64) {
        if (!findRom(dir, {"d64_1", "d64rom1", "dragrom"}, 0x4000, basic)) {
            missing = "Dragon 64 BASIC ROM not found: put d64_1.rom (the 32K-mode BASIC, 16 KB; also tried d64rom1, dragrom) and "
                      "d64_2.rom (64K mode) in " + dir + "/ or set MMEMU_DRAGON_FIRMWARE to their directory";
            return false;
        }
        if (!findRom(dir, {"d64_2", "d64rom2"}, 0x4000, basic64k))
            missing = "Dragon 64 64K-mode BASIC (d64_2.rom, 16 KB) not found in " + dir + ": 32K mode only";
        return true;
    }
    if (!findRom(dir, {"d32", "dragon32", "d32rom", "dragon"}, 0x4000, basic)) {
        missing = "Dragon 32 BASIC ROM not found: put d32.rom (16 KB; also tried dragon32, d32rom, dragon) in " + dir +
                  "/ or set MMEMU_DRAGON_FIRMWARE to its directory";
        return false;
    }
    return true;
}

std::vector<uint8_t> dragonPlaceholderRom() {
    std::vector<uint8_t> r(0x4000, 0x12);
    const uint8_t code[] = {
        0x8E, 0x00, 0x00,           // LDX #$0000 (the SAM shows $0000 at reset)
        0x86, 0x60,                 // LDA #$60 (space)
        0xA7, 0x80,                 // STA ,X+
        0x8C, 0x02, 0x00,           // CMPX #$0200
        0x26, 0xF9,                 // BNE
        0x8E, 0x00, 0x00,           // LDX #$0000
        0x10, 0x8E, 0x80, 0x20,     // LDY #$8020
        0xA6, 0xA0,                 // LDA ,Y+
        0x27, 0x04,                 // BEQ done
        0xA7, 0x80,                 // STA ,X+
        0x20, 0xF8,                 // BRA
        0x20, 0xFE,                 // done: BRA *
    };
    std::memcpy(r.data(), code, sizeof code);
    const char* msg = "NO DRAGON BASIC ROM: PUT D32.ROM (D64_1.ROM) IN ROMS/DRAGON/ (SEE DOC/README-DRAGON.MD)";
    for (size_t i = 0; msg[i]; ++i) r[0x20 + i] = (uint8_t)msg[i];
    r[0x20 + std::strlen(msg)] = 0;
    r[0x3FFE] = 0x80;               // reset vector: $8000
    r[0x3FFF] = 0x00;
    return r;
}

std::vector<uint8_t> loadDragonDosRom(const std::string& path, std::string& err) {
    std::vector<uint8_t> rom;
    if (!path.empty()) {
        if (!readFile(path, rom) || rom.empty()) err = "can't read DragonDOS ROM " + path;
        return rom;
    }
    const std::string dir = dragonFirmwareDir();
    for (size_t size : {(size_t)0x2000, (size_t)0x4000})
        if (findRom(dir, {"ddos12a", "ddos12", "ddos40", "ddos15", "ddos10", "dplus49b", "dplus48", "sdose6", "sdose5"}, size, rom))
            return rom;
    err = "DragonDOS ROM not found: put ddos12a.rom (DragonDOS 1.2, 8 KB; or ddos12, ddos10, dplus49b, sdose6) in " + dir +
          "/ (or set MMEMU_DRAGON_FIRMWARE), or give its path";
    return rom;
}

// ---------------------------------------------------------------------------
// The printer port
// ---------------------------------------------------------------------------
Printer::~Printer() {
    if (m_file) std::fclose(m_file);
}

bool Printer::attachSerial(const std::string& spec) {
    for (const char* prefix : {"file:", "printer:"}) {
        const size_t n = std::strlen(prefix);
        if (spec.compare(0, n, prefix) != 0) continue;
        if (m_file) std::fclose(m_file);
        m_file = std::fopen(spec.substr(n).c_str(), "ab");
        return m_file != nullptr;
    }
    if (spec == "none" && m_file) {
        std::fclose(m_file);
        m_file = nullptr;
        return true;
    }
    return false;
}

bool Printer::strobe(bool level, uint8_t data) {
    if (level == m_strobe) return false;
    m_strobe = level;
    if (!level) return false;
    printed.push_back(data);
    if (printed.size() > 65536) printed.erase(printed.begin(), printed.begin() + 32768);
    if (m_file) {
        std::fputc(data, m_file);
        std::fflush(m_file);
    }
    return true;
}

bool AciaPort::attachSerial(const std::string& spec) {
    return m_sys->acia() && m_sys->acia()->attachSerial(spec);
}

void AciaPort::getDeviceInfo(DeviceInfo& out) const {
    if (m_sys->acia()) m_sys->acia()->getDeviceInfo(out);
    else IOHandler::getDeviceInfo(out);
}

// ---------------------------------------------------------------------------
// The board's differences
// ---------------------------------------------------------------------------
void System::wireDragon() {
    keyboard.setDragonLayout(true);
    // PIA1 PA1 is the printer's STROBE (not a serial line): data from PIA0 port B
    pia1.a.postWrite = [this]() {
        const uint8_t pa = pia1.portA();
        soundChanged();
        tape.output((uint8_t)(pa >> 2));
        printerStrobe();
    };
    // PIA1 PB2: the RAM size (Dragon 32) or the ROM select (Dragon 64)
    pia1.b.preRead = [this]() { ramSizeUpdate(); };
    auto vdgAndSound = pia1.b.postWrite;
    pia1.b.postWrite = [this, vdgAndSound]() {
        vdgAndSound();
        dragonRomSelect();
    };
    if (m_model == Model::Dragon64) {
        // The 6551 at $FF04-$FF07, its own 1.8432 MHz crystal setting the rates; its /IRQ
        // isn't connected (as XRoar)
        m_acia.reset(new Acia6551(0xFF00));
        m_acia->setName("ACIA");
        m_acia->setClockHz((uint32_t)(m_masterHz / TICKS_SLOW));
    }
}

void System::setDragonRoms(const std::vector<uint8_t>& basic, const std::vector<uint8_t>& basic64k) {
    m_rom.assign(0x4000, 0xFF);
    for (size_t i = 0; i < 0x4000 && !basic.empty(); ++i) m_rom[i] = basic[i % basic.size()];
    m_haveExt = true;
    m_rom64.clear();
    if (m_model == Model::Dragon64 && !basic64k.empty()) {
        m_rom64.assign(0x4000, 0xFF);
        for (size_t i = 0; i < 0x4000; ++i) m_rom64[i] = basic64k[i % basic64k.size()];
    }
    dragonRomSelect();
}

// PIA1 PB2 on a Dragon 32: high with 4K or 16K chips, low with 32K/64K
// ones; on the Dragon 64 an output (ROMSEL) with a pull-up
void System::dragonRamSizeUpdate() {
    if (m_model == Model::Dragon64 || m_ram.kb() <= 16) {
        pia1.b.inSource |= 0x04;
        pia1.b.inSink |= 0x04;
    } else {
        pia1.b.inSource &= (uint8_t)~0x04;
        pia1.b.inSink &= (uint8_t)~0x04;
    }
}

// The Dragon 64's ROM select: PB2 high the 32K-mode BASIC, low the 64K-mode one
void System::dragonRomSelect() {
    m_rom64Sel = m_model == Model::Dragon64 && !(pia1.portB() & 0x04);
}

void System::printerStrobe() {
    if (!printer.strobe(pia1.portA() & 0x02, pia0.portB())) return;
    pia1.setCa1(false);                       // /ACK
    m_ackLeft = (uint64_t)(m_masterHz * ACK_SECONDS) + 1;
}

// Time passes for the printer's /ACK and the Dragon 64's 6551
void System::dragonClock(uint64_t ticks) {
    if (m_ackLeft) {
        if (m_ackLeft <= ticks) {
            m_ackLeft = 0;
            pia1.setCa1(true);
        } else {
            m_ackLeft -= ticks;
        }
    }
    if (m_acia) {                             // in CPU cycles (16 ticks), 64 at a time
        m_aciaAcc += ticks;
        if (m_aciaAcc >= TICKS_SLOW * 64) {
            m_acia->tick(m_aciaAcc / TICKS_SLOW);
            m_aciaAcc %= TICKS_SLOW;
        }
    }
}

// The CoCo's PIAs answer through their 32 bytes; a Dragon's only where A2
// is low ($FF00-$FF03, $FF08-...), except an unexpanded Dragon 32's
bool System::piaDecoded(uint16_t a) const {
    if (!dragon() || (m_model == Model::Dragon32 && m_ram.kb() <= 32)) return true;
    return !(a & 4);
}

// A byte of the ROM ($8000-$BFFF as offsets 0-$3FFF), nullptr where none is fitted
const uint8_t* System::romByte(uint16_t off) const {
    if (m_rom64Sel) return m_rom64.empty() ? nullptr : &m_rom64[off & 0x3FFF];
    if (off < 0x2000 && !m_haveExt) return nullptr;
    return &m_rom[off & 0x3FFF];
}

}  // namespace coco
