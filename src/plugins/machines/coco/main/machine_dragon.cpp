// The Dragon 32 and Dragon 64: the CoCo's board as Dragon Data built it.
// The wiring after XRoar (dragon/dragon.c, dragon32.c, dragon64.c, dkbd.c;
// GPL-3.0-or-later) and the Dragon's circuit diagrams; mmsim's own code.

#include "coco_system.h"
#include "dragon_pro.h"
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
    if (model == Model::Dragon200E) {
        // XRoar's romlists dragon200e / dragon200e_alt: the 200-E's own, else the Dragon 64's
        if (!findRom(dir, {"d200e_1", "d200e_rom1", "ic18_v1.4e.ic34", "d64_1", "d64rom1", "dragrom"}, 0x4000, basic)) {
            missing = "Dragon 200-E BASIC ROM not found: put d200e_1.rom (16 KB; also tried d200e_rom1, d64_1) and "
                      "d200e_2.rom (64K mode) in " + dir + "/ or set MMEMU_DRAGON_FIRMWARE to their directory";
            return false;
        }
        if (!findRom(dir, {"d200e_2", "d200e_rom2", "ic17_v1.4e.ic37", "d64_2", "d64rom2"}, 0x4000, basic64k))
            missing = "Dragon 200-E 64K-mode BASIC (d200e_2.rom, 16 KB) not found in " + dir + ": 32K mode only";
        return true;
    }
    if (model == Model::DragonPro) {
        // The BASIC (MAME's alpha_ba is the Dragon 64's 32K-mode BASIC) and, in the second
        // slot, the 8 KB boot ROM (XRoar's romlists dragonpro_basic, dragonpro_boot)
        if (!findRom(dir, {"alpha-basic", "alpha_ba", "d64_1", "d64rom1", "dragrom"}, 0x4000, basic)) {
            missing = "Dragon Professional BASIC ROM not found: put alpha-basic.rom (16 KB; also tried alpha_ba, d64_1) and "
                      "alpha-boot-v1.0.rom (the boot ROM, 8 KB) in " + dir + "/ or set MMEMU_DRAGON_FIRMWARE to their directory";
            return false;
        }
        if (!findRom(dir, {"alpha-boot-v1.0", "alpha_bt_10", "alpha-boot-v0.4", "alpha_bt_04"}, 0x2000, basic64k))
            missing = "Dragon Professional boot ROM (alpha-boot-v1.0.rom, 8 KB; or alpha_bt_10, alpha_bt_04) not found in " +
                      dir + ": BASIC starts directly";
        return true;
    }
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
        if (findRom(dir, {"ddos12a", "ddos12", "ddos40", "ddos42", "ddos15", "ddos11c", "ddos10", "dplus49b", "dplus48", "sdose6", "sdose5"}, size, rom))
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
    keyboard.setDragon200eLayout(m_model == Model::Dragon200E);
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
    if (isDragon64Board(m_model)) {
        // The 6551 at $FF04-$FF07, its own 1.8432 MHz crystal setting the rates; its /IRQ
        // isn't connected (as XRoar)
        m_acia.reset(new Acia6551(0xFF00));
        m_acia->setName("ACIA");
        m_acia->setClockHz((uint32_t)(m_masterHz / TICKS_SLOW));
    }
    if (m_model == Model::DragonPro) wireDragonPro();
}

void System::setDragonRoms(const std::vector<uint8_t>& basic, const std::vector<uint8_t>& basic64k) {
    m_rom.assign(0x4000, 0xFF);
    for (size_t i = 0; i < 0x4000 && !basic.empty(); ++i) m_rom[i] = basic[i % basic.size()];
    m_haveExt = true;
    m_rom64.clear();
    if (isDragon64Board(m_model) && !basic64k.empty()) {
        m_rom64.assign(0x4000, 0xFF);
        for (size_t i = 0; i < 0x4000; ++i) m_rom64[i] = basic64k[i % basic64k.size()];
    }
    dragonRomSelect();
}

// PIA1 PB2 on a Dragon 32: high with 4K or 16K chips, low with 32K/64K
// ones; on the Dragon 64 an output (ROMSEL) with a pull-up
void System::dragonRamSizeUpdate() {
    if (isDragon64Board(m_model) || m_ram.kb() <= 16) {
        pia1.b.inSource |= 0x04;
        pia1.b.inSink |= 0x04;
    } else {
        pia1.b.inSource &= (uint8_t)~0x04;
        pia1.b.inSink &= (uint8_t)~0x04;
    }
}

// The Dragon 64's ROM select: PB2 high the 32K-mode BASIC, low the 64K-mode one.
// The Professional's: PIA2 PA2 high the boot ROM (in m_rom64, 8 KB twice), low BASIC
void System::dragonRomSelect() {
    if (m_pro) m_rom64Sel = !m_rom64.empty() && m_pro->bootSelected();
    else m_rom64Sel = (m_model == Model::Dragon64 || m_model == Model::Dragon200E) && !(pia1.portB() & 0x04);
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
    if (m_pro) proClock(ticks);
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

// ---------------------------------------------------------------------------
// The Dragon 200-E
// ---------------------------------------------------------------------------
bool loadDragonCharset(std::vector<uint8_t>& rom, std::string& notice) {
    const std::string dir = dragonFirmwareDir();
    for (const char* n : {"d200e_26.rom", "d200e_26.bin", "rom26.ic1", "d200e_26"})
        if (readFile(dir + "/" + n, rom) && rom.size() == 0x1000) return true;
    rom.clear();
    notice = "Dragon 200-E character set (d200e_26.rom, 4 KB; or rom26.ic1) not found in " + dir +
             ": the VDG's own characters";
    return false;
}

void System::setDragonCharset(std::vector<uint8_t> rom) {
    m_charset = std::move(rom);
    if (!m_charset.empty()) m_charset.resize(0x1000, 0xFF);
}

// The 200-E's character generator (XRoar's vdg_fetch_handler_chargen): in
// alphanumerics (A/G low) a byte with D7 low is drawn from the ROM, at the
// VDG's row, GM0 (the INT/EXT pin, A7) and the byte, as the external
// characters' dots, inverted (its data is active low); D7 high is
// semigraphics as on any Dragon
void System::fetchVideoCharset(int n, uint16_t* out) {
    const uint8_t pb = pia1.portB();
    const bool graphics = pb & 0x80;
    const unsigned a7 = (pb & 0x10) ? 0x80 : 0;
    const unsigned row = vdg.row();
    while (n > 0) {
        uint16_t r = 0, col = 0;
        const int k = sam.videoBytes(n, r, col);
        const uint8_t* p = m_ram.at(0, r, col);
        for (int i = 0; i < k; ++i) {
            const uint8_t d = p ? p[i] : 0xFF;
            if (!graphics && !(d & 0x80))
                *out++ = (uint16_t)(m_charset[((row << 8) | a7 | d) & 0xFFF] | Mc6847::F_INV | Mc6847::F_EXT);
            else
                *out++ = (uint16_t)(d | ((pb & 0x10) ? Mc6847::F_EXT : 0) | ((d & 0x80) ? Mc6847::F_AS : 0) |
                                    ((d & 0x40) ? Mc6847::F_INV : 0));
        }
        n -= k;
    }
}

// The legends XRoar's dkbd.c gives the 200-E (codes: row << 3 | column as
// the CoCo numbers them, 0x100 with SHIFT)
int dragon200eKey(const std::string& text, size_t& i) {
    constexpr int SHIFT = 0x100;
    const uint8_t c = (uint8_t)text[i];
    switch (c) {
        case ';': return 0;                       // the @ key
        case '+': return 0 | SHIFT;
        case '@': return 49 | SHIFT;              // SHIFT+CLEAR
        case '~': return 31 | SHIFT;              // SHIFT+SPACE
        default: break;
    }
    if ((c != 0xC2 && c != 0xC3) || i + 1 >= text.size()) return -1;
    const unsigned cp = ((c & 0x1Fu) << 6) | ((uint8_t)text[i + 1] & 0x3Fu);
    int code;
    switch (cp) {
        case 0xC7: case 0xE7: code = 32 | SHIFT; break;     // Ç ç: SHIFT+0
        case 0xDC: case 0xFC: code = 50 | SHIFT; break;     // Ü ü: SHIFT+BREAK
        case 0xCF: case 0xEF: code = 30; break;             // Î î: RIGHT
        case 0xBF: code = 30 | SHIFT; break;                // ¿
        case 0xC3: case 0xE3: code = 28; break;             // Ã ã: DOWN
        case 0xA1: code = 28 | SHIFT; break;                // ¡
        case 0xF1: code = 43; break;                        // ñ: the ; key
        case 0xD1: code = 43 | SHIFT; break;                // Ñ
        case 0xA7: code = 31 | SHIFT; break;                // §
        default: return -1;
    }
    ++i;
    return code;
}

}  // namespace coco
