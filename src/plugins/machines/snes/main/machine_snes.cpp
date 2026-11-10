// Super Nintendo: the 5A22's memory map, timing, internal registers, DMA and
// HDMA, the cartridge mappings and the joypads, after Mesen2
// (Core/SNES/SnesMemoryManager.cpp, InternalRegisters.cpp,
// SnesDmaController.cpp, BaseCartridge.cpp, SnesControlManager.cpp),
// GPL-3.0, Sour and the Mesen contributors, and fullsnes / Anomie.

#include "machine_snes.h"
#include "snes_apu.h"
#include "snes_cx4.h"
#include "snes_gsu.h"
#include "snes_necdsp.h"
#include "snes_ppu.h"
#include "snes_sa1.h"
#include "snes_obc1.h"
#include "snes_sdd1.h"
#include "snes_spc7110.h"
#include "snes_st018.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdebug/main/execution_observer.h"
#include "libmem/main/ibus.h"
#include "plugins/65816/main/cpu65816.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

namespace snes {
namespace {

constexpr uint64_t MASTER_HZ = 21477272;
constexpr int LINE_CLOCKS = 1364;
constexpr int REFRESH_AT = 538;
constexpr int HDMA_AT = 276 * 4;

// ---------------------------------------------------------------------------
// The cartridge
// ---------------------------------------------------------------------------

enum class Map { LoRom, HiRom, ExHiRom };

/** bsnes Bus::mirror: fold an offset into a ROM whose size is not a power of two. */
uint32_t mirror(uint32_t addr, uint32_t size) {
    if (!size) return 0;
    uint32_t base = 0, mask = 1u << 23;
    while (addr >= size) {
        while (!(addr & mask)) mask >>= 1;
        addr -= mask;
        if (size > mask) { size -= mask; base += mask; }
        mask >>= 1;
    }
    return base + addr;
}

/** Where coprocessor firmware is looked for: MMEMU_SNES_FIRMWARE, else roms/snes. */
std::string firmwareDir() {
    const char* v = std::getenv("MMEMU_SNES_FIRMWARE");
    return v ? v : "roms/snes";
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

struct Cart {
    std::vector<uint8_t> rom, sram;
    Map map = Map::LoRom;
    std::string title;
    uint32_t headerAt = 0;
    // A uPD7725 (DSP-1..4): its registers, DR and SR, told apart by srMask
    std::unique_ptr<NecDsp> dsp;
    std::string dspName;
    uint16_t dspSrMask = 0;
    // The ST010/ST011 (a uPD96050): registers at $60:0000-$0FFF, its RAM at $68-$6F:0000-$0FFF, saved as the battery
    bool st01x = false;
    std::vector<uint8_t> dspSave;               // its RAM as bytes, little-endian words
    // The Super FX: it owns the map ($3000 registers, its RAM, ROM in two layouts)
    std::unique_ptr<Gsu> gsu;
    // The SA-1: it owns the map too (I-RAM, BW-RAM = sram, the MMC's ROM banks)
    std::unique_ptr<Sa1> sa1;
    // The Cx4: its registers and data RAM at $6000-$7FFF; ROM and RAM keep the LoROM map
    std::unique_ptr<Cx4> cx4;
    // The S-DD1: its own map ($4800 registers, banked ROM, decompression on DMA)
    std::unique_ptr<Sdd1> sdd1;
    // The SPC7110: its own map too ($4800 registers, data ROM windows, decompression, RTC)
    std::unique_ptr<Spc7110> spc7110;
    // The OBC1: its ports over the battery RAM at $6000-$7FFF; the rest is the plain map
    std::unique_ptr<Obc1> obc1;
    // The ST018 (an ARM): its link registers at $3000-$3FFF; the rest is the plain map
    std::unique_ptr<St018> st018;
    // Battery RAM (chipset low nibble 2, 5, 6, 9 or A, as Mesen): the cartridge RAM, or the Super FX's
    bool battery = false;
    std::string savePath;
    std::vector<uint8_t> saved;                 // what the file holds
    std::vector<uint8_t> savedRtc;              // what the clock's file holds

    /** The clock's file beside the save: `<rom>.rtc`, as Mesen. */
    std::string rtcPath() const {
        if (savePath.empty()) return "";
        const auto dot = savePath.find_last_of('.');
        return (dot == std::string::npos ? savePath : savePath.substr(0, dot)) + ".rtc";
    }

    std::vector<uint8_t>* batteryRam() {
        if (!battery) return nullptr;
        if (st01x) {
            const auto& r = dsp->dataRam();
            dspSave.resize(r.size() * 2);
            for (size_t i = 0; i < r.size(); ++i) { dspSave[i * 2] = (uint8_t)r[i]; dspSave[i * 2 + 1] = (uint8_t)(r[i] >> 8); }
            return &dspSave;
        }
        if (gsu) return &gsu->ram();
        return sram.empty() ? nullptr : &sram;
    }
    void loadSave(const std::string& path) {
        savePath = path;
        if (spc7110 && spc7110->hasRtc() && !path.empty()) {
            std::vector<uint8_t> data;
            if (readFile(rtcPath(), data)) spc7110->setRtcState(data);
            savedRtc = spc7110->rtcState();
        }
        auto* ram = batteryRam();
        if (!ram || path.empty()) return;
        std::vector<uint8_t> data;
        if (readFile(path, data) && !data.empty())
            for (size_t i = 0; i < ram->size(); ++i) (*ram)[i] = data[i % data.size()];
        if (st01x) {
            auto& r = dsp->dataRam();
            for (size_t i = 0; i < r.size(); ++i) r[i] = (uint16_t)(dspSave[i * 2] | dspSave[i * 2 + 1] << 8);
        }
        saved = *ram;
    }
    /** Write the battery RAM if it changed since the last write. */
    void flushSave() {
        if (spc7110 && spc7110->hasRtc() && !savePath.empty()) {
            const auto st = spc7110->rtcState();
            if (st != savedRtc) {
                std::ofstream f(rtcPath(), std::ios::binary | std::ios::trunc);
                if (f && f.write((const char*)st.data(), (std::streamsize)st.size())) savedRtc = st;
            }
        }
        auto* ram = batteryRam();
        if (!ram || savePath.empty() || *ram == saved) return;
        std::ofstream f(savePath, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write((const char*)ram->data(), (std::streamsize)ram->size());
        if (f) saved = *ram;
    }

    /** DSP-1/2/3/4 by the header's chipset byte and the title (as Mesen). */
    bool loadDsp(const std::string& rawTitle, std::string& err, bool st = false) {
        if (st) dspName = rawTitle == "2DAN MORITA SHOUGI" ? "st011" : "st010";
        else if (rawTitle == "DUNGEON MASTER") dspName = "dsp2";
        else if (rawTitle == "PILOTWINGS") dspName = "dsp1";
        else if (rawTitle == "SD\xB6\xDE\xDD\xC0\xDE\xD1GX") dspName = "dsp3";
        else if (rawTitle == "PLANETS CHAMP TG3000" || rawTitle == "TOP GEAR 3000") dspName = "dsp4";
        else dspName = "dsp1b";
        dsp = std::make_unique<NecDsp>();
        if (st) dsp->setModel(true, dspName == "st011" ? 22000000.0 : 11000000.0);
        const uint32_t progSize = dsp->programSize(), dataSize = dsp->dataSize();
        std::vector<uint8_t> fw;
        const uint32_t size = progSize + dataSize;
        if ((rom.size() & (st ? 0xFFFF : 0x7FFF)) == size) {
            // The firmware appended to the ROM image
            fw.assign(rom.end() - size, rom.end());
            rom.resize(rom.size() - size);
        } else {
            const std::string dir = firmwareDir();
            std::vector<uint8_t> prog, data;
            if (!readFile(dir + "/" + dspName + ".rom", fw) || fw.size() != size) {
                fw.clear();
                if (readFile(dir + "/" + dspName + ".program.rom", prog) && readFile(dir + "/" + dspName + ".data.rom", data) &&
                    prog.size() == progSize && data.size() == dataSize) {
                    fw = prog;
                    fw.insert(fw.end(), data.begin(), data.end());
                }
            }
            if (fw.empty()) {
                std::string upper = dspName;
                for (auto& c : upper) c = (char)std::toupper((unsigned char)c);
                err = "This cartridge has a " + upper + " chip: its firmware is needed in " + dir + "/" + dspName +
                      ".rom (" + std::to_string(size) + " bytes; or " + dspName + ".program.rom and " + dspName + ".data.rom)";
                dsp.reset();
                return false;
            }
        }
        if (!dsp->load(fw, err)) return false;
        st01x = st;
        dspSrMask = st ? 0x0001 : map == Map::LoRom ? 0x4000 : 0x1000;
        return true;
    }

    /** The DSP's registers: LoROM $30-$3F:$8000-$FFFF and $60-$6F:$0000-$7FFF (SR at A14), HiROM $00-$1F:$6000-$7FFF (SR at A12). */
    bool dspAt(uint8_t bank, uint16_t a, bool& sr) const {
        if (!dsp) return false;
        const uint8_t b = bank & 0x7F;
        bool hit;
        if (st01x) hit = b == 0x60 && a < 0x1000;
        else if (map == Map::LoRom) hit = (b >= 0x30 && b < 0x40 && a >= 0x8000) || (b >= 0x60 && b < 0x70 && a < 0x8000);
        else hit = b < 0x20 && a >= 0x6000 && a < 0x8000;
        sr = (a & dspSrMask) != 0;
        return hit;
    }

    /** Score the internal header at `at` (bsnes/Mesen heuristics). */
    static int score(const std::vector<uint8_t>& r, uint32_t at, Map m) {
        if (r.size() < at + 0x40) return -100;
        int s = 0;
        const uint8_t mode = r[at + 0x15];
        const uint16_t sum = (uint16_t)(r[at + 0x1E] | r[at + 0x1F] << 8), cmp = (uint16_t)(r[at + 0x1C] | r[at + 0x1D] << 8);
        if ((uint16_t)(sum ^ cmp) == 0xFFFF) s += 4;
        const uint16_t reset = (uint16_t)(r[at + 0x3C] | r[at + 0x3D] << 8);
        if (reset < 0x8000) s -= 4;
        else {
            // the first opcode at the reset vector
            const uint32_t off = m == Map::LoRom ? (at & ~0x7FFFu) + (reset & 0x7FFF) : (at & ~0xFFFFu) + reset;
            if (off < r.size()) {
                const uint8_t op = r[off];
                if (op == 0x78 || op == 0x18 || op == 0x38 || op == 0x9C || op == 0x4C || op == 0x5C || op == 0xC2 || op == 0xE2 || op == 0xA9 || op == 0x20 || op == 0x22) s += 2;
                if (op == 0x00 || op == 0xFF || op == 0xDB || op == 0x42) s -= 4;
            }
        }
        const uint8_t low = mode & 0x0F;
        if (m == Map::LoRom && (low == 0 || low == 2 || low == 3)) s += 2;
        if (m == Map::HiRom && (low == 1 || low == 5 || low == 0xA)) s += 2;
        if (m == Map::ExHiRom && low == 5) s += 2;
        if ((mode & 0xE0) == 0x20) s += 1;
        for (int i = 0; i < 21; ++i) { const uint8_t c = r[at + i]; if (c < 0x20 || c > 0x7E) { s -= 1; break; } }
        if (r[at + 0x18] <= 0x08) s += 1;
        return s;
    }

    bool load(std::vector<uint8_t> img, std::string& err) {
        if (img.size() % 1024 == 512) img.erase(img.begin(), img.begin() + 512);   // copier header
        if (img.size() < 0x8000) { err = "SNES ROM too small (" + std::to_string(img.size()) + " bytes)"; return false; }
        rom = std::move(img);
        int best = -1000;
        const std::pair<uint32_t, Map> cands[] = {{0x7FC0, Map::LoRom}, {0xFFC0, Map::HiRom}, {0x40FFC0, Map::ExHiRom}};
        for (auto& c : cands) {
            const int s = score(rom, c.first, c.second);
            if (s > best) { best = s; headerAt = c.first; map = c.second; }
        }
        title.clear();
        std::string raw;
        for (int i = 0; i < 21; ++i) { const char c = (char)rom[headerAt + i]; title += (c >= 0x20 && c < 0x7F) ? c : ' '; raw += c; }
        while (!title.empty() && title.back() == ' ') title.pop_back();
        while (!raw.empty() && raw.back() == ' ') raw.pop_back();
        const uint8_t ramSize = rom[headerAt + 0x18];
        sram.assign(ramSize && ramSize <= 0x0C ? (1024u << ramSize) : 0, 0);
        // The chipset byte: a coprocessor when the low nibble is 3 or more; 0x in the high nibble is a NEC DSP
        const uint8_t chips = rom[headerAt + 0x16];
        const uint8_t chipLow = chips & 0x0F;
        battery = chipLow == 0x02 || chipLow == 0x05 || chipLow == 0x06 || chipLow == 0x09 || chipLow == 0x0A;
        if ((chips & 0x0F) >= 0x03 && (chips >> 4) == 0x0 && !loadDsp(raw, err)) return false;
        if ((chips & 0x0F) >= 0x03 && (chips >> 4) == 0x1) {
            // The Super FX's RAM: the expansion RAM size ($FFBD), else 64 KB (as Mesen)
            const uint8_t exp = headerAt >= 3 ? rom[headerAt - 3] : 0;
            gsu = std::make_unique<Gsu>(&rom, exp >= 1 && exp <= 7 ? (1024u << exp) : 0x10000);
            sram.clear();
        }
        if ((chips & 0x0F) >= 0x03 && (chips >> 4) == 0x3) sa1 = std::make_unique<Sa1>(&rom, &sram);
        // The Cx4: chipset $Fx with cartridge type $10 ($FFBF), as Mesen
        if ((chips & 0x0F) >= 0x03 && (chips >> 4) == 0xF && headerAt >= 1 && rom[headerAt - 1] == 0x10)
            cx4 = std::make_unique<Cx4>(&rom, &sram);
        if ((chips & 0x0F) >= 0x03 && (chips >> 4) == 0x4) sdd1 = std::make_unique<Sdd1>(&rom, &sram);
        // The OBC1: chipset $2x, given cartridge RAM (as Mesen)
        if ((chips & 0x0F) >= 0x03 && (chips >> 4) == 0x2 && !sram.empty()) obc1 = std::make_unique<Obc1>(&sram);
        // The ST010/ST011: chipset $Fx with cartridge type $01; battery-backed DSP RAM (as Mesen)
        if ((chips & 0x0F) >= 0x03 && (chips >> 4) == 0xF && headerAt >= 1 && rom[headerAt - 1] == 0x01) {
            if (!loadDsp(raw, err, true)) return false;
            battery = true;
        }
        // The ST018: chipset $Fx with cartridge type $02; battery-backed (as Mesen). Its firmware: st018.rom
        if ((chips & 0x0F) >= 0x03 && (chips >> 4) == 0xF && headerAt >= 1 && rom[headerAt - 1] == 0x02) {
            const std::string path = firmwareDir() + "/st018.rom";
            std::vector<uint8_t> fw;
            if (!readFile(path, fw) || fw.size() != St018::PRG_SIZE + St018::DATA_SIZE) {
                err = "This cartridge has an ST018 chip: its firmware is needed in " + path + " (" +
                      std::to_string(St018::PRG_SIZE + St018::DATA_SIZE) + " bytes)";
                return false;
            }
            st018 = std::make_unique<St018>(fw);
            battery = true;
        }
        // The SPC7110: chipset $Fx with cartridge type $00; $F9 has the RTC. Always battery-backed (as Mesen)
        if ((chips & 0x0F) >= 0x03 && (chips >> 4) == 0xF && headerAt >= 1 && rom[headerAt - 1] == 0x00) {
            spc7110 = std::make_unique<Spc7110>(&rom, &sram, chipLow == 0x09);
            battery = true;
        }
        return true;
    }

    std::string describe() const {
        static const char* names[] = {"LoROM", "HiROM", "ExHiROM"};
        std::string s = title + ", " + names[(int)map] + ", " + std::to_string(rom.size() / 1024) + "K ROM";
        if (!sram.empty()) s += ", " + std::to_string(sram.size() / 1024) + "K RAM";
        if (battery) s += ", battery";
        if (dsp) { std::string n = dspName; for (auto& c : n) c = (char)std::toupper((unsigned char)c); s += ", " + n; }
        if (gsu) s += ", Super FX, " + std::to_string(gsu->ram().size() / 1024) + "K RAM";
        if (sa1) s += ", SA-1";
        if (cx4) s += ", Cx4";
        if (sdd1) s += ", S-DD1";
        if (obc1) s += ", OBC1";
        if (st018) s += ", ST018";
        if (spc7110) s += spc7110->hasRtc() ? ", SPC7110, RTC" : ", SPC7110";
        return s;
    }

    /** The Super FX map for the SNES: 0 ROM, 1 RAM (offset in `off`), -1 nothing. */
    int gsuMap(uint8_t bank, uint16_t a, uint32_t& off) const {
        const uint8_t b = bank & 0x7F;
        if (b < 0x40) {
            if (a >= 0x8000) { off = (uint32_t)b * 0x8000 + (a - 0x8000); return 0; }
            if (a >= 0x6000) { off = (uint32_t)(a - 0x6000); return 1; }
            return -1;
        }
        if (b < 0x60) { off = (uint32_t)(b - 0x40) << 16 | a; return 0; }
        if (b == 0x70 || b == 0x71) { off = (uint32_t)(b & 1) << 16 | a; return 1; }
        return -1;
    }

    /** -1: the cartridge does not answer (open bus). */
    int read(uint8_t bank, uint16_t a) const {
        if (gsu) {
            uint32_t off;
            const int k = gsuMap(bank, a, off);
            if (k == 0) return gsu->cpuReadRom(mirror(off, (uint32_t)rom.size()), a);
            if (k == 1) return gsu->cpuReadRam(off);
            return -1;
        }
        const int32_t r = romOffset(bank, a);
        if (r >= 0) return rom[mirror((uint32_t)r, (uint32_t)rom.size())];
        const int32_t s = sramOffset(bank, a);
        if (s >= 0) return sram[(uint32_t)s % sram.size()];
        return -1;
    }
    void write(uint8_t bank, uint16_t a, uint8_t v) {
        if (gsu) {
            uint32_t off;
            if (gsuMap(bank, a, off) == 1) gsu->cpuWriteRam(off, v);
            return;
        }
        const int32_t s = sramOffset(bank, a);
        if (s >= 0) sram[(uint32_t)s % sram.size()] = v;
    }

    int32_t romOffset(uint8_t bank, uint16_t a) const {
        const uint8_t b = bank & 0x7F;
        switch (map) {
        case Map::LoRom:
            if (b >= 0x7E && !(bank & 0x80)) return -1;
            if (a >= 0x8000) return (int32_t)(b * 0x8000 + (a - 0x8000));
            if (b >= 0x40 && b < 0x70) return (int32_t)(b * 0x8000 + a);    // the lower halves of $40-$6F
            return -1;
        case Map::HiRom:
            if (b >= 0x7E && !(bank & 0x80)) return -1;
            if (b >= 0x40) return (int32_t)(((b & 0x3F) << 16) | a);
            if (a >= 0x8000) return (int32_t)(((b & 0x3F) << 16) | a);
            return -1;
        case Map::ExHiRom: {
            if (b >= 0x7E && !(bank & 0x80)) return -1;
            const uint32_t hi = (bank & 0x80) ? 0 : 0x400000;
            if (b >= 0x40) return (int32_t)(hi | ((b & 0x3F) << 16) | a);
            if (a >= 0x8000) return (int32_t)(hi | ((b & 0x3F) << 16) | a);
            return -1;
        }
        }
        return -1;
    }
    int32_t sramOffset(uint8_t bank, uint16_t a) const {
        if (sram.empty()) return -1;
        const uint8_t b = bank & 0x7F;
        if (map == Map::LoRom) {
            if (b >= 0x70 && b < 0x7E && a < 0x8000) return (int32_t)(((b - 0x70) << 15) | a);
            return -1;
        }
        if (b >= 0x20 && b < 0x40 && a >= 0x6000 && a < 0x8000) return (int32_t)(((b - 0x20) << 13) | (a - 0x6000));
        return -1;
    }
};

// ---------------------------------------------------------------------------
// DMA
// ---------------------------------------------------------------------------

struct DmaChannel {
    uint8_t ctrl = 0xFF, bbad = 0xFF;
    uint16_t a1t = 0xFFFF;
    uint8_t a1b = 0xFF;
    uint16_t das = 0xFFFF;
    uint8_t dasb = 0xFF;
    uint16_t a2a = 0xFFFF;
    uint8_t nltr = 0xFF, unused = 0xFF;
    bool doTransfer = false, done = true;
    bool dmaActive = false;     // a general DMA in progress (HDMA on the channel cancels it)
};

const uint8_t MODE_LEN[8] = {1, 2, 2, 4, 4, 4, 2, 4};
const uint8_t MODE_OFS[8][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1}, {0, 1, 2, 3}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1}};

// ---------------------------------------------------------------------------
// The bus
// ---------------------------------------------------------------------------

class SnesBus : public IBus, public W65C816::IdleHook {
public:
    SnesBus(Ppu* ppu, Apu* apu) : m_ppu(ppu), m_apu(apu) {
        m_cfg = {24, 8, BusRole::DATA, true, 0xFFFFFF};
        m_wram.assign(0x20000, 0);
    }

    void setCart(std::unique_ptr<Cart> c, std::string label) { m_cart = std::move(c); m_label = std::move(label); }
    Cart* cart() const { return m_cart.get(); }
    const std::string& label() const { return m_label; }
    void setCpu(W65C816* cpu) { m_cpu = cpu; }

    uint16_t pads[2] = {0, 0};

    uint64_t master() const { return m_master; }
    int line() const { return m_line; }
    int hclock() const { return m_hClock; }

    void powerOn() {
        std::fill(m_wram.begin(), m_wram.end(), 0);
        m_master = 0; m_hClock = 0; m_line = 0;
        m_lines = 262; m_vblankLine = 225; m_lineClocks = LINE_CLOCKS;
        m_refreshDone = m_hdmaDone = false;
        m_irqFired = false;
        m_dmaPending = m_hdmaPending = m_hdmaInitPending = m_dmaStartDelay = false;
        m_hdmaInitDue = false; m_hdmaInitAt = 12; m_refreshAt = REFRESH_AT; m_dmaClock = 0; m_cpuSpeed = 8;
        m_nmitimen = 0; m_wrio = 0xFF;
        m_mpyA = 0xFF; m_mpyB = 0xFF; m_dividend = 0xFFFF; m_quotient = 0; m_product = 0;
        m_aluShift = 0; m_multCounter = m_divCounter = 0; m_aluPrev = m_cpu ? m_cpu->cycles() : 0;
        m_htime = m_vtime = 0x1FF;
        m_hdmaen = 0; m_fastRom = false;
        m_nmiFlag = false; m_irqFlag = false;
        m_joyBusy = 0;
        m_joy.fill(0);
        m_wramAddr = 0;
        m_openBus = 0;
        m_strobe = false; m_serial[0] = m_serial[1] = 0;
        for (auto& c : m_dma) c = DmaChannel{};
        m_apuSynced = 0;
        if (m_cart && m_cart->dsp) m_cart->dsp->reset();
        if (m_cart && m_cart->gsu) m_cart->gsu->reset();
        if (m_cart && m_cart->sa1) m_cart->sa1->reset();
        if (m_cart && m_cart->cx4) m_cart->cx4->reset();
        if (m_cart && m_cart->st018) m_cart->st018->powerOn();
        if (m_cart && m_cart->sdd1) m_cart->sdd1->reset();
        if (m_cart && m_cart->spc7110) m_cart->spc7110->reset();
    }

    /** After a CPU step: the interrupt lines, the APU brought up to time. */
    void endStep() {
        m_cpu->setNmiLine(m_nmiFlag && (m_nmitimen & 0x80));
        m_cpu->setIrqLine(m_irqFlag || (m_cart && ((m_cart->gsu && m_cart->gsu->irq()) || (m_cart->sa1 && m_cart->sa1->cpuIrq()) || (m_cart->cx4 && m_cart->cx4->irq()))));
        syncApu();
    }
    void syncApu() {
        const uint64_t target = m_master / MASTER_HZ * Apu::CLOCK_HZ + (m_master % MASTER_HZ) * Apu::CLOCK_HZ / MASTER_HZ;
        if (target > m_apuSynced) { m_apu->runTo(target); m_apuSynced = target; }
    }

    // ------------------------------------------------------------------
    // Time
    // ------------------------------------------------------------------
    void advance(int clocks) {
        while (clocks > 0) {
            int next = m_lineClocks;
            if (m_hdmaInitDue) next = m_hdmaInitAt;
            else if (!m_refreshDone) next = m_refreshAt;
            else if (!m_hdmaDone) next = HDMA_AT;
            const int step = std::min(clocks, std::max(0, next - m_hClock));
            m_hClock += step;
            m_master += (uint64_t)step;
            clocks -= step;
            checkIrq();
            if (m_hClock >= next) {
                if (m_hdmaInitDue) {
                    // Line 0: the HDMA channels are set up for the frame (run at the CPU's next cycle but one)
                    m_hdmaInitDue = false;
                    m_hdmaInitPending = true;
                    m_dmaStartDelay = true;
                } else if (!m_refreshDone) {
                    m_refreshDone = true;
                    m_hClock += 40;                       // DRAM refresh: the CPU waits
                    m_master += 40;
                } else if (!m_hdmaDone) {
                    m_hdmaDone = true;
                    if (m_line < m_vblankLine) {
                        m_ppu->endLine(m_line);
                        // HDMA for the next line, at the CPU's next cycle but one (as Mesen)
                        if (m_hdmaen) { m_hdmaPending = true; m_dmaStartDelay = true; }
                    }
                } else {
                    endOfLine();
                }
            }
        }
        if (m_cart && m_cart->gsu) m_cart->gsu->runTo(m_master);    // the Super FX keeps pace
        if (m_cart && m_cart->sa1) m_cart->sa1->runTo(m_master);    // and the SA-1
        if (m_cart && m_cart->cx4) m_cart->cx4->runTo(m_master);
        if (m_cart && m_cart->st018) m_cart->st018->runTo(m_master);    // and the ST018's ARM
    }
    void idle(uint32_t) override { cpuCycle(6); advance(6); }
    /** The start of a CPU cycle of `speed` clocks: pending DMA/HDMA runs here (Mesen's ProcessCpuCycle). */
    void cpuCycle(int speed) {
        m_cpuSpeed = speed;
        if (m_dmaPending || m_hdmaPending || m_hdmaInitPending) processPendingDma();
    }

    // ------------------------------------------------------------------
    // IBus
    // ------------------------------------------------------------------
    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "system"; }

    uint8_t read8(uint32_t addr) override {
        addr &= 0xFFFFFF;
        const int sp = speed(addr);
        cpuCycle(sp);
        advance(sp - 4);
        const uint8_t v = busRead(addr, false);
        advance(4);
        if (m_observer) m_observer->onMemoryRead(this, addr, v);
        return v;
    }
    void write8(uint32_t addr, uint8_t v) override {
        addr &= 0xFFFFFF;
        if (m_observer) m_observer->onMemoryWrite(this, addr, peek8(addr), v);
        const int sp = speed(addr);
        cpuCycle(sp);
        advance(sp);
        busWrite(addr, v);
    }
    uint8_t peek8(uint32_t addr) override { return busRead(addr & 0xFFFFFF, true); }

    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

private:
    /** The H counter: 4 clocks a dot, but dots 323 and 327 take 6. */
    int dot() const {
        if (m_hClock <= 1292) return m_hClock >> 2;
        if (m_hClock <= 1310) return (m_hClock - 2) >> 2;
        return (m_hClock - 4) >> 2;
    }

    int speed(uint32_t addr) const {
        const uint8_t bank = (uint8_t)(addr >> 16);
        const uint16_t a = (uint16_t)addr;
        if (bank >= 0x40 && bank < 0x80) return 8;
        if (bank >= 0xC0) return m_fastRom ? 6 : 8;
        if (a < 0x2000) return 8;
        if (a < 0x4000) return 6;
        if (a < 0x4200) return 12;
        if (a < 0x6000) return 6;
        if (a < 0x8000) return 8;
        return (bank >= 0x80 && m_fastRom) ? 6 : 8;
    }

    uint8_t busRead(uint32_t addr, bool peek) {
        const uint8_t bank = (uint8_t)(addr >> 16);
        const uint16_t a = (uint16_t)addr;
        if (bank == 0x7E || bank == 0x7F) return m_openBus = m_wram[((bank & 1) << 16) | a];
        if (!(bank & 0x40)) {
            if (a < 0x2000) return m_openBus = m_wram[a];
            if (a >= 0x2100 && a < 0x2200) return m_openBus = bRead((uint8_t)a, peek);
            if (a == 0x4016 || a == 0x4017) {
                uint8_t v = (uint8_t)(m_openBus & 0xFC);
                if (a == 0x4017) v |= 0x1C;
                const int p = a & 1;
                v |= m_strobe ? (uint8_t)(pads[p] >> 15) : (uint8_t)(m_serial[p] >> 15);
                if (!peek && !m_strobe) m_serial[p] = (uint16_t)(m_serial[p] << 1 | 1);
                return m_openBus = v;
            }
            if (a >= 0x4200 && a < 0x4220) return internalRead(a, peek);   // internal: open bus kept
            if (a >= 0x4300 && a < 0x4380) return dmaRead(a);
        }
        if (m_cart) {
            if (m_cart->sa1) {
                const int v = m_cart->sa1->cpuRead(bank, a, peek);
                return v >= 0 ? (m_openBus = (uint8_t)v) : m_openBus;
            }
            if (m_cart->sdd1) {
                const int v = m_cart->sdd1->cpuRead(bank, a, peek);
                return v >= 0 ? (m_openBus = (uint8_t)v) : m_openBus;
            }
            if (m_cart->spc7110) {
                const int v = m_cart->spc7110->cpuRead(bank, a, peek);
                return v >= 0 ? (m_openBus = (uint8_t)v) : m_openBus;
            }
            if (m_cart->gsu && !(bank & 0x40) && a >= 0x3000 && a < 0x4000)
                return m_openBus = peek ? m_cart->gsu->peekReg(a) : m_cart->gsu->readReg(a);
            if (m_cart->cx4 && !(bank & 0x40) && a >= 0x6000 && a < 0x8000) return m_openBus = m_cart->cx4->read(a);
            if (m_cart->obc1 && !(bank & 0x40) && a >= 0x6000 && a < 0x8000) return m_openBus = m_cart->obc1->read(a);
            if (m_cart->st018 && !(bank & 0x40) && a >= 0x3000 && a < 0x4000) {
                if (!peek) m_cart->st018->runTo(m_master);
                const int v = m_cart->st018->read(a, peek);
                return v >= 0 ? (m_openBus = (uint8_t)v) : m_openBus;
            }
            if (m_cart->st01x && (bank & 0x78) == 0x68 && a < 0x1000) {
                if (!peek) m_cart->dsp->runTo(m_master, MASTER_HZ);
                const uint16_t w = m_cart->dsp->dataRam()[(a >> 1) & 0x7FF];
                return m_openBus = (a & 1) ? (uint8_t)(w >> 8) : (uint8_t)w;
            }
            bool sr;
            if (m_cart->dspAt(bank, a, sr)) {
                if (peek) { const auto& st = m_cart->dsp->state(); return sr ? (uint8_t)(st.sr >> 8) : (uint8_t)st.dr; }
                m_cart->dsp->runTo(m_master, MASTER_HZ);
                return m_openBus = m_cart->dsp->read(sr);
            }
            const int v = m_cart->read(bank, a);
            if (v >= 0) return m_openBus = (uint8_t)v;
        }
        return m_openBus;
    }

    void busWrite(uint32_t addr, uint8_t v) {
        const uint8_t bank = (uint8_t)(addr >> 16);
        const uint16_t a = (uint16_t)addr;
        m_openBus = v;
        if (bank == 0x7E || bank == 0x7F) { m_wram[((bank & 1) << 16) | a] = v; return; }
        if (!(bank & 0x40)) {
            if (a < 0x2000) { m_wram[a] = v; return; }
            if (a >= 0x2100 && a < 0x2200) { bWrite((uint8_t)a, v); return; }
            if (a == 0x4016) {
                const bool s = v & 1;
                if (m_strobe && !s) { m_serial[0] = pads[0]; m_serial[1] = pads[1]; }
                m_strobe = s;
                return;
            }
            if (a >= 0x4200 && a < 0x4220) { internalWrite(a, v); return; }
            if (a >= 0x4300 && a < 0x4380) {
                if (m_cart && m_cart->sdd1) m_cart->sdd1->dmaRegWrite(a, v);     // the S-DD1 watches the DMA setup
                dmaWrite(a, v);
                return;
            }
        }
        if (m_cart) {
            if (m_cart->sa1) { m_cart->sa1->cpuWrite(bank, a, v); return; }
            if (m_cart->sdd1) { m_cart->sdd1->cpuWrite(bank, a, v); return; }
            if (m_cart->spc7110) { m_cart->spc7110->cpuWrite(bank, a, v); return; }
            if (m_cart->gsu && !(bank & 0x40) && a >= 0x3000 && a < 0x4000) { m_cart->gsu->writeReg(a, v); return; }
            if (m_cart->cx4 && !(bank & 0x40) && a >= 0x6000 && a < 0x8000) { m_cart->cx4->write(a, v); return; }
            if (m_cart->obc1 && !(bank & 0x40) && a >= 0x6000 && a < 0x8000) { m_cart->obc1->write(a, v); return; }
            if (m_cart->st018 && !(bank & 0x40) && a >= 0x3000 && a < 0x4000) {
                m_cart->st018->runTo(m_master);
                m_cart->st018->write(a, v);
                return;
            }
            if (m_cart->st01x && (bank & 0x78) == 0x68 && a < 0x1000) {
                m_cart->dsp->runTo(m_master, MASTER_HZ);
                uint16_t& w = m_cart->dsp->dataRam()[(a >> 1) & 0x7FF];
                w = (a & 1) ? (uint16_t)((w & 0x00FF) | v << 8) : (uint16_t)((w & 0xFF00) | v);
                return;
            }
            bool sr;
            if (m_cart->dspAt(bank, a, sr)) {
                m_cart->dsp->runTo(m_master, MASTER_HZ);
                m_cart->dsp->write(sr, v);
                return;
            }
            m_cart->write(bank, a, v);
        }
    }

    // The B bus ($21xx): the PPU, the APU ports, the WRAM port
    uint8_t bRead(uint8_t r, bool peek) {
        if (r == 0x37) {
            // SLHV: latch the H/V counters (when WRIO bit 7 is set); open bus
            if (!peek && (m_wrio & 0x80)) m_ppu->latchCounters(dot(), m_line);
            return m_openBus;
        }
        if (r < 0x40) return peek ? m_ppu->peek(r) : m_ppu->read(r);
        if (r < 0x80) { syncApu(); return m_apu->cpuRead(r & 3); }
        if (r == 0x80) {
            const uint8_t v = m_wram[m_wramAddr & 0x1FFFF];
            if (!peek) m_wramAddr = (m_wramAddr + 1) & 0x1FFFF;
            return v;
        }
        return m_openBus;
    }
    void bWrite(uint8_t r, uint8_t v) {
        if (r < 0x40) {
            if (r < 0x34) m_ppu->sync(m_line, m_hClock >> 2);     // the pixels so far with the old value
            m_ppu->write(r, v);
            return;
        }
        if (r < 0x80) { syncApu(); m_apu->cpuWrite(r & 3, v); return; }
        switch (r) {
        case 0x80: m_wram[m_wramAddr & 0x1FFFF] = v; m_wramAddr = (m_wramAddr + 1) & 0x1FFFF; break;
        case 0x81: m_wramAddr = (m_wramAddr & 0x1FF00) | v; break;
        case 0x82: m_wramAddr = (m_wramAddr & 0x100FF) | (uint32_t)v << 8; break;
        case 0x83: m_wramAddr = (m_wramAddr & 0x0FFFF) | (uint32_t)(v & 1) << 16; break;
        }
    }

    // $4200-$421F
    uint8_t internalRead(uint16_t a, bool peek) {
        switch (a) {
        case 0x4210: {
            const uint8_t v = (uint8_t)((m_nmiFlag << 7) | (m_openBus & 0x70) | 0x02);
            if (!peek) m_nmiFlag = false;
            return v;
        }
        case 0x4211: {
            const uint8_t v = (uint8_t)((m_irqFlag << 7) | (m_openBus & 0x7F));
            if (!peek) m_irqFlag = false;
            return v;
        }
        case 0x4212: {
            const bool vblank = m_line >= m_vblankLine;
            const bool hblank = m_hClock < 4 || m_hClock >= 274 * 4;
            return (uint8_t)((vblank << 7) | (hblank << 6) | (m_openBus & 0x3E) | (m_joyBusy > 0 ? 1 : 0));
        }
        case 0x4213: return m_wrio;
        case 0x4214: if (!peek) runAlu(true); return (uint8_t)m_quotient;
        case 0x4215: if (!peek) runAlu(true); return (uint8_t)(m_quotient >> 8);
        case 0x4216: if (!peek) runAlu(true); return (uint8_t)m_product;
        case 0x4217: if (!peek) runAlu(true); return (uint8_t)(m_product >> 8);
        default:
            if (a >= 0x4218) return m_joy[a - 0x4218];
            return m_openBus;
        }
    }
    // The multiplier and divider (Mesen's AluMulDiv): a bit a CPU cycle, 8 cycles to multiply and 16 to
    // divide, shift-and-add on the result registers themselves, so a read part way through sees the
    // partial result. They count the CPU's own cycles, so they stand still during DMA.
    void runAlu(bool forRead) {
        uint64_t now = m_cpu->cycles();
        if (forRead) --now;                       // a read happens earlier in its cycle than a write
        if (m_multCounter || m_divCounter) {
            uint64_t n = now - m_aluPrev;
            while (n-- && (m_multCounter || m_divCounter)) {
                if (m_multCounter) {
                    --m_multCounter;
                    if (m_quotient & 1) m_product = (uint16_t)(m_product + m_aluShift);
                    m_aluShift <<= 1;
                    m_quotient >>= 1;
                }
                if (m_divCounter) {
                    --m_divCounter;
                    m_aluShift >>= 1;
                    m_quotient = (uint16_t)(m_quotient << 1);
                    if (m_product >= m_aluShift) { m_product = (uint16_t)(m_product - m_aluShift); m_quotient |= 1; }
                }
            }
        }
        m_aluPrev = now;
    }
    void aluWrite(uint16_t a, uint8_t v) {
        runAlu(true);
        // One finishing on this cycle still counts as running: it blocks starting another
        const bool busy = m_multCounter || m_divCounter;
        runAlu(false);
        switch (a) {
        case 0x4202: m_mpyA = v; break;
        case 0x4203:
            m_product = 0;
            if (!busy) {
                m_multCounter = 8;
                m_mpyB = v;
                m_quotient = (uint16_t)(v << 8 | m_mpyA);
                m_aluShift = v;
            } else if (!m_multCounter && !m_divCounter) {
                m_quotient = (uint16_t)(v << 8 | m_mpyA);   // on the last clock: the value lands, nothing starts
            }
            break;
        case 0x4204: m_dividend = (uint16_t)((m_dividend & 0xFF00) | v); break;
        case 0x4205: m_dividend = (uint16_t)((m_dividend & 0x00FF) | v << 8); break;
        case 0x4206:
            m_product = m_dividend;
            if (!busy) {
                m_divCounter = 16;
                m_aluShift = (uint32_t)v << 16;
            }
            break;
        default: break;
        }
    }

    void internalWrite(uint16_t a, uint8_t v) {
        switch (a) {
        case 0x4200:
            m_nmitimen = v;
            if (!(v & 0x30)) m_irqFlag = false;
            m_cpu->setNmiLine(m_nmiFlag && (v & 0x80));
            break;
        case 0x4201:
            if ((m_wrio & 0x80) && !(v & 0x80)) m_ppu->latchCounters(dot(), m_line);
            m_wrio = v;
            break;
        case 0x4202: case 0x4203: case 0x4204: case 0x4205: case 0x4206: aluWrite(a, v); break;
        case 0x4207: m_htime = (uint16_t)((m_htime & 0x100) | v); break;
        case 0x4208: m_htime = (uint16_t)((m_htime & 0xFF) | (v & 1) << 8); break;
        case 0x4209: m_vtime = (uint16_t)((m_vtime & 0x100) | v); break;
        case 0x420A: m_vtime = (uint16_t)((m_vtime & 0xFF) | (v & 1) << 8); break;
        case 0x420B: generalDma(v); break;
        case 0x420C: m_hdmaen = v; break;
        case 0x420D: m_fastRom = v & 1; break;
        default: break;
        }
    }

    // $43x0-$43xF
    uint8_t dmaRead(uint16_t a) const {
        const DmaChannel& c = m_dma[(a >> 4) & 7];
        switch (a & 0x0F) {
        case 0x0: return c.ctrl;
        case 0x1: return c.bbad;
        case 0x2: return (uint8_t)c.a1t;
        case 0x3: return (uint8_t)(c.a1t >> 8);
        case 0x4: return c.a1b;
        case 0x5: return (uint8_t)c.das;
        case 0x6: return (uint8_t)(c.das >> 8);
        case 0x7: return c.dasb;
        case 0x8: return (uint8_t)c.a2a;
        case 0x9: return (uint8_t)(c.a2a >> 8);
        case 0xA: return c.nltr;
        case 0xB: case 0xF: return c.unused;
        default: return m_openBus;
        }
    }
    void dmaWrite(uint16_t a, uint8_t v) {
        DmaChannel& c = m_dma[(a >> 4) & 7];
        switch (a & 0x0F) {
        case 0x0: c.ctrl = v; break;
        case 0x1: c.bbad = v; break;
        case 0x2: c.a1t = (uint16_t)((c.a1t & 0xFF00) | v); break;
        case 0x3: c.a1t = (uint16_t)((c.a1t & 0x00FF) | v << 8); break;
        case 0x4: c.a1b = v; break;
        case 0x5: c.das = (uint16_t)((c.das & 0xFF00) | v); break;
        case 0x6: c.das = (uint16_t)((c.das & 0x00FF) | v << 8); break;
        case 0x7: c.dasb = v; break;
        case 0x8: c.a2a = (uint16_t)((c.a2a & 0xFF00) | v); break;
        case 0x9: c.a2a = (uint16_t)((c.a2a & 0x00FF) | v << 8); break;
        case 0xA: c.nltr = v; break;
        case 0xB: case 0xF: c.unused = v; break;
        default: break;
        }
    }

    // DMA timing (Mesen's SnesDmaController): it starts on a whole multiple of 8 master clocks, takes 8
    // clocks of overhead, 8 a channel and 8 a byte (read at 4, written at 8), and ends on a whole CPU
    // cycle. HDMA cuts in between a DMA's bytes.
    void syncStartDma() {
        m_dmaClock = 8 - (int)(m_master & 7);
        advance(m_dmaClock);
    }
    void syncEndDma() { advance(m_cpuSpeed - (m_dmaClock % m_cpuSpeed)); }
    bool anyDmaActive() const {
        for (const auto& c : m_dma) if (c.dmaActive) return true;
        return false;
    }
    static bool isWorkRam(uint32_t addr) {
        const uint8_t bank = (uint8_t)(addr >> 16);
        return bank == 0x7E || bank == 0x7F || (!(bank & 0x40) && (uint16_t)addr < 0x2000);
    }
    /** A byte the DMA reads from the A bus: it can't reach the B bus or the DMA registers (open bus). */
    uint8_t dmaReadA(uint32_t aAddr) {
        advance(4);
        const uint16_t a = (uint16_t)aAddr;
        if (!(aAddr & 0x400000) && ((a & 0xFF00) == 0x2100 || (a >= 0x4300 && a < 0x4380) || a == 0x420B || a == 0x420C))
            return m_openBus;
        return busRead(aAddr, false);
    }

    /** One byte between the A bus and the B bus ($21xx), in the channel's direction: 8 clocks. */
    void transfer(const DmaChannel& c, uint32_t aAddr, uint8_t bReg) {
        const uint16_t a = (uint16_t)aAddr;
        const bool aInvalid = !(aAddr & 0x400000) && ((a & 0xFF00) == 0x2100 || (a >= 0x4300 && a < 0x4380) || a == 0x420B || a == 0x420C);
        const bool wramPort = bReg == 0x80 && isWorkRam(aAddr);
        if (c.ctrl & 0x80) {                                  // B -> A
            if (wramPort) {
                // $2180 -> work RAM: no read; the write happens, with a meaningless value
                advance(4);
                advance(4);
                busWrite(aAddr, 0xFF);
                return;
            }
            advance(4);
            const uint8_t v = bRead(bReg, false);
            advance(4);
            if (!aInvalid) busWrite(aAddr, v);
        } else {
            if (wramPort) { advance(8); return; }             // work RAM -> $2180: nothing is written
            const uint8_t v = dmaReadA(aAddr);
            advance(4);
            bWrite(bReg, v);
        }
    }

    /** Pending transfers, at the start of a CPU cycle (Mesen's ProcessPendingTransfers). */
    bool processPendingDma() {
        if (!(m_dmaPending || m_hdmaPending || m_hdmaInitPending)) return false;
        if (m_dmaStartDelay) { m_dmaStartDelay = false; return false; }
        if (m_hdmaPending) return hdmaRun();
        if (m_hdmaInitPending) return hdmaInitRun();
        m_dmaPending = false;
        syncStartDma();
        advance(8);                                           // the DMA's overhead
        m_dmaClock += 8;
        processPendingDma();
        for (int ch = 0; ch < 8; ++ch) {
            DmaChannel& c = m_dma[ch];
            if (!c.dmaActive) continue;
            advance(8);                                       // the channel's overhead
            m_dmaClock += 8;
            processPendingDma();
            const int mode = c.ctrl & 7;
            int i = 0;
            do {
                transfer(c, (uint32_t)c.a1b << 16 | c.a1t, (uint8_t)(c.bbad + MODE_OFS[mode][i & 3]));
                if (!(c.ctrl & 0x08)) c.a1t = (c.ctrl & 0x10) ? (uint16_t)(c.a1t - 1) : (uint16_t)(c.a1t + 1);
                --c.das;
                ++i;
                processPendingDma();                          // HDMA can cut in
            } while (c.das != 0 && c.dmaActive);              // HDMA on this channel ends it (as Mesen)
            m_dmaClock += 8 * i;
            c.dmaActive = false;
        }
        syncEndDma();
        return true;
    }

    void generalDma(uint8_t channels) {
        // Every channel is active from the start: HDMA on a channel cancels its DMA, even before its turn.
        // It starts at the CPU's next cycle but one.
        for (int ch = 0; ch < 8; ++ch) if ((channels >> ch) & 1) m_dma[ch].dmaActive = true;
        if (channels) { m_dmaPending = true; m_dmaStartDelay = true; }
    }

    // HDMA (Anomie's HDMA doc, Mesen SnesDmaController)
    bool hdmaInitRun() {
        m_hdmaInitPending = false;
        for (auto& c : m_dma) { c.done = false; c.doTransfer = false; }
        if (!m_hdmaen) return false;
        const bool sync = !anyDmaActive();
        if (sync) syncStartDma();
        m_dmaClock += 8;
        advance(8);
        for (int ch = 0; ch < 8; ++ch) {
            DmaChannel& c = m_dma[ch];
            c.doTransfer = true;
            if (!(m_hdmaen & (1 << ch))) continue;
            c.a2a = c.a1t;
            c.dmaActive = false;
            c.nltr = dmaReadA((uint32_t)c.a1b << 16 | c.a2a);
            advance(4);
            m_dmaClock += 8;
            c.a2a = (uint16_t)(c.a2a + 1);
            if (c.nltr == 0) c.done = true;
            if (c.ctrl & 0x40) {
                const uint8_t lo = dmaReadA((uint32_t)c.a1b << 16 | c.a2a);
                c.a2a = (uint16_t)(c.a2a + 1);
                advance(4);
                m_dmaClock += 8;
                if (!c.done) {
                    const uint8_t hi = dmaReadA((uint32_t)c.a1b << 16 | c.a2a);
                    c.a2a = (uint16_t)(c.a2a + 1);
                    advance(4);
                    m_dmaClock += 8;
                    c.das = (uint16_t)(lo | hi << 8);
                } else {
                    c.das = (uint16_t)(lo << 8);
                }
            }
        }
        if (sync) syncEndDma();
        return true;
    }
    bool lastActiveHdma(int ch) const {
        for (int i = ch + 1; i < 8; ++i) if ((m_hdmaen & (1 << i)) && !m_dma[i].done) return false;
        return true;
    }
    bool hdmaRun() {
        m_hdmaPending = false;
        if (!m_hdmaen) return false;
        const bool sync = !anyDmaActive();
        if (sync) syncStartDma();
        m_dmaClock += 8;
        advance(8);
        // Every channel's transfer first, then the tables for the next line
        for (int ch = 0; ch < 8; ++ch) {
            DmaChannel& c = m_dma[ch];
            if (!(m_hdmaen & (1 << ch))) continue;
            c.dmaActive = false;
            if (c.done || !c.doTransfer) continue;
            const int mode = c.ctrl & 7;
            for (int i = 0; i < MODE_LEN[mode]; ++i) {
                uint32_t src;
                if (c.ctrl & 0x40) { src = (uint32_t)c.dasb << 16 | c.das; c.das = (uint16_t)(c.das + 1); }
                else { src = (uint32_t)c.a1b << 16 | c.a2a; c.a2a = (uint16_t)(c.a2a + 1); }
                transfer(c, src, (uint8_t)(c.bbad + MODE_OFS[mode][i]));
            }
            m_dmaClock += 8 * MODE_LEN[mode];
        }
        for (int ch = 0; ch < 8; ++ch) {
            DmaChannel& c = m_dma[ch];
            if (!(m_hdmaen & (1 << ch)) || c.done) continue;
            --c.nltr;
            c.doTransfer = c.nltr & 0x80;
            // The next byte of the table is read whether it's needed or not
            const uint8_t next = dmaReadA((uint32_t)c.a1b << 16 | c.a2a);
            advance(4);
            m_dmaClock += 8;
            if (!(c.nltr & 0x7F)) {
                c.nltr = next;
                c.a2a = (uint16_t)(c.a2a + 1);
                if (c.ctrl & 0x40) {
                    if (c.nltr == 0 && lastActiveHdma(ch)) {
                        // The last active channel ending: only the high byte is read, the low is $00
                        const uint8_t hi = dmaReadA((uint32_t)c.a1b << 16 | c.a2a);
                        c.a2a = (uint16_t)(c.a2a + 1);
                        advance(4);
                        m_dmaClock += 8;
                        c.das = (uint16_t)(hi << 8);
                    } else {
                        const uint8_t lo = dmaReadA((uint32_t)c.a1b << 16 | c.a2a);
                        c.a2a = (uint16_t)(c.a2a + 1);
                        advance(4);
                        const uint8_t hi = dmaReadA((uint32_t)c.a1b << 16 | c.a2a);
                        c.a2a = (uint16_t)(c.a2a + 1);
                        advance(4);
                        m_dmaClock += 16;
                        c.das = (uint16_t)(lo | hi << 8);
                    }
                }
                if (c.nltr == 0) c.done = true;
                c.doTransfer = true;
            }
        }
        if (sync) syncEndDma();
        return true;
    }

    void checkIrq() {
        const bool hEn = m_nmitimen & 0x10, vEn = m_nmitimen & 0x20;
        if ((!hEn && !vEn) || m_irqFired) return;
        if (vEn && m_line != m_vtime) return;
        const int at = hEn ? m_htime * 4 + 14 : 10;
        if (m_hClock >= at) { m_irqFlag = true; m_irqFired = true; }
    }

    void endOfLine() {
        m_hClock -= m_lineClocks;
        m_refreshDone = m_hdmaDone = false;
        m_irqFired = false;
        if (m_joyBusy > 0) --m_joyBusy;
        if (++m_line == m_lines) {
            m_line = 0;
            m_nmiFlag = false;
            // A frame is 262 lines; interlaced, the even field's is 263
            m_ppu->startFrame();
            m_vblankLine = m_ppu->vblankLine();
            m_lines = (m_ppu->interlace() && !m_ppu->oddField()) ? 263 : 262;
            m_hdmaInitDue = true;
            m_hdmaInitAt = 12 + (int)(m_master & 7);
        }
        m_refreshAt = REFRESH_AT - (int)(m_master & 7);
        // Not interlaced, line 240 of the odd field is 4 clocks short
        m_lineClocks = (m_line == 240 && m_ppu->oddField() && !m_ppu->interlace()) ? LINE_CLOCKS - 4 : LINE_CLOCKS;
        if (m_line == m_vblankLine) {
            m_ppu->startVblank();
            if (m_cart && m_cart->battery && ++m_saveFrames >= 60) { m_saveFrames = 0; m_cart->flushSave(); }
            m_nmiFlag = true;
            if (m_nmitimen & 0x01) {
                // Auto joypad read: the pads' 16 bits into $4218-$421F (3 lines busy)
                m_joy.fill(0);
                for (int p = 0; p < 2; ++p) { m_joy[p * 2] = (uint8_t)pads[p]; m_joy[p * 2 + 1] = (uint8_t)(pads[p] >> 8); }
                m_serial[0] = 0xFFFF; m_serial[1] = 0xFFFF;
                m_joyBusy = 3;
            }
        }
        checkIrq();
    }

    BusConfig m_cfg;
    Ppu* m_ppu;
    Apu* m_apu;
    W65C816* m_cpu = nullptr;
    std::unique_ptr<Cart> m_cart;
    std::string m_label;
    std::vector<uint8_t> m_wram;

    uint64_t m_master = 0, m_apuSynced = 0;
    int m_hClock = 0, m_line = 0;
    int m_lines = 262, m_vblankLine = 225, m_lineClocks = LINE_CLOCKS;
    int m_saveFrames = 0;
    bool m_refreshDone = false, m_hdmaDone = false, m_irqFired = false;
    // DMA/HDMA requests waiting for the CPU's next cycle
    bool m_dmaPending = false, m_hdmaPending = false, m_hdmaInitPending = false, m_dmaStartDelay = false;
    bool m_hdmaInitDue = false;
    int m_hdmaInitAt = 12, m_refreshAt = REFRESH_AT, m_dmaClock = 0, m_cpuSpeed = 8;

    uint8_t m_nmitimen = 0, m_wrio = 0xFF, m_mpyA = 0xFF, m_mpyB = 0xFF, m_hdmaen = 0;
    uint16_t m_dividend = 0xFFFF, m_quotient = 0, m_product = 0, m_htime = 0x1FF, m_vtime = 0x1FF;
    uint32_t m_aluShift = 0;
    uint8_t m_multCounter = 0, m_divCounter = 0;
    uint64_t m_aluPrev = 0;
    bool m_fastRom = false, m_nmiFlag = false, m_irqFlag = false;
    int m_joyBusy = 0;
    std::array<uint8_t, 8> m_joy{};
    uint32_t m_wramAddr = 0;
    uint8_t m_openBus = 0;
    bool m_strobe = false;
    uint16_t m_serial[2] = {};
    DmaChannel m_dma[8];
};

SnesBus* busOf(MachineDescriptor* m) {
    if (!m) return nullptr;
    for (auto& b : m->buses)
        if (auto* p = dynamic_cast<SnesBus*>(b.bus)) return p;
    return nullptr;
}

void powerOn(MachineDescriptor* m) {
    auto* bus = busOf(m);
    if (!bus) return;
    m->ioRegistry->resetAll();
    bus->powerOn();
    auto* cpu = static_cast<W65C816*>(m->cpus[0].cpu);
    cpu->setNmiLine(false);
    cpu->setIrqLine(false);
    cpu->reset();
}

uint16_t padBits(uint32_t b) {
    using M = MachineDescriptor;
    uint16_t p = 0;
    if (b & M::PAD_A) p |= BTN_A;
    if (b & M::PAD_B) p |= BTN_B;
    if (b & M::PAD_X) p |= BTN_X;
    if (b & M::PAD_Y) p |= BTN_Y;
    if (b & M::PAD_L) p |= BTN_L;
    if (b & M::PAD_R) p |= BTN_R;
    if (b & M::PAD_SELECT) p |= BTN_SELECT;
    if (b & M::PAD_START) p |= BTN_START;
    if (b & M::PAD_UP) p |= BTN_UP;
    if (b & M::PAD_DOWN) p |= BTN_DOWN;
    if (b & M::PAD_LEFT) p |= BTN_LEFT;
    if (b & M::PAD_RIGHT) p |= BTN_RIGHT;
    return p;
}

}  // namespace

bool insertCartridge(MachineDescriptor* m, const std::vector<uint8_t>& image, const std::string& label, std::string& err,
                     const std::string& savePath) {
    auto* bus = busOf(m);
    if (!bus) { err = "Not a SNES."; return false; }
    auto cart = std::make_unique<Cart>();
    if (!cart->load(image, err)) return false;
    if (bus->cart()) bus->cart()->flushSave();
    cart->loadSave(savePath);
    const std::string d = cart->describe();
    bus->setCart(std::move(cart), label + " (" + d + ")");
    powerOn(m);
    return true;
}

void removeCartridge(MachineDescriptor* m) {
    auto* bus = busOf(m);
    if (!bus) return;
    if (bus->cart()) bus->cart()->flushSave();
    bus->setCart(nullptr, "");
    powerOn(m);
}

void flushSave(MachineDescriptor* m) {
    auto* bus = busOf(m);
    if (bus && bus->cart()) bus->cart()->flushSave();
}

std::string savePathFor(const std::string& romPath) {
    const auto slash = romPath.find_last_of('/');
    const auto dot = romPath.find_last_of('.');
    const std::string stem = (dot != std::string::npos && (slash == std::string::npos || dot > slash)) ? romPath.substr(0, dot) : romPath;
    return stem + ".srm";
}

std::string cartridgeInfo(MachineDescriptor* m) {
    auto* bus = busOf(m);
    return bus && bus->cart() ? bus->label() : "";
}

void setButtons(MachineDescriptor* m, int port, uint16_t buttons) {
    if (auto* bus = busOf(m)) bus->pads[port & 1] = buttons;
}

uint64_t masterClock(MachineDescriptor* m) { auto* b = busOf(m); return b ? b->master() : 0; }
int scanline(MachineDescriptor* m) { auto* b = busOf(m); return b ? b->line() : 0; }
int hclock(MachineDescriptor* m) { auto* b = busOf(m); return b ? b->hclock() : 0; }

MachineDescriptor* create(bool sfc, std::string* /*err*/) {
    auto* desc = new MachineDescriptor();
    desc->machineId = sfc ? "superfamicom" : "snes";
    desc->displayName = sfc ? "Nintendo Super Famicom" : "Super Nintendo Entertainment System";
    desc->licenseClass = "proprietary";
    desc->uiPanesSet = true;
    desc->uiPanes = {"cartridge"};
    desc->clockHz = (uint32_t)MASTER_HZ;

    auto* ppu = new Ppu();
    auto* apu = new Apu();
    apu->releaseCoreAndBus();
    auto* bus = new SnesBus(ppu, apu);
    desc->buses.push_back({"system", bus});
    desc->buses.push_back({"aram", apu->bus()});
    auto* cpu = new W65C816();
    cpu->setDataBus(bus);
    cpu->setIdleHook(bus);
    bus->setCpu(cpu);
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});
    desc->cpus.push_back({"spc700", apu->cpu(), apu->bus(), apu->bus(), nullptr, false, 1});

    auto* io = new IORegistry();
    desc->ioRegistry = io;
    io->registerHandler(ppu);
    io->registerHandler(apu);
    desc->deleters.push_back([ppu, apu]() { delete ppu; delete apu; });

    // Controllers: keyboard and joystick into pad 1, gamepads into either
    auto* held = new uint16_t[4]{0, 0, 0, 0};   // keys, joystick 0, joystick 1, -
    auto* pads = new uint16_t[2]{0, 0};
    desc->deleters.push_back([held, pads]() { delete[] held; delete[] pads; });
    auto apply = [bus, held, pads]() {
        bus->pads[0] = (uint16_t)(held[0] | held[1] | pads[0]);
        bus->pads[1] = (uint16_t)(held[2] | pads[1]);
    };
    desc->onGamepad = [pads, apply](int port, uint32_t b) {
        if (port < 0 || port > 1) return false;
        pads[port] = padBits(b);
        apply();
        return true;
    };
    desc->joystickPorts = 2;
    desc->primaryJoystickPort = 0;
    desc->onJoystick = [held, apply](int port, uint8_t bits) {
        if (port < 0 || port > 1) return;
        uint16_t p = 0;
        if (!(bits & 0x01)) p |= BTN_UP;
        if (!(bits & 0x02)) p |= BTN_DOWN;
        if (!(bits & 0x04)) p |= BTN_LEFT;
        if (!(bits & 0x08)) p |= BTN_RIGHT;
        if (!(bits & 0x10)) p |= BTN_B;
        held[1 + port] = p;
        apply();
    };
    desc->onKey = [held, apply](const std::string& name, bool down) {
        std::string n = name;
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        uint16_t bit = 0;
        if (n == "up") bit = BTN_UP;
        else if (n == "down") bit = BTN_DOWN;
        else if (n == "left") bit = BTN_LEFT;
        else if (n == "right") bit = BTN_RIGHT;
        else if (n == "x") bit = BTN_A;
        else if (n == "z") bit = BTN_B;
        else if (n == "s") bit = BTN_X;
        else if (n == "a") bit = BTN_Y;
        else if (n == "q") bit = BTN_L;
        else if (n == "w") bit = BTN_R;
        else if (n == "return" || n == "start") bit = BTN_START;
        else if (n == "tab" || n == "left_shift" || n == "right_shift" || n == "select") bit = BTN_SELECT;
        else return false;
        held[0] = down ? (uint16_t)(held[0] | bit) : (uint16_t)(held[0] & ~bit);
        apply();
        return true;
    };
    desc->screenButtons = {
        {"START", {"start"}, "Controller 1's START button (Return)."},
        {"SELECT", {"select"}, "Controller 1's SELECT button (Tab)."}};

    // The RESET button: the CPU restarts at its vector, the APU and PPU
    // reset; work RAM, VRAM and the cartridge's RAM stay
    desc->onReset = [bus, cpu, apu](MachineDescriptor&) {
        apu->reset();
        if (bus->cart() && bus->cart()->dsp) bus->cart()->dsp->reset();
        if (bus->cart() && bus->cart()->gsu) bus->cart()->gsu->reset();
        if (bus->cart() && bus->cart()->sa1) bus->cart()->sa1->reset();
        if (bus->cart() && bus->cart()->cx4) bus->cart()->cx4->reset();
        // The ST018 isn't reset by the button (as Mesen): the SNES resets it through $3804
        if (bus->cart() && bus->cart()->sdd1) bus->cart()->sdd1->reset();
        if (bus->cart() && bus->cart()->spc7110) bus->cart()->spc7110->reset();
        cpu->setNmiLine(false);
        cpu->setIrqLine(false);
        cpu->reset();
    };
    desc->schedulerStep = [bus, cpu](MachineDescriptor&) -> int {
        const uint64_t before = bus->master();
        cpu->step();
        if (bus->master() == before) bus->idle(0);        // WAI / STP: idle cycles, and DMA/HDMA still run
        bus->endStep();
        return (int)(bus->master() - before);
    };
    powerOn(desc);
    return desc;
}

}  // namespace snes
