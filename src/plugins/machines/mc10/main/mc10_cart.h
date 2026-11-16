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
    /** A write the 6803 makes to its own RAM or registers: the chip keeps
     *  it, but the cycle is on the bus too (no SEL: the chip ignores it). */
    virtual void internalWrite(uint16_t, uint8_t) {}
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
 * Darren Atkinson's MCX-128: 128 KB of SRAM in two 64 KB banks and a 16 KB
 * EPROM with MCX BASIC (mcx128bas.rom), decoded by a GAL22V10 as its
 * equations (MCX128.pld) give it. $BF00-$BF7F are the registers (even:
 * the RAM banks, P0 for $0000-$3FFF and $C000-$FEFF, P1 for $4000-$BEFF;
 * odd: the ROM map, 0 all EPROM at $C000, 1 RAM at $C000 and the EPROM's
 * upper half at $E000, 2 RAM at $C000 and the MC-10's ROM at $E000, 3 all
 * RAM); writes to $C000-$FFFF always reach the RAM; $FF00-$FFFF is bank 0's.
 * The MC-10's own devices keep $4000-$4FFF (while P1 is 0), the keyboard
 * and VDG at $BF80-$BFFF, and $E000-$FFFF reads in map 2. Writes to the
 * 6803's own RAM and registers are on the bus too, so they land in the
 * SRAM under them ($0000-$00FF): with RAME cleared BASIC finds them there.
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
    void internalWrite(uint16_t a, uint8_t v) override;
    /** The SRAM (128 KB: bank 1 at $10000). */
    uint8_t sram(uint32_t off) const { return m_ram[off & 0x1FFFF]; }
private:
    /** The MC-10's own decoding keeps the cycle (SEL high). */
    bool internal(uint16_t a, bool write) const;
    bool romSelected(uint16_t a) const;
    /** The SRAM offset for `a` (-1: not selected). */
    long ramOffset(uint16_t a, bool write) const;
    std::vector<uint8_t> m_ram, m_rom;
    uint8_t m_bankCr = 0, m_romCr = 0;
};

}  // namespace mc10
