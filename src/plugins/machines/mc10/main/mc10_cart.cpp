// The MC-10's expansion port devices: the 16K RAM pack, program packs and
// the MCX-128.

#include "mc10_cart.h"
#include <algorithm>
#include <cstring>

namespace mc10 {

bool RamPack::read(uint16_t a, uint8_t& v) {
    if (a < 0x5000 || a >= 0x9000) return false;
    v = m_ram[a - 0x5000];
    return true;
}

bool RamPack::write(uint16_t a, uint8_t v) {
    if (a < 0x5000 || a >= 0x9000) return false;
    m_ram[a - 0x5000] = v;
    return true;
}

bool RomPak::read(uint16_t a, uint8_t& v) {
    if (a < 0x5000 || a >= 0x5000 + m_rom.size()) return false;
    v = m_rom[a - 0x5000];
    return true;
}

bool RomPak::write(uint16_t a, uint8_t) {
    return a >= 0x5000 && a < 0x5000 + m_rom.size();
}

// ---------------------------------------------------------------------------
// MCX-128
// ---------------------------------------------------------------------------
Mcx128::Mcx128(std::vector<uint8_t> rom) : m_ram(0x20000, 0), m_rom(std::move(rom)) {
    m_rom.resize(0x4000, 0xFF);
}

std::string Mcx128::describe() const {
    return "mcx128 (128K RAM, bank " + std::to_string(m_bankCr & 3) + ", ROM map " + std::to_string(m_romCr & 3) + ")";
}

long Mcx128::ramOffset(uint16_t a, bool write) const {
    const long b0 = (m_bankCr & 1) ? 0x10000 : 0, b1 = (m_bankCr & 2) ? 0x10000 : 0;
    const int map = m_romCr & 3;
    if ((a >= 0x0004 && a <= 0x0007) || a == 0x000F || (a >= 0x0020 && a < 0x4000)) return b0 + a;
    if (a >= 0x4000 && a < 0xBF00) {
        if (b1) return 0x18000 + (a - 0x4000);                 // all RAM, bank 1
        if (a < 0x6000) return 0x8000 + (a - 0x4000);          // under the internal RAM
        return 0xA000 + (a - 0x6000);
    }
    if (a >= 0xC000 && a < 0xFF00) {
        const bool low = a < 0xE000;
        if (map == 3 || (low && map != 0)) return b0 + 0x4000 + (a - 0xC000);
        if (write) return b0 + (low ? 0x4000 + (a - 0xC000) : 0x6000 + (a - 0xE000));   // under the ROM
        return -1;
    }
    if (a >= 0xFF00) {
        if (map == 3 || write) return 0x7F00 + (a - 0xFF00);   // the same page in both banks
        return -1;
    }
    return -1;
}

bool Mcx128::read(uint16_t a, uint8_t& v) {
    if (a == 0xBF00) { v = m_bankCr & 3; return true; }
    if (a == 0xBF01) { v = m_romCr & 3; return true; }
    const int map = m_romCr & 3;
    if (a >= 0xC000 && ((a < 0xFF00 && (map == 0 || (map == 1 && a >= 0xE000))) || (a >= 0xFF00 && map <= 1))) {
        // The EPROM: all of it at $C000 in map 0; its upper half at $E000 in map 1; its last page at $FF00
        v = m_rom[a >= 0xFF00 ? 0x3F00 + (a - 0xFF00) : map == 0 ? a - 0xC000 : 0x2000 + (a - 0xE000)];
        return true;
    }
    const long o = ramOffset(a, false);
    if (o < 0) return false;
    v = m_ram[(size_t)o];
    return true;
}

bool Mcx128::write(uint16_t a, uint8_t v) {
    if (a == 0xBF00) { m_bankCr = v; return true; }
    if (a == 0xBF01) { m_romCr = v; return true; }
    const long o = ramOffset(a, true);
    if (o < 0) return false;
    m_ram[(size_t)o] = v;
    // With bank 1 clear, $4000-$5FFF writes reach the internal RAM too (the video)
    if (!(m_bankCr & 2) && a >= 0x4000 && a < 0x6000 && writeThrough) writeThrough(a, v);
    return true;
}

std::vector<uint8_t> Mcx128::saveState() const {
    std::vector<uint8_t> s(m_ram);
    s.push_back(m_bankCr);
    s.push_back(m_romCr);
    return s;
}

void Mcx128::loadState(const std::vector<uint8_t>& s) {
    if (s.size() != m_ram.size() + 2) return;
    std::copy(s.begin(), s.begin() + (long)m_ram.size(), m_ram.begin());
    m_bankCr = s[m_ram.size()];
    m_romCr = s[m_ram.size() + 1];
}

}  // namespace mc10
