// NES cartridges and mappers after Mesen2 (Core/NES/BaseMapper.cpp,
// NesHeader.cpp, Mappers/Nintendo/*), GPL-3.0, Sour and the Mesen
// contributors, and the nesdev wiki.

#include "nes_cart.h"

#include <algorithm>
#include <cstring>

namespace nes {

// ---------------------------------------------------------------------------
// The header (nesdev "INES", "NES 2.0"; Mesen NesHeader.cpp)
// ---------------------------------------------------------------------------

bool parseHeader(const std::vector<uint8_t>& img, RomInfo& info, std::string& err) {
    if (img.size() < 16 || std::memcmp(img.data(), "NES\x1A", 4) != 0) {
        err = "not an iNES image (no \"NES\\x1A\" header)";
        return false;
    }
    info = RomInfo{};
    const uint8_t f6 = img[6], f7 = img[7];
    info.nes2 = (f7 & 0x0C) == 0x08;
    info.trainer = (f6 & 0x04) != 0;
    info.battery = (f6 & 0x02) != 0;
    info.mirroring = (f6 & 0x08) ? Mirroring::FourScreen : (f6 & 0x01) ? Mirroring::Vertical : Mirroring::Horizontal;
    info.mapper = f6 >> 4;
    if (info.nes2) {
        info.mapper |= (f7 & 0xF0) | ((img[8] & 0x0F) << 8);
        info.submapper = img[8] >> 4;
        auto size = [](uint8_t lsb, uint8_t msb, uint32_t unit) -> uint32_t {
            if (msb == 0x0F) return (uint32_t)((1u << (lsb >> 2)) * ((lsb & 3) * 2 + 1));   // exponent-multiplier
            return (uint32_t)((msb << 8) | lsb) * unit;
        };
        info.prgSize = size(img[4], img[9] & 0x0F, 0x4000);
        info.chrSize = size(img[5], img[9] >> 4, 0x2000);
        auto shifts = [](uint8_t v) -> uint32_t { return v ? 64u << v : 0; };
        const uint32_t work = shifts(img[10] & 0x0F), save = shifts(img[10] >> 4);
        info.prgRamSize = std::max<uint32_t>(work + save, 0x2000);
        info.chrRamSize = shifts(img[11] & 0x0F) + shifts(img[11] >> 4);
    } else {
        // Old dumps carry junk ("DiskDude!") in bytes 7-15: then the high
        // nibble of the mapper is not trusted (Mesen NesHeader::GetMapperID)
        bool junk = false;
        for (int i = 12; i < 16; ++i) if (img[i]) junk = true;
        if (!junk) info.mapper |= f7 & 0xF0;
        info.prgSize = (uint32_t)img[4] * 0x4000;
        info.chrSize = (uint32_t)img[5] * 0x2000;
        info.prgRamSize = 0x2000;
    }
    if (info.prgSize == 0) { err = "iNES image with no PRG ROM"; return false; }
    if (info.chrSize == 0 && info.chrRamSize == 0) info.chrRamSize = 0x2000;
    const size_t need = 16 + (info.trainer ? 512 : 0) + info.prgSize + info.chrSize;
    if (img.size() < need) {
        err = "iNES image truncated: " + std::to_string(img.size()) + " bytes, header says " + std::to_string(need);
        return false;
    }
    return true;
}

std::string mapperName(int m) {
    switch (m) {
    case 0: return "NROM";
    case 1: return "MMC1";
    case 2: return "UxROM";
    case 3: return "CNROM";
    case 4: return "MMC3";
    case 7: return "AxROM";
    default: return "mapper " + std::to_string(m);
    }
}

// ---------------------------------------------------------------------------
// The base cartridge
// ---------------------------------------------------------------------------

Cartridge::Cartridge(RomInfo info, std::vector<uint8_t> prg, std::vector<uint8_t> chr)
    : m_info(info), m_prg(std::move(prg)), m_chr(std::move(chr)) {
    if (m_chr.empty()) {
        m_chrIsRam = true;
        m_chr.assign(std::max<uint32_t>(m_info.chrRamSize, 0x2000), 0);
    }
    m_prgRam.assign(std::max<uint32_t>(m_info.prgRamSize, 0x2000), 0);
    m_vram.assign(m_info.mirroring == Mirroring::FourScreen ? 0x1000 : 0x800, 0);
}

void Cartridge::reset() {
    prg32(0);
    if (m_prg.size() <= 0x4000) prg16(1, 0);
    chr8(0);
    setMirroring(m_info.mirroring);
    m_prgRamEnabled = m_prgRamWritable = true;
}

std::string Cartridge::describe() const {
    std::string s = mapperName(m_info.mapper) + ", " + std::to_string(m_prg.size() / 1024) + "K PRG, " +
                    std::to_string(m_chr.size() / 1024) + "K CHR " + (m_chrIsRam ? "RAM" : "ROM");
    if (m_info.battery) s += ", battery";
    return s;
}

std::string Cartridge::state() const {
    std::string s = "PRG";
    for (int i = 0; i < 4; ++i) s += " " + std::to_string(m_prgMap[i] / 0x2000);
    s += " CHR";
    for (int i = 0; i < 8; ++i) s += " " + std::to_string(m_chrMap[i] / 0x400);
    static const char* mir[] = {"horizontal", "vertical", "single A", "single B", "four-screen"};
    s += std::string(", ") + mir[(int)m_mirroring];
    return s;
}

void Cartridge::prg8(int slot, int bank) {
    const int n = std::max<int>(1, (int)(m_prg.size() / 0x2000));
    if (bank < 0) bank += n;
    bank = ((bank % n) + n) % n;
    m_prgMap[slot & 3] = (uint32_t)bank * 0x2000;
}

void Cartridge::prg16(int slot, int bank) {
    const int n = std::max<int>(1, (int)(m_prg.size() / 0x4000));
    if (bank < 0) bank += n;
    bank = ((bank % n) + n) % n;
    if (m_prg.size() < 0x4000) { prg8(slot * 2, 0); prg8(slot * 2 + 1, 0); return; }
    prg8(slot * 2, bank * 2);
    prg8(slot * 2 + 1, bank * 2 + 1);
}

void Cartridge::prg32(int bank) {
    const int n = std::max<int>(1, (int)(m_prg.size() / 0x8000));
    if (m_prg.size() < 0x8000) { prg16(0, 0); prg16(1, -1); return; }
    bank = ((bank % n) + n) % n;
    for (int i = 0; i < 4; ++i) prg8(i, bank * 4 + i);
}

void Cartridge::chr1(int slot, int bank) {
    const int n = std::max<int>(1, (int)(m_chr.size() / 0x400));
    bank = ((bank % n) + n) % n;
    m_chrMap[slot & 7] = (uint32_t)bank * 0x400;
}

void Cartridge::setMirroring(Mirroring m) {
    if (m_info.mirroring == Mirroring::FourScreen) m = Mirroring::FourScreen;
    m_mirroring = m;
    switch (m) {
    case Mirroring::Horizontal: m_ntMap[0] = m_ntMap[1] = 0; m_ntMap[2] = m_ntMap[3] = 0x400; break;
    case Mirroring::Vertical:   m_ntMap[0] = m_ntMap[2] = 0; m_ntMap[1] = m_ntMap[3] = 0x400; break;
    case Mirroring::SingleA:    m_ntMap[0] = m_ntMap[1] = m_ntMap[2] = m_ntMap[3] = 0; break;
    case Mirroring::SingleB:    m_ntMap[0] = m_ntMap[1] = m_ntMap[2] = m_ntMap[3] = 0x400; break;
    case Mirroring::FourScreen: for (int i = 0; i < 4; ++i) m_ntMap[i] = (uint32_t)i * 0x400; break;
    }
}

uint8_t Cartridge::cpuRead(uint16_t addr, uint8_t openBus) { return cpuPeek(addr, openBus); }

uint8_t Cartridge::cpuPeek(uint16_t addr, uint8_t openBus) const {
    if (addr >= 0x8000) return m_prg[m_prgMap[(addr >> 13) & 3] + (addr & 0x1FFF)];
    if (addr >= 0x6000 && m_prgRamEnabled) return m_prgRam[(addr - 0x6000) % m_prgRam.size()];
    return openBus;
}

void Cartridge::cpuWrite(uint16_t addr, uint8_t value, uint64_t) {
    if (addr >= 0x6000 && addr < 0x8000 && m_prgRamEnabled && m_prgRamWritable)
        m_prgRam[(addr - 0x6000) % m_prgRam.size()] = value;
}

uint8_t Cartridge::ppuRead(uint16_t addr) const {
    addr &= 0x3FFF;
    if (addr < 0x2000) return m_chr[m_chrMap[addr >> 10] + (addr & 0x3FF)];
    return m_vram[(m_ntMap[(addr >> 10) & 3] + (addr & 0x3FF)) % m_vram.size()];
}

void Cartridge::ppuWrite(uint16_t addr, uint8_t value) {
    addr &= 0x3FFF;
    if (addr < 0x2000) {
        if (m_chrIsRam) m_chr[m_chrMap[addr >> 10] + (addr & 0x3FF)] = value;
        return;
    }
    m_vram[(m_ntMap[(addr >> 10) & 3] + (addr & 0x3FF)) % m_vram.size()] = value;
}

// ---------------------------------------------------------------------------
// The mappers
// ---------------------------------------------------------------------------

namespace {

/** 0: NROM, 16 or 32 KB of PRG, 8 KB of CHR, no registers. */
class Nrom : public Cartridge {
public:
    Nrom(RomInfo i, std::vector<uint8_t> p, std::vector<uint8_t> c) : Cartridge(i, std::move(p), std::move(c)) {}
};

/**
 * 1: MMC1 (SxROM). A 5-bit serial port: each write to $8000-$FFFF shifts
 * bit 0 in, the fifth lands in the register picked by A13-A14; bit 7 resets
 * the port and sets PRG mode 3. Writes on consecutive CPU cycles (the dummy
 * write of a read-modify-write) are ignored after the first (Mesen MMC1.h).
 * SUROM/SXROM: CHR bit 4 selects the 256 KB half of a 512 KB PRG.
 */
class Mmc1 : public Cartridge {
public:
    Mmc1(RomInfo i, std::vector<uint8_t> p, std::vector<uint8_t> c) : Cartridge(i, std::move(p), std::move(c)) {}

    void reset() override {
        Cartridge::reset();
        m_shift = 0; m_count = 0;
        m_control = 0x0C;
        m_chr0 = m_chr1 = m_prgReg = 0;
        m_lastWrite = ~0ull - 1;
        update();
    }
    void cpuWrite(uint16_t addr, uint8_t v, uint64_t cycle) override {
        if (addr < 0x8000) { Cartridge::cpuWrite(addr, v, cycle); return; }
        const bool consecutive = cycle == m_lastWrite + 1;
        m_lastWrite = cycle;
        if (consecutive) return;
        if (v & 0x80) {
            m_shift = 0; m_count = 0;
            m_control |= 0x0C;
            update();
            return;
        }
        m_shift = (uint8_t)((m_shift >> 1) | ((v & 1) << 4));
        if (++m_count < 5) return;
        switch ((addr >> 13) & 3) {
        case 0: m_control = m_shift; break;
        case 1: m_chr0 = m_shift; break;
        case 2: m_chr1 = m_shift; break;
        case 3: m_prgReg = m_shift; break;
        }
        m_shift = 0; m_count = 0;
        update();
    }
    std::string state() const override {
        return "control $" + hex(m_control) + " chr0 $" + hex(m_chr0) + " chr1 $" + hex(m_chr1) + " prg $" + hex(m_prgReg) +
               "; " + Cartridge::state();
    }

private:
    static std::string hex(uint8_t v) { static const char* d = "0123456789ABCDEF"; return {d[v >> 4], d[v & 15]}; }
    void update() {
        static const Mirroring mir[4] = {Mirroring::SingleA, Mirroring::SingleB, Mirroring::Vertical, Mirroring::Horizontal};
        setMirroring(mir[m_control & 3]);
        // 512 KB PRG: CHR register bit 4 picks the outer 256 KB
        const int outer = m_prg.size() > 0x40000 ? ((m_chr0 >> 4) & 1) * 16 : 0;
        const int bank = m_prgReg & 0x0F;
        switch ((m_control >> 2) & 3) {
        case 0: case 1: prg16(0, outer + (bank & 0x0E)); prg16(1, outer + (bank | 1)); break;
        case 2: prg16(0, outer); prg16(1, outer + bank); break;
        case 3: prg16(0, outer + bank); prg16(1, outer + 15); break;
        }
        if (m_control & 0x10) { chr4(0, m_chr0); chr4(1, m_chr1); }
        else chr8(m_chr0 >> 1);
        m_prgRamEnabled = !(m_prgReg & 0x10);
    }
    uint8_t m_shift = 0, m_count = 0, m_control = 0x0C, m_chr0 = 0, m_chr1 = 0, m_prgReg = 0;
    uint64_t m_lastWrite = 0;
};

/** 2: UxROM. Any write to $8000-$FFFF selects the 16 KB bank at $8000; the last is fixed at $C000. */
class Uxrom : public Cartridge {
public:
    Uxrom(RomInfo i, std::vector<uint8_t> p, std::vector<uint8_t> c) : Cartridge(i, std::move(p), std::move(c)) {}
    void reset() override { Cartridge::reset(); prg16(0, 0); prg16(1, -1); }
    void cpuWrite(uint16_t addr, uint8_t v, uint64_t cycle) override {
        if (addr < 0x8000) { Cartridge::cpuWrite(addr, v, cycle); return; }
        prg16(0, v);
    }
};

/** 3: CNROM. A write to $8000-$FFFF selects the 8 KB CHR bank. */
class Cnrom : public Cartridge {
public:
    Cnrom(RomInfo i, std::vector<uint8_t> p, std::vector<uint8_t> c) : Cartridge(i, std::move(p), std::move(c)) {}
    void cpuWrite(uint16_t addr, uint8_t v, uint64_t cycle) override {
        if (addr < 0x8000) { Cartridge::cpuWrite(addr, v, cycle); return; }
        chr8(v);
    }
};

/** 7: AxROM. A write selects the 32 KB PRG bank (bits 0-2) and the single-screen nametable (bit 4). */
class Axrom : public Cartridge {
public:
    Axrom(RomInfo i, std::vector<uint8_t> p, std::vector<uint8_t> c) : Cartridge(i, std::move(p), std::move(c)) {}
    void reset() override { Cartridge::reset(); prg32(0); setMirroring(Mirroring::SingleA); }
    void cpuWrite(uint16_t addr, uint8_t v, uint64_t cycle) override {
        if (addr < 0x8000) { Cartridge::cpuWrite(addr, v, cycle); return; }
        prg32(v & 0x07);
        setMirroring((v & 0x10) ? Mirroring::SingleB : Mirroring::SingleA);
    }
};

/**
 * 4: MMC3 (TxROM). Eight bank registers ($8000 selects, $8001 writes),
 * PRG mode (bit 6: $8000 or $C000 fixed to the second-last bank) and CHR
 * A12 inversion (bit 7); mirroring at $A000, work RAM enable/protect at
 * $A001. The scanline counter is clocked by rising edges of PPU A12 after
 * A12 has been low a while (filtering the edges within the sprite fetches):
 * reload from $C000 when zero or when $C001 asked, else decrement; reaching
 * zero with IRQs on ($E001; $E000 disables and acknowledges) asserts /IRQ
 * (the "new" Sharp behaviour; Mesen MMC3.h).
 */
class Mmc3 : public Cartridge {
public:
    Mmc3(RomInfo i, std::vector<uint8_t> p, std::vector<uint8_t> c) : Cartridge(i, std::move(p), std::move(c)) {}

    void reset() override {
        Cartridge::reset();
        static const uint8_t init[8] = {0, 2, 4, 5, 6, 7, 0, 1};
        std::copy(init, init + 8, m_regs);
        m_select = 0;
        m_latch = m_counter = 0;
        m_reload = m_enabled = m_irq = false;
        m_a12Low = 0;
        m_a12WasLow = true;
        update();
    }
    void cpuWrite(uint16_t addr, uint8_t v, uint64_t cycle) override {
        if (addr < 0x8000) { Cartridge::cpuWrite(addr, v, cycle); return; }
        const bool odd = addr & 1;
        switch (addr & 0xE000) {
        case 0x8000:
            if (!odd) m_select = v;
            else m_regs[m_select & 7] = v;
            update();
            break;
        case 0xA000:
            if (!odd) setMirroring((v & 1) ? Mirroring::Horizontal : Mirroring::Vertical);
            else { m_prgRamEnabled = (v & 0x80) != 0; m_prgRamWritable = !(v & 0x40); }
            break;
        case 0xC000:
            if (!odd) m_latch = v;
            else { m_counter = 0; m_reload = true; }
            break;
        case 0xE000:
            if (!odd) { m_enabled = false; m_irq = false; }
            else m_enabled = true;
            break;
        }
    }
    void ppuAddress(uint16_t addr, uint64_t dot) override {
        // A12Watcher: a rise counts when A12 has been low for more than 10 dots
        if (addr & 0x1000) {
            if (m_a12WasLow && dot - m_a12Low > 10) clockCounter();
            m_a12WasLow = false;
        } else if (!m_a12WasLow) {
            m_a12WasLow = true;
            m_a12Low = dot;
        }
    }
    bool irq() const override { return m_irq; }
    std::string state() const override {
        std::string s = "regs";
        for (int i = 0; i < 8; ++i) s += " " + std::to_string(m_regs[i]);
        s += ", IRQ latch " + std::to_string(m_latch) + " counter " + std::to_string(m_counter) +
             (m_enabled ? " on" : " off") + (m_irq ? " asserted" : "");
        return s + "; " + Cartridge::state();
    }

private:
    void clockCounter() {
        if (m_counter == 0 || m_reload) m_counter = m_latch;
        else --m_counter;
        if (m_counter == 0 && m_enabled) m_irq = true;
        m_reload = false;
    }
    void update() {
        if (m_select & 0x40) { prg8(0, -2); prg8(2, m_regs[6]); }
        else { prg8(0, m_regs[6]); prg8(2, -2); }
        prg8(1, m_regs[7]);
        prg8(3, -1);
        const int inv = (m_select & 0x80) ? 4 : 0;
        chr1(0 ^ inv, m_regs[0] & 0xFE); chr1(1 ^ inv, m_regs[0] | 1);
        chr1(2 ^ inv, m_regs[1] & 0xFE); chr1(3 ^ inv, m_regs[1] | 1);
        chr1(4 ^ inv, m_regs[2]); chr1(5 ^ inv, m_regs[3]);
        chr1(6 ^ inv, m_regs[4]); chr1(7 ^ inv, m_regs[5]);
    }
    uint8_t m_regs[8] = {};
    uint8_t m_select = 0, m_latch = 0, m_counter = 0;
    bool m_reload = false, m_enabled = false, m_irq = false;
    uint64_t m_a12Low = 0;
    bool m_a12WasLow = true;
};

}  // namespace

bool Cartridge::supported(int m) { return m == 0 || m == 1 || m == 2 || m == 3 || m == 4 || m == 7; }

std::unique_ptr<Cartridge> Cartridge::load(const std::vector<uint8_t>& img, std::string& err) {
    RomInfo info;
    if (!parseHeader(img, info, err)) return nullptr;
    if (!supported(info.mapper)) {
        err = "iNES mapper " + std::to_string(info.mapper) + " is not supported (have 0 NROM, 1 MMC1, 2 UxROM, 3 CNROM, 4 MMC3, 7 AxROM)";
        return nullptr;
    }
    size_t at = 16 + (info.trainer ? 512 : 0);
    std::vector<uint8_t> prg(img.begin() + (long)at, img.begin() + (long)(at + info.prgSize));
    at += info.prgSize;
    std::vector<uint8_t> chr(img.begin() + (long)at, img.begin() + (long)(at + info.chrSize));
    std::unique_ptr<Cartridge> c;
    switch (info.mapper) {
    case 0: c.reset(new Nrom(info, std::move(prg), std::move(chr))); break;
    case 1: c.reset(new Mmc1(info, std::move(prg), std::move(chr))); break;
    case 2: c.reset(new Uxrom(info, std::move(prg), std::move(chr))); break;
    case 3: c.reset(new Cnrom(info, std::move(prg), std::move(chr))); break;
    case 4: c.reset(new Mmc3(info, std::move(prg), std::move(chr))); break;
    case 7: c.reset(new Axrom(info, std::move(prg), std::move(chr))); break;
    }
    if (info.trainer) {
        const uint8_t* t = img.data() + 16;
        std::copy(t, t + 512, c->prgRam().begin() + 0x1000);    // the trainer sits at $7000
    }
    c->reset();
    return c;
}

}  // namespace nes
