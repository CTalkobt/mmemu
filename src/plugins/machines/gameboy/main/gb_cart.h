#pragma once

// Game Boy cartridges: ROM only, MBC1 (and the MBC1M multicart wiring), MBC2,
// MBC3 (with the clock and MBC30) and MBC5, after Mesen2 (Core/Gameboy/Carts/,
// GbCartFactory.h, GameboyHeader.h; GPL-3.0, Sour and the Mesen contributors).
//
// As in Mesen a mapper only lays out the CPU's 256-byte pages (Gameboy::map)
// and says which ones its own registers answer (Gameboy::mapRegisters).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gb {

class Gameboy;

/** The cartridge header at $0134-$014F. */
struct Header {
    std::string title;
    uint8_t cgbFlag = 0, sgbFlag = 0, cartType = 0, romSize = 0, ramSize = 0;
    /** The cartridge RAM in bytes (MBC2: its 512 nibbles). */
    uint32_t cartRamSize() const;
    bool hasBattery() const;
    static Header parse(const std::vector<uint8_t>& rom);
};

/** The MBC3's clock (Mesen's GbMbc3Rtc): seconds, minutes, hours, the day count, halt and carry. */
class Mbc3Rtc {
public:
    void reset();
    /** Let `cycles` Game Boy clocks (4.19 MHz) pass. */
    void tick(uint64_t cycles);
    void runForDuration(int64_t seconds);
    void latch() { std::copy(m_regs, m_regs + 5, m_latched); }
    uint8_t read(uint8_t reg) const;
    void write(uint8_t reg, uint8_t v);
    /** Its registers and the host time they were saved at (Mesen's .rtc file: 5 bytes and a big-endian ms count). */
    std::vector<uint8_t> state() const;
    void setState(const std::vector<uint8_t>& s);
private:
    bool running() const { return !(m_regs[4] & 0x40); }
    uint8_t m_regs[5] = {}, m_latched[5] = {};
    uint64_t m_ticks = 0;          // Game Boy clocks into the current second
};

/** A cartridge: the plain ROM-only one, and the base of the mappers. */
class Cart {
public:
    virtual ~Cart() = default;
    void attach(Gameboy* gb) { m_gb = gb; }
    /** At power on: which pages are the mapper's registers. */
    virtual void initCart() {}
    virtual void refreshMappings();
    virtual uint8_t readRegister(uint16_t) { return 0; }
    virtual void writeRegister(uint16_t, uint8_t) {}
    virtual Mbc3Rtc* rtc() { return nullptr; }
    /** "ROM", "MBC1", ... */
    virtual const char* name() const { return "ROM"; }

    /** The mapper for this header (null: not supported, `err` says why). */
    static std::unique_ptr<Cart> create(const Header& h, const std::vector<uint8_t>& rom, std::string& err);
protected:
    Gameboy* m_gb = nullptr;
};

}  // namespace gb
