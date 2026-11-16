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

// The GAL's equations (Darren Atkinson's MCX128.pld, as XRoar's mcx128.c
// has them): which of the internal devices, the EPROM and the SRAM a cycle
// selects, and the SRAM's A16
namespace {
bool intRam(uint16_t a) { return (a & 0xF000) == 0x4000; }             // $4000-$4FFF (the 4K board)
bool regAddrs(uint16_t a) { return (a & 0xFF80) == 0xBF00; }           // $BF00-$BF7F
bool kbdVdg(uint16_t a) { return (a & 0xFF80) == 0xBF80; }             // $BF80-$BFFF
}  // namespace

bool Mcx128::internal(uint16_t a, bool write) const {
    const int map = m_romCr & 3;
    return (intRam(a) && !(m_bankCr & 2)) || kbdVdg(a) || ((a & 0xE000) == 0xE000 && map == 2 && !write);
}

bool Mcx128::romSelected(uint16_t a) const {
    const int map = m_romCr & 3;
    return (a >= 0xC000 && map == 0) || ((a & 0xE000) == 0xE000 && map == 1);
}

long Mcx128::ramOffset(uint16_t a, bool write) const {
    const int page = a >> 14, map = m_romCr & 3;
    const bool io = (a & 0xFF00) == 0xBF00;
    const bool ram = page == 0 || (page == 1 && !internal(a, write)) || (page == 2 && !io) || (page == 3 && write) ||
                     ((a & 0xE000) == 0xC000 && map != 0) || ((a & 0xE000) == 0xE000 && map == 3);
    if (!ram) return -1;
    const bool p0 = m_bankCr & 1, p1 = m_bankCr & 2;
    const bool a16 = (page == 0 && p0) || ((page == 1 || page == 2) && p1) || (page == 3 && p0 && a < 0xFF00);
    return (a16 ? 0x10000 : 0) + a;
}

bool Mcx128::read(uint16_t a, uint8_t& v) {
    if (regAddrs(a)) { v = (uint8_t)((a & 1) ? (m_romCr & 3) : (m_bankCr & 3)); return true; }
    if (internal(a, false)) return false;
    if (romSelected(a)) { v = m_rom[a & 0x3FFF]; return true; }
    const long o = ramOffset(a, false);
    if (o < 0) { v = (uint8_t)a; return true; }                     // nothing drives the bus
    v = m_ram[(size_t)o];
    return true;
}

bool Mcx128::write(uint16_t a, uint8_t v) {
    if (regAddrs(a)) {
        if (a & 1) m_romCr = v & 3;
        else m_bankCr = v & 3;
        return true;
    }
    if (internal(a, true)) return false;
    const long o = ramOffset(a, true);
    if (o >= 0) m_ram[(size_t)o] = v;
    return true;
}

void Mcx128::internalWrite(uint16_t a, uint8_t v) {
    const long o = ramOffset(a, true);
    if (o >= 0) m_ram[(size_t)o] = v;
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
