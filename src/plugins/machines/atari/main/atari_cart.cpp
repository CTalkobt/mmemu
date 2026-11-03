#include "atari_cart.h"
#include <cstring>

// Bank switching after atari800's cartridge.c (access_D5, PutByte, the
// set_bank_* helpers): the state each access sets and what it maps.

namespace atari {

namespace {

struct TypeInfo { int type; int kb; const char* name; };

const TypeInfo TYPES[] = {
    {Cartridge::STD_8, 8, "Standard 8 KB"},
    {Cartridge::STD_16, 16, "Standard 16 KB"},
    {Cartridge::OSS_034M_16, 16, "OSS two chip 16 KB (034M)"},
    {Cartridge::DB_32, 32, "DB 32 KB"},
    {Cartridge::WILL_64, 64, "Williams 64 KB"},
    {Cartridge::EXP_64, 64, "Express 64 KB"},
    {Cartridge::DIAMOND_64, 64, "Diamond 64 KB"},
    {Cartridge::SDX_64, 64, "SpartaDOS X 64 KB"},
    {Cartridge::XEGS_32, 32, "XEGS 32 KB"},
    {Cartridge::XEGS_07_64, 64, "XEGS 64 KB"},
    {Cartridge::XEGS_128, 128, "XEGS 128 KB"},
    {Cartridge::OSS_M091_16, 16, "OSS one chip 16 KB"},
    {Cartridge::WILL_32, 32, "Williams 32 KB"},
    {Cartridge::XEGS_256, 256, "XEGS 256 KB"},
    {Cartridge::XEGS_512, 512, "XEGS 512 KB"},
    {Cartridge::XEGS_1024, 1024, "XEGS 1 MB"},
    {Cartridge::MEGA_16, 16, "MegaCart 16 KB"},
    {Cartridge::MEGA_32, 32, "MegaCart 32 KB"},
    {Cartridge::MEGA_64, 64, "MegaCart 64 KB"},
    {Cartridge::MEGA_128, 128, "MegaCart 128 KB"},
    {Cartridge::MEGA_256, 256, "MegaCart 256 KB"},
    {Cartridge::MEGA_512, 512, "MegaCart 512 KB"},
    {Cartridge::MEGA_1024, 1024, "MegaCart 1 MB"},
    {Cartridge::SWXEGS_32, 32, "Switchable XEGS 32 KB"},
    {Cartridge::SWXEGS_64, 64, "Switchable XEGS 64 KB"},
    {Cartridge::SWXEGS_128, 128, "Switchable XEGS 128 KB"},
    {Cartridge::SWXEGS_256, 256, "Switchable XEGS 256 KB"},
    {Cartridge::SWXEGS_512, 512, "Switchable XEGS 512 KB"},
    {Cartridge::SWXEGS_1024, 1024, "Switchable XEGS 1 MB"},
    {Cartridge::PHOENIX_8, 8, "Phoenix 8 KB"},
    {Cartridge::BLIZZARD_16, 16, "Blizzard 16 KB"},
    {Cartridge::ATMAX_128, 128, "Atarimax 128 KB"},
    {Cartridge::ATMAX_OLD_1024, 1024, "Atarimax 1 MB (old)"},
    {Cartridge::SDX_128, 128, "SpartaDOS X 128 KB"},
    {Cartridge::OSS_8, 8, "OSS 8 KB"},
    {Cartridge::OSS_043M_16, 16, "OSS two chip 16 KB (043M)"},
    {Cartridge::MEGA_2048, 2048, "MegaCart 2 MB"},
    {Cartridge::ATMAX_NEW_1024, 1024, "Atarimax 1 MB (new)"},
};

const TypeInfo* info(int type) {
    for (const auto& t : TYPES) if (t.type == type) return &t;
    return nullptr;
}

bool isXegs(int t) {
    return t == Cartridge::XEGS_32 || t == Cartridge::XEGS_07_64 || t == Cartridge::XEGS_128 ||
           t == Cartridge::XEGS_256 || t == Cartridge::XEGS_512 || t == Cartridge::XEGS_1024 ||
           (t >= Cartridge::SWXEGS_32 && t <= Cartridge::SWXEGS_1024) || t == Cartridge::DB_32;
}
bool isMega(int t) {
    return (t >= Cartridge::MEGA_16 && t <= Cartridge::MEGA_1024) || t == Cartridge::MEGA_2048;
}

}  // namespace

bool Cartridge::load(const std::vector<uint8_t>& bytes, std::string& err) {
    int type = 0;
    std::vector<uint8_t> rom;
    if (bytes.size() >= 16 && std::memcmp(bytes.data(), "CART", 4) == 0) {
        type = (bytes[4] << 24) | (bytes[5] << 16) | (bytes[6] << 8) | bytes[7];
        rom.assign(bytes.begin() + 16, bytes.end());
    } else if (bytes.size() == 8192) {
        type = STD_8;
        rom = bytes;
    } else if (bytes.size() == 16384) {
        type = STD_16;
        rom = bytes;
    } else {
        err = "Not a cartridge: a raw image must be 8 or 16 KB; others need a .car header.";
        return false;
    }
    const TypeInfo* ti = info(type);
    if (!ti) { err = "Unsupported cartridge type " + std::to_string(type) + "."; return false; }
    if (rom.size() < (size_t)ti->kb * 1024) {
        err = std::string(ti->name) + " cartridge: expected " + std::to_string(ti->kb) +
              " KB of ROM, got " + std::to_string(rom.size()) + " bytes.";
        return false;
    }
    rom.resize((size_t)ti->kb * 1024);
    m_type = type;
    m_rom = std::move(rom);
    reset();
    return true;
}

std::string Cartridge::typeName() const {
    const TypeInfo* ti = info(m_type);
    return ti ? ti->name : "none";
}

void Cartridge::reset() {
    switch (m_type) {
        case OSS_034M_16:    m_state = 1; break;
        case ATMAX_OLD_1024: m_state = 0x7F; break;
        default:             m_state = 0; break;
    }
}

bool Cartridge::rd4() const {
    switch (m_type) {
        case STD_16:      return true;
        case BLIZZARD_16: return m_state == 0;
        default:
            if (isXegs(m_type) || isMega(m_type)) return !(m_state & 0x80);
            return false;
    }
}

bool Cartridge::rd5() const {
    switch (m_type) {
        case 0: return false;
        case STD_8: case STD_16: return true;
        case PHOENIX_8: case BLIZZARD_16: return m_state == 0;
        case OSS_034M_16: case OSS_043M_16: case OSS_M091_16: case OSS_8: return m_state >= 0;
        case WILL_64: case WILL_32: case EXP_64: case DIAMOND_64: case SDX_64: case SDX_128:
            return !(m_state & 0x08);
        case ATMAX_128: return !(m_state & 0x10);
        case ATMAX_OLD_1024: case ATMAX_NEW_1024: return !(m_state & 0x80);
        default:
            if (isXegs(m_type) || isMega(m_type)) return !(m_state & 0x80);
            return false;
    }
}

uint8_t Cartridge::read(uint16_t addr) const {
    const size_t size = m_rom.size();
    if (addr < 0xA000) {                                // $8000-$9FFF
        size_t off = addr - 0x8000;
        if (m_type == STD_16 || m_type == BLIZZARD_16) return at(off);
        if (isXegs(m_type)) return at((size_t)(m_state & 0x7F) * 0x2000 + off);
        if (isMega(m_type)) return at((size_t)(m_state & 0x7F) * 0x4000 + off);
        return 0xFF;
    }
    size_t off = addr - 0xA000;                          // $A000-$BFFF
    switch (m_type) {
        case STD_8: case PHOENIX_8: return at(off);
        case STD_16: case BLIZZARD_16: return at(0x2000 + off);
        case OSS_034M_16: case OSS_043M_16: case OSS_M091_16: case OSS_8: {
            size_t fixed = (m_type == OSS_034M_16 || m_type == OSS_043M_16) ? 0x3000 : 0;
            if (off >= 0x1000) return at(fixed + off - 0x1000);   // $B000: always the same 4 KB
            if (m_state == 0xFF) return 0xFF;
            return at((size_t)m_state * 0x1000 + off);
        }
        case WILL_64: case WILL_32: case EXP_64: case DIAMOND_64: case SDX_64:
            return at((size_t)(m_state & 7) * 0x2000 + off);
        case SDX_128:
            return at((size_t)((m_state & 7) + ((m_state & 0x10) >> 1)) * 0x2000 + off);
        case ATMAX_128: return at((size_t)(m_state & 0x0F) * 0x2000 + off);
        case ATMAX_OLD_1024: case ATMAX_NEW_1024: return at((size_t)(m_state & 0x7F) * 0x2000 + off);
        default:
            if (isXegs(m_type)) return at(size - 0x2000 + off);   // the last bank, fixed
            if (isMega(m_type)) return at((size_t)(m_state & 0x7F) * 0x4000 + 0x2000 + off);
            return 0xFF;
    }
}

// Bank switching by the address of a $D5xx access (reads and writes)
void Cartridge::access(uint16_t addr) {
    const uint8_t a = (uint8_t)addr;
    switch (m_type) {
        case OSS_034M_16:
        case OSS_043M_16:
            if (a & 0x08) { m_state = -1; break; }
            switch (a & 0x07) {
                case 0x00: m_state = 0; break;
                case 0x03: case 0x07: m_state = m_type == OSS_034M_16 ? 1 : 2; break;
                case 0x04: m_state = m_type == OSS_034M_16 ? 2 : 1; break;
                default:   m_state = 0xFF; break;   // two chips at once, or none: $FF
            }
            break;
        case OSS_M091_16:
            switch (a & 0x09) {
                case 0x00: m_state = 1; break;
                case 0x01: m_state = 3; break;
                case 0x08: m_state = -1; break;
                default:   m_state = 2; break;
            }
            break;
        case OSS_8:
            switch (a & 0x09) {
                case 0x00: case 0x01: m_state = 1; break;
                case 0x08: m_state = -1; break;
                default:   m_state = 0; break;
            }
            break;
        case DB_32:    m_state = a & 0x03; break;
        case WILL_64:  m_state = a & 0x0F; break;
        case WILL_32:  m_state = a & 0x0B; break;
        case EXP_64:     if ((a & 0xF0) == 0x70) m_state = (a ^ 7) & 0x0F; break;
        case DIAMOND_64: if ((a & 0xF0) == 0xD0) m_state = (a ^ 7) & 0x0F; break;
        case SDX_64:
            if ((a & 0xF0) == 0xE0) m_state = (a & 0x08) ? (a & 0x0C) : ((a ^ 0x07) & 0x0F);
            break;
        case SDX_128:
            if ((a & 0xE0) == 0xE0) m_state = (a & 0x08) ? (a & 0x0C) : ((a ^ 0x17) & 0x1F);
            break;
        case PHOENIX_8: case BLIZZARD_16: m_state = 1; break;   // any access turns it off
        case ATMAX_128: if ((a & 0xE0) == 0) m_state = a & 0x1F; break;
        case ATMAX_OLD_1024: case ATMAX_NEW_1024: m_state = a; break;
        default: break;
    }
}

uint8_t Cartridge::readCctl(uint16_t addr) {
    access(addr);
    return 0xFF;
}

void Cartridge::writeCctl(uint16_t addr, uint8_t v) {
    switch (m_type) {
        case XEGS_32:     m_state = v & 0x03; return;
        case XEGS_07_64:  m_state = v & 0x07; return;
        case XEGS_128:    m_state = v & 0x0F; return;
        case XEGS_256:    m_state = v & 0x1F; return;
        case XEGS_512:    m_state = v & 0x3F; return;
        case XEGS_1024:   m_state = v & 0x7F; return;
        case MEGA_16:     m_state = v & 0x80; return;
        case MEGA_32:     m_state = v & 0x81; return;
        case MEGA_64:  case SWXEGS_32:  m_state = v & 0x83; return;
        case MEGA_128: case SWXEGS_64:  m_state = v & 0x87; return;
        case MEGA_256: case SWXEGS_128: m_state = v & 0x8F; return;
        case MEGA_512: case SWXEGS_256: m_state = v & 0x9F; return;
        case MEGA_1024: case SWXEGS_512: m_state = v & 0xBF; return;
        case MEGA_2048: case SWXEGS_1024: m_state = v; return;
        default: access(addr); return;
    }
}

}  // namespace atari
