// Game Boy cartridges, after Mesen2 (Core/Gameboy/Carts/, GbCartFactory.h, GameboyHeader.h),
// GPL-3.0, Sour and the Mesen contributors.

#include "gb_cart.h"
#include "gameboy.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace gb {

using Mem = Gameboy::Mem;

// ---------------------------------------------------------------------------
// The header
// ---------------------------------------------------------------------------

Header Header::parse(const std::vector<uint8_t>& rom) {
    Header h;
    if (rom.size() < 0x150) return h;
    for (int i = 0; i < 11; ++i) {
        const char c = (char)rom[0x134 + i];
        if (!c) break;
        h.title += (c >= 0x20 && c < 0x7F) ? c : ' ';
    }
    while (!h.title.empty() && h.title.back() == ' ') h.title.pop_back();
    h.cgbFlag = rom[0x143];
    h.sgbFlag = rom[0x146];
    h.cartType = rom[0x147];
    h.romSize = rom[0x148];
    h.ramSize = rom[0x149];
    return h;
}

uint32_t Header::cartRamSize() const {
    if (cartType == 0x05 || cartType == 0x06) return 0x200;      // the MBC2's 512 x 4 bits
    switch (ramSize) {
    case 1: return 0x800;
    case 2: return 0x2000;
    case 3: return 0x8000;
    case 4: return 0x20000;
    case 5: return 0x10000;
    default: return 0;
    }
}

bool Header::hasBattery() const {
    switch (cartType) {
    case 0x03: case 0x06: case 0x09: case 0x0D: case 0x0F: case 0x10: case 0x13: case 0x1B: case 0x1E: case 0x22: case 0xFF:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// ROM only
// ---------------------------------------------------------------------------

void Cart::refreshMappings() { m_gb->map(0x0000, 0x7FFF, Mem::Rom, 0, true); }

namespace {

// ---------------------------------------------------------------------------
// MBC1 (MBC1M: the multicart wiring, 4 bits of ROM bank)
// ---------------------------------------------------------------------------
class Mbc1 : public Cart {
public:
    explicit Mbc1(bool multicart) : m_multicart(multicart) {}
    const char* name() const override { return m_multicart ? "MBC1M" : "MBC1"; }
    void initCart() override {
        m_ramEnabled = false; m_prgBank = 1; m_ramBank = 0; m_mode = false;
        m_gb->mapRegisters(0x0000, 0x7FFF, Gameboy::ACC_WRITE);
    }
    void refreshMappings() override {
        const int shift = m_multicart ? 4 : 5, mask = m_multicart ? 0x0F : 0x1F;
        m_gb->map(0x0000, 0x3FFF, Mem::Rom, (uint32_t)(m_mode ? (m_ramBank << shift) : 0) * 0x4000, true);
        const uint8_t bank = (uint8_t)((m_prgBank & mask) | (m_ramBank << shift));
        m_gb->map(0x4000, 0x7FFF, Mem::Rom, (uint32_t)bank * 0x4000, true);
        if (m_ramEnabled) {
            m_gb->map(0xA000, 0xBFFF, Mem::CartRam, (uint32_t)(m_mode ? m_ramBank : 0) * 0x2000, false);
            m_gb->mapRegisters(0xA000, 0xBFFF, Gameboy::ACC_NONE);
        } else {
            m_gb->unmap(0xA000, 0xBFFF);
            m_gb->mapRegisters(0xA000, 0xBFFF, Gameboy::ACC_READ);
        }
    }
    uint8_t readRegister(uint16_t) override { return 0xFF; }      // RAM off
    void writeRegister(uint16_t addr, uint8_t v) override {
        switch (addr & 0x6000) {
        case 0x0000: m_ramEnabled = (v & 0x0F) == 0x0A; break;
        case 0x2000: m_prgBank = (uint8_t)std::max(1, v & 0x1F); break;
        case 0x4000: m_ramBank = v & 0x03; break;
        case 0x6000: m_mode = (v & 1) != 0; break;
        }
        refreshMappings();
    }
private:
    bool m_multicart;
    bool m_ramEnabled = false, m_mode = false;
    uint8_t m_prgBank = 1, m_ramBank = 0;
};

// ---------------------------------------------------------------------------
// MBC2: 16 ROM banks and 512 nibbles of RAM (the upper nibble reads as 1s)
// ---------------------------------------------------------------------------
class Mbc2 : public Cart {
public:
    const char* name() const override { return "MBC2"; }
    void initCart() override {
        m_ramEnabled = false; m_prgBank = 1;
        m_gb->mapRegisters(0x0000, 0x3FFF, Gameboy::ACC_WRITE);
        auto& ram = m_gb->cartRam();
        for (size_t i = 0; i < ram.size(); ++i) ram[i] |= 0xF0;
    }
    void refreshMappings() override {
        m_gb->map(0x0000, 0x3FFF, Mem::Rom, 0, true);
        m_gb->map(0x4000, 0x7FFF, Mem::Rom, (uint32_t)m_prgBank * 0x4000, true);
        if (m_ramEnabled) {
            for (int i = 0; i < 16; ++i) m_gb->map((uint16_t)(0xA000 + 0x200 * i), (uint16_t)(0xA1FF + 0x200 * i), Mem::CartRam, 0, false);
            m_gb->mapRegisters(0xA000, 0xBFFF, Gameboy::ACC_WRITE);
        } else {
            m_gb->unmap(0xA000, 0xBFFF);
            m_gb->mapRegisters(0xA000, 0xBFFF, Gameboy::ACC_READ);
        }
    }
    uint8_t readRegister(uint16_t) override { return 0xFF; }
    void writeRegister(uint16_t addr, uint8_t v) override {
        if (addr >= 0xA000 && addr <= 0xBFFF) {
            auto& ram = m_gb->cartRam();
            if (!ram.empty()) ram[addr & 0x1FF] = (uint8_t)((v & 0x0F) | 0xF0);
            return;
        }
        if (addr & 0x100) m_prgBank = (uint8_t)std::max(1, v & 0x0F);
        else m_ramEnabled = (v & 0x0F) == 0x0A;
        refreshMappings();
    }
private:
    bool m_ramEnabled = false;
    uint8_t m_prgBank = 1;
};

// ---------------------------------------------------------------------------
// MBC3 (with the clock; MBC30: 8 RAM banks)
// ---------------------------------------------------------------------------
class Mbc3 : public Cart {
public:
    Mbc3(bool hasRtc, bool mbc30) : m_hasRtc(hasRtc), m_mbc30(mbc30) {}
    const char* name() const override { return m_hasRtc ? "MBC3+RTC" : (m_mbc30 ? "MBC30" : "MBC3"); }
    Mbc3Rtc* rtc() override { return m_hasRtc ? &m_rtc : nullptr; }
    void initCart() override {
        m_enabled = false; m_prgBank = 1; m_ramBank = 0;
        m_lastCycle = 0;
        m_gb->mapRegisters(0x0000, 0x7FFF, Gameboy::ACC_WRITE);
    }
    void refreshMappings() override {
        m_gb->map(0x0000, 0x3FFF, Mem::Rom, 0, true);
        m_gb->map(0x4000, 0x7FFF, Mem::Rom, (uint32_t)m_prgBank * 0x4000, true);
        if (m_enabled && m_ramBank <= (m_mbc30 ? 7 : 3)) {
            m_gb->map(0xA000, 0xBFFF, Mem::CartRam, (uint32_t)m_ramBank * 0x2000, false);
            m_gb->mapRegisters(0xA000, 0xBFFF, Gameboy::ACC_NONE);
        } else if (m_hasRtc && m_enabled && m_ramBank >= 0x08 && m_ramBank <= 0x0C) {
            m_gb->mapRegisters(0xA000, 0xBFFF, Gameboy::ACC_RW);
        } else {
            m_gb->unmap(0xA000, 0xBFFF);
            m_gb->mapRegisters(0xA000, 0xBFFF, Gameboy::ACC_READ);
        }
    }
    uint8_t readRegister(uint16_t) override {
        if (!m_enabled || !m_hasRtc) return 0xFF;
        return m_rtc.read(m_ramBank & 0x0F);
    }
    void writeRegister(uint16_t addr, uint8_t v) override {
        if (addr <= 0x7FFF) {
            switch (addr & 0x6000) {
            case 0x0000: m_enabled = (v & 0x0F) == 0x0A; break;
            case 0x2000: m_prgBank = std::max<uint8_t>(1, v); break;
            case 0x4000: m_ramBank = v & 0x0F; break;
            case 0x6000: updateTime(); m_rtc.latch(); break;     // latched on every write (latch-rtc-test, as Mesen)
            }
            refreshMappings();
        } else if (addr >= 0xA000 && addr <= 0xBFFF) {
            updateTime();
            m_rtc.write(m_ramBank & 0x0F, v);
        }
    }
private:
    void updateTime() {
        const uint64_t now = m_gb->apuCycles();          // real time (the CPU's clock doubles in the CGB's double speed)
        if (now > m_lastCycle) m_rtc.tick(now - m_lastCycle);
        m_lastCycle = now;
    }
    bool m_hasRtc, m_mbc30;
    bool m_enabled = false;
    uint8_t m_prgBank = 1, m_ramBank = 0;
    uint64_t m_lastCycle = 0;
    Mbc3Rtc m_rtc;
};

// ---------------------------------------------------------------------------
// MBC5: 9 bits of ROM bank, 16 RAM banks (8 with the rumble motor)
// ---------------------------------------------------------------------------
class Mbc5 : public Cart {
public:
    explicit Mbc5(bool rumble) : m_rumble(rumble) {}
    const char* name() const override { return m_rumble ? "MBC5+rumble" : "MBC5"; }
    void initCart() override {
        m_ramEnabled = false; m_prgBank = 1; m_ramBank = 0;
        m_gb->mapRegisters(0x0000, 0x5FFF, Gameboy::ACC_WRITE);
    }
    void refreshMappings() override {
        m_gb->map(0x0000, 0x3FFF, Mem::Rom, 0, true);
        m_gb->map(0x4000, 0x7FFF, Mem::Rom, (uint32_t)m_prgBank * 0x4000, true);
        if (m_ramEnabled) {
            m_gb->map(0xA000, 0xBFFF, Mem::CartRam, (uint32_t)m_ramBank * 0x2000, false);
            m_gb->mapRegisters(0xA000, 0xBFFF, Gameboy::ACC_NONE);
        } else {
            m_gb->unmap(0xA000, 0xBFFF);
            m_gb->mapRegisters(0xA000, 0xBFFF, Gameboy::ACC_READ);
        }
    }
    uint8_t readRegister(uint16_t) override { return 0xFF; }
    void writeRegister(uint16_t addr, uint8_t v) override {
        switch (addr & 0x7000) {
        case 0x0000: case 0x1000: m_ramEnabled = v == 0x0A; break;
        case 0x2000: m_prgBank = (uint16_t)(v | (m_prgBank & 0x100)); break;
        case 0x3000: m_prgBank = (uint16_t)((m_prgBank & 0xFF) | ((v & 1) << 8)); break;
        case 0x4000: case 0x5000: m_ramBank = m_rumble ? (v & 0x07) : (v & 0x0F); break;
        }
        refreshMappings();
    }
private:
    bool m_rumble;
    bool m_ramEnabled = false;
    uint16_t m_prgBank = 1;
    uint8_t m_ramBank = 0;
};

}  // namespace

std::unique_ptr<Cart> Cart::create(const Header& h, const std::vector<uint8_t>& rom, std::string& err) {
    switch (h.cartType) {
    case 0x00:
        return std::make_unique<Cart>();
    case 0x01: case 0x02: case 0x03: {
        // The logo at both $0104 and $40104: a multicart's MBC1 wiring (as Mesen)
        const bool multicart = rom.size() > 0x40134 && std::memcmp(&rom[0x104], &rom[0x40104], 0x30) == 0;
        return std::make_unique<Mbc1>(multicart);
    }
    case 0x05: case 0x06:
        return std::make_unique<Mbc2>();
    case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13:
        return std::make_unique<Mbc3>(h.cartType <= 0x10, h.cartRamSize() >= 0x10000);
    case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E:
        return std::make_unique<Mbc5>(h.cartType >= 0x1C);
    default: {
        static const char* hex = "0123456789ABCDEF";
        err = std::string("Unsupported Game Boy cartridge type $") + hex[h.cartType >> 4] + hex[h.cartType & 15] +
              " (ROM, MBC1, MBC2, MBC3 and MBC5 are emulated)";
        return nullptr;
    }
    }
}

// ---------------------------------------------------------------------------
// The MBC3's clock (Mesen's GbMbc3Rtc)
// ---------------------------------------------------------------------------

void Mbc3Rtc::reset() {
    std::fill(m_regs, m_regs + 5, 0);
    std::fill(m_latched, m_latched + 5, 0);
    m_ticks = 0;
}

void Mbc3Rtc::tick(uint64_t cycles) {
    if (!running()) return;
    m_ticks += cycles;
    while (m_ticks >= Gameboy::CLOCK_HZ) {
        runForDuration(1);
        m_ticks -= Gameboy::CLOCK_HZ;
    }
}

void Mbc3Rtc::runForDuration(int64_t seconds) {
    while (seconds > 0) {
        const int64_t step = m_regs[0] >= 60 ? 1 : std::min<int64_t>(60 - m_regs[0], seconds);
        seconds -= step;
        m_regs[0] = (uint8_t)((m_regs[0] + step) & 0x3F);
        if (m_regs[0] != 60) continue;
        m_regs[0] = 0;
        m_regs[1] = (m_regs[1] + 1) & 0x3F;
        if (m_regs[1] != 60) continue;
        m_regs[1] = 0;
        m_regs[2] = (m_regs[2] + 1) & 0x1F;
        if (m_regs[2] != 24) continue;
        m_regs[2] = 0;
        const uint16_t days = (uint16_t)(m_regs[3] | ((m_regs[4] & 1) << 8));
        if (days == 0xFF) { m_regs[3] = 0; m_regs[4] |= 0x01; }
        else if (days == 0x1FF) { m_regs[3] = 0; m_regs[4] = (uint8_t)((m_regs[4] & ~0x01) | 0x80); }   // the carry
        else ++m_regs[3];
    }
}

uint8_t Mbc3Rtc::read(uint8_t reg) const { return reg >= 0x08 && reg <= 0x0C ? m_latched[reg - 0x08] : 0xFF; }

void Mbc3Rtc::write(uint8_t reg, uint8_t v) {
    switch (reg) {
    case 0x08: m_regs[0] = v & 0x3F; m_ticks = 0; break;     // the seconds restart
    case 0x09: m_regs[1] = v & 0x3F; break;
    case 0x0A: m_regs[2] = v & 0x1F; break;
    case 0x0B: m_regs[3] = v; break;
    case 0x0C: m_regs[4] = v & 0xC1; break;
    }
}

std::vector<uint8_t> Mbc3Rtc::state() const {
    std::vector<uint8_t> d(m_regs, m_regs + 5);
    uint64_t t = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch()).count();
    for (int i = 0; i < 8; ++i) d.push_back((uint8_t)(t >> (56 - 8 * i)));
    return d;
}

void Mbc3Rtc::setState(const std::vector<uint8_t>& d) {
    if (d.size() != 13) return;
    std::copy(d.begin(), d.begin() + 5, m_regs);
    uint64_t t = 0;
    for (int i = 0; i < 8; ++i) t = t << 8 | d[5 + i];
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t elapsed = now - (int64_t)t;
    if (elapsed > 0) runForDuration(elapsed / 1000);     // the time it was off (as Mesen)
}

}  // namespace gb
