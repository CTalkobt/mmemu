#include "c64_pla.h"

// The memory map of each of the 32 configurations (LORAM | HIRAM << 1 |
// CHAREN << 2 | EXROM << 3 | GAME << 4), one bit per configuration, from
// VICE's c64meminit.c / c64mem.c (GPL-2.0-or-later, VICE team).
namespace {
constexpr uint32_t KERNAL  = 0xCC00CCCC;  // 2,3,6,7, 10,11,14,15, 26,27,30,31
constexpr uint32_t BASIC   = 0x00008888;  // 3,7, 11,15
constexpr uint32_t CHARROM = 0x0C000E0E;  // 1,2,3, 9,10,11, 26,27
constexpr uint32_t IO      = 0xE0FFE0E0;  // 5,6,7, 13,14,15, 16-23 (Ultimax), 29,30,31
constexpr uint32_t ROML    = 0x88FF8800;  // 11,15, 16-23, 27,31
constexpr uint32_t ROMH_A  = 0xCC000000;  // 26,27,30,31: ROMH at $A000
constexpr uint32_t ULTIMAX = 0x00FF0000;  // 16-23: ROMH at $E000, open areas

inline bool in(uint32_t set, int config) { return (set >> config) & 1; }
inline uint8_t romByte(const uint8_t* rom, uint32_t size, uint32_t off) {
    return rom && off < size ? rom[off] : 0xFF;
}
}  // namespace

void C64PLA::reset() {
    // The port lines float high at reset (LORAM=HIRAM=CHAREN=1) through the
    // 6510's pull-ups; the cartridge lines stay as the cartridge wires them.
}

bool C64PLA::ioRead(IBus* /*bus*/, uint32_t addr, uint8_t* val) {
    addr &= 0xFFFF;
    if (addr < 0x1000) return false;
    const int c = config();
    const bool ultimax = in(ULTIMAX, c);
    auto ram = [&]() -> bool {
        if (!m_ram) return false;
        *val = m_ram[addr];
        return true;
    };

    if (addr < 0x8000) {
        if (ultimax) { *val = 0xFF; return true; }         // open
        return ram();
    }
    if (addr < 0xA000) {
        if (in(ROML, c)) { *val = romByte(m_roml, m_romlSize, addr - 0x8000); return true; }
        return ram();
    }
    if (addr < 0xC000) {
        if (in(BASIC, c))  { *val = romByte(m_basicRom, m_basicSize, addr - 0xA000); return true; }
        if (in(ROMH_A, c)) { *val = romByte(m_romh, m_romhSize, addr - 0xA000); return true; }
        if (ultimax) { *val = 0xFF; return true; }
        return ram();
    }
    if (addr < 0xD000) {
        if (ultimax) { *val = 0xFF; return true; }
        return ram();
    }
    if (addr < 0xE000) {
        if (in(IO, c)) return false;                        // the I/O chips answer
        if (in(CHARROM, c) && m_charRom) {
            *val = m_charRom[(addr - 0xD000) & (m_charSize - 1)];
            return true;
        }
        return ram();
    }
    if (in(KERNAL, c)) { *val = romByte(m_kernalRom, m_kernalSize, addr - 0xE000); return true; }
    if (ultimax) { *val = romByte(m_romh, m_romhSize, addr - 0xE000); return true; }
    return ram();
}

bool C64PLA::ioWrite(IBus* /*bus*/, uint32_t addr, uint8_t val) {
    addr &= 0xFFFF;
    if (addr < 0x1000) return false;
    const int c = config();
    if (in(ULTIMAX, c)) {
        // Only $0000-$0FFF and the I/O are there; ROML/ROMH writes go to the
        // cartridge (nothing for a plain ROM one)
        return !(addr >= 0xD000 && addr < 0xE000);
    }
    if (addr >= 0xD000 && addr < 0xE000 && !in(IO, c) && m_ram) {
        m_ram[addr] = val;                                  // RAM under the character ROM
        return true;
    }
    return false;                                           // to RAM, or the I/O chips
}
