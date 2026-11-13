#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mc10 {

/**
 * A device in the MC-10's expansion port, as XRoar's mc10_cart.c models
 * it: it sees every external bus cycle first, and asserts SEL to take the
 * cycle away from the machine's own decoding (RAM, the keyboard and VDG
 * latch, the ROM). /NMI is on the port too.
 */
class Cart {
public:
    virtual ~Cart() = default;
    /** A read cycle: true (and `v`) when the cartridge drives it (SEL). */
    virtual bool read(uint16_t addr, uint8_t& v) = 0;
    /** A write cycle: true when the cartridge takes it (SEL). */
    virtual bool write(uint16_t addr, uint8_t v) = 0;
    /** The debugger's read: no side effects. */
    virtual bool peek(uint16_t addr, uint8_t& v) { return read(addr, v); }
    virtual void reset() {}
    /** "ram16k", "rompak", "mcx128". */
    virtual std::string kind() const = 0;
    /** One line for `expansion` and the GUI. */
    virtual std::string describe() const = 0;
    virtual std::vector<uint8_t> saveState() const { return {}; }
    virtual void loadState(const std::vector<uint8_t>&) {}
    /** /NMI from the port. */
    std::function<void(bool)> setNmi;
    /** A write the cartridge passes on to the machine as well (the MCX-128's
     *  mirror of $4000-$5FFF into the internal RAM). */
    std::function<void(uint16_t, uint8_t)> writeThrough;
};

/** Tandy's 16K RAM Expansion Pack (26-3013): RAM at $5000-$8FFF, 20 KB in all. */
class RamPack : public Cart {
public:
    RamPack() : m_ram(0x4000, 0) {}
    bool read(uint16_t a, uint8_t& v) override;
    bool write(uint16_t a, uint8_t v) override;
    void reset() override {}
    std::string kind() const override { return "ram16k"; }
    std::string describe() const override { return "ram16k (16K RAM pack, $5000-$8FFF)"; }
    std::vector<uint8_t> saveState() const override { return m_ram; }
    void loadState(const std::vector<uint8_t>& s) override { if (s.size() == m_ram.size()) m_ram = s; }
    void clear() { std::fill(m_ram.begin(), m_ram.end(), 0); }
private:
    std::vector<uint8_t> m_ram;
};

/** A program pack: up to 16 KB of ROM at $5000 (as MAME's mc10 "pak"). */
class RomPak : public Cart {
public:
    RomPak(std::vector<uint8_t> rom, std::string label) : m_rom(std::move(rom)), m_label(std::move(label)) {
        if (m_rom.size() > 0x4000) m_rom.resize(0x4000);
    }
    bool read(uint16_t a, uint8_t& v) override;
    bool write(uint16_t a, uint8_t v) override;
    std::string kind() const override { return "rompak"; }
    std::string describe() const override { return "rompak " + m_label; }
private:
    std::vector<uint8_t> m_rom;
    std::string m_label;
};

/**
 * Darren Atkinson's MCX-128: 128 KB of RAM in two 64 KB banks and a 16 KB
 * EPROM with its extended BASIC (mcx128bas.rom). $BF00 selects the RAM
 * banks (bit 0: $0000-$3FFF and $C000-$FFFF; bit 1: $4000-$BEFF, with bit
 * 1 clear the internal RAM's range reads from bank 0 and writes go to both),
 * $BF01 the ROM map (0: EPROM at $C000-$FEFF, 1: RAM at $C000, EPROM's
 * second half at $E000, 2: RAM at $C000, the MC-10's ROM read at $E000
 * with writes to RAM, 3: all RAM); the EPROM's last page at $FF00 in maps 0
 * and 1. As MAME's mcx128.cpp maps it.
 */
class Mcx128 : public Cart {
public:
    explicit Mcx128(std::vector<uint8_t> rom);
    bool read(uint16_t a, uint8_t& v) override;
    bool write(uint16_t a, uint8_t v) override;
    bool peek(uint16_t a, uint8_t& v) override { return read(a, v); }
    void reset() override { m_bankCr = 0; m_romCr = 0; }
    std::string kind() const override { return "mcx128"; }
    std::string describe() const override;
    std::vector<uint8_t> saveState() const override;
    void loadState(const std::vector<uint8_t>& s) override;
    uint8_t bankControl() const { return m_bankCr; }
    uint8_t romControl() const { return m_romCr; }
private:
    /** The RAM offset for `a` (-1: not the cartridge's RAM in this map). */
    long ramOffset(uint16_t a, bool write) const;
    std::vector<uint8_t> m_ram, m_rom;
    uint8_t m_bankCr = 0, m_romCr = 0;
};

}  // namespace mc10
