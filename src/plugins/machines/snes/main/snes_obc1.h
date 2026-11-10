#pragma once

// The OBC1 in Metal Combat: Falcon's Revenge (#15).
//
// After Mesen2 (Core/SNES/Coprocessors/OBC1/), GPL-3.0, Sour and the Mesen
// contributors. A sprite helper over the cartridge's battery RAM at
// $6000-$7FFF ($00-$3F/$80-$BF): two OAM-shaped tables in that RAM ($1800
// or $1C00, chosen by $7FF5 bit 0), an index into them ($7FF6), and ports
// for the chosen sprite's four bytes ($7FF0-$7FF3) and its two high bits
// ($7FF4), so the game needn't work out the addresses or masks.

#include <cstdint>
#include <vector>

namespace snes {

class Obc1 {
public:
    /** `ram` belongs to the cartridge (a power of two, not empty). */
    explicit Obc1(std::vector<uint8_t>* ram) : m_ram(ram) {}

    /** $6000-$7FFF: `a` is the address in the bank. */
    uint8_t read(uint16_t a) const;
    void write(uint16_t a, uint8_t v);

private:
    uint8_t ram(uint16_t a) const { return (*m_ram)[a & (m_ram->size() - 1)]; }
    void setRam(uint16_t a, uint8_t v) { (*m_ram)[a & (m_ram->size() - 1)] = v; }
    // $1FF5 bit 0: 0 = the table at $1C00, 1 = at $1800
    uint16_t base() const { return (uint16_t)(0x1800 | ((~ram(0x1FF5) & 0x01) << 10)); }
    uint16_t low() const { return (uint16_t)(base() | ((ram(0x1FF6) & 0x7F) << 2)); }
    uint16_t high() const { return (uint16_t)((base() | ((ram(0x1FF6) & 0x7F) >> 2)) + 0x200); }

    std::vector<uint8_t>* m_ram;
};

}  // namespace snes
