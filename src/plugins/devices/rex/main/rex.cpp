// AMD-style flash and the REX / REX# option-socket cards (see rex.h).
// mmsim's own code.

#include "rex.h"
#include <algorithm>
#include <cstring>
#include <fstream>

// ---------------------------------------------------------------------------
// AmdFlash
// ---------------------------------------------------------------------------
void AmdFlash::eraseSector(uint32_t addr) {
    uint32_t off, size;
    if (addr < 0x10000) {
        if (addr < 0x4000) { off = 0; size = 0x4000; }
        else if (addr < 0x8000) { off = addr & 0xE000; size = 0x2000; }
        else { off = 0x8000; size = 0x8000; }
    } else {
        off = addr & 0xFF0000;
        size = 0x10000;
    }
    if (off >= m_data.size()) return;
    std::fill(m_data.begin() + off, m_data.begin() + std::min<size_t>(off + size, m_data.size()), 0xFF);
    m_dirty = true;
}

void AmdFlash::write(uint32_t addr, uint8_t v, uint64_t now) {
    const uint8_t lo = addr & 0xFF;
    if (v == 0xF0 && m_state != PROG && m_state != BYPASS_PROG) {
        m_state = RO;
        return;
    }
    auto program = [&]() {
        uint8_t& cell = m_data[addr % m_data.size()];
        const uint8_t was = cell;
        cell &= v;                          // a program only clears bits
        if (cell != was) m_dirty = true;
        m_busyUntil = now + PROGRAM_CLOCKS;
    };
    switch (m_state) {
    case RO:
        if (v == 0x98 && addr == 0xAA) m_state = CFI;
        else if (v == 0xAA && lo == 0xAA) m_state = UNLK1;
        break;
    case UNLK1: m_state = (v == 0x55 && lo == 0x55) ? CMD : RO; break;
    case CMD:
        if (lo != 0xAA) { m_state = RO; break; }
        switch (v) {
        case 0xA0: m_state = PROG; break;
        case 0x20: m_state = BYPASS; break;
        case 0x80: m_state = UNLK2; break;
        case 0x90: m_state = AUTOSELECT; break;
        default: m_state = RO; break;
        }
        break;
    case UNLK2: m_state = (v == 0xAA && lo == 0xAA) ? UNLK3 : RO; break;
    case UNLK3: m_state = (v == 0x55 && lo == 0x55) ? CMD2 : RO; break;
    case CMD2:
        m_state = RO;
        if (v == 0x10 && lo == 0xAA) {
            std::fill(m_data.begin(), m_data.end(), 0xFF);
            m_dirty = true;
            m_busyUntil = now + CHIP_ERASE_CLOCKS;
        } else if (v == 0x30) {
            eraseSector(addr);
            m_busyUntil = now + SECTOR_ERASE_CLOCKS;
        }
        break;
    case PROG:
        program();
        m_state = RO;
        break;
    case BYPASS:
        if (v == 0xA0) m_state = BYPASS_PROG;
        else if (v == 0x90) m_state = BYPASS_EXIT;
        break;
    case BYPASS_PROG:
        program();
        m_state = BYPASS;
        break;
    case BYPASS_EXIT: m_state = v == 0x00 ? RO : BYPASS; break;
    default:                                // AUTOSELECT, CFI: only F0 leaves (above)
        break;
    }
}

// ---------------------------------------------------------------------------
// Rex
// ---------------------------------------------------------------------------
namespace {
const uint8_t KEY[6] = {0xB8, 0xF2, 0x34, 0xB0, 0x31, 0xBF};
}

Rex::~Rex() { flush(); }

bool Rex::open(const std::string& path) {
    flush();
    auto& d = m_flash.data();
    std::fill(d.begin(), d.end(), 0xFF);
    if (!path.empty()) {
        std::ifstream f(path, std::ios::binary);
        if (f) {
            f.read(reinterpret_cast<char*>(d.data()), (std::streamsize)d.size());
        } else {
            std::ofstream make(path, std::ios::binary);       // a new card: erased
            if (!make) return false;
            make.write(reinterpret_cast<const char*>(d.data()), (std::streamsize)d.size());
        }
    }
    m_path = path;
    m_flash.clearDirty();
    m_flash.reset();
    reset();
    return true;
}

bool Rex::saveTo(const std::string& path) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const auto& d = m_flash.data();
    f.write(reinterpret_cast<const char*>(d.data()), (std::streamsize)d.size());
    return (bool)f;
}

bool Rex::flush() {
    if (!m_flash.dirty() || m_path.empty()) return true;
    if (!saveTo(m_path)) return false;
    m_flash.clearDirty();
    return true;
}

void Rex::reset() {
    m_state = 7;
    m_key = 0;
    m_sector = 0;
    m_flashSel = true;
    m_pa = 0;
    m_ret = 0xFF;
}

uint8_t Rex::peek(uint16_t addr) const {
    if (m_state == 7) return m_flash.read(m_sector | (addr & 0x7FFF));
    if (m_state == 3 || m_state == 8) return m_ret;
    return 0xFF;
}

uint8_t Rex::read(uint16_t addr, uint64_t now) {
    addr &= 0x7FFF;
    const uint8_t lo = addr & 0xFF;
    const bool sharp = m_kind == Kind::RexSharp;
    switch (m_state) {
    case 7:                                 // reading, watching for the key
        if (lo == KEY[m_key]) {
            if (++m_key == 6) { m_key = 0; m_state = 0; }
        } else {
            m_key = 0;                      // VirtualT: this read does not restart the key
        }
        return m_flash.read(m_sector | addr);
    case 0:                                 // a command
        switch (addr & 7) {
        case 0: m_state = 7; m_key = 0; break;
        case 3:
            m_ret = (uint8_t)((m_flash.busy(now) ? 0 : 0x80) | (m_flashSel ? 0x40 : 0) | (m_sector >> 15));
            m_state = 3;
            return m_ret;
        case 7:
            m_ret = (uint8_t)(0x10 | model());
            m_state = 3;
            return m_ret;
        case 2: case 5: case 6:
            if (sharp) { m_state = 2; m_sharpCmd = addr & 7; break; }
            m_state = addr & 7;
            m_pa = lo;
            break;
        default:                            // 1, 4
            m_state = addr & 7;
            m_pa = lo;
            break;
        }
        return 0xFF;
    case 1:                                 // the sector
        m_sector = (uint32_t)(addr & 0x1F) << 15;
        m_flashSel = !(addr & 0x40);
        m_state = 0;
        return 0xFF;
    case 2:
        if (sharp) {                        // REX#: the data first
            m_sharpData = lo;
            m_state = 6;
        } else {                            // the data: low byte of this read
            m_flash.write(m_sector | m_pa, lo, now);
            m_state = 0;
        }
        return 0xFF;
    case 3:                                 // the status / ID again
        m_state = 0;
        return m_ret;
    case 4:
        m_ret = m_flashSel ? m_flash.read(m_sector | addr) : 0xFF;
        m_state = 8;
        return m_ret;
    case 8:
        m_state = 0;
        return m_ret;
    case 5:
        m_flash.write(0x55, 0x55, now);
        m_state = 0;
        return 0xFF;
    case 6:
        if (sharp) {                        // REX#: now the address
            const uint32_t pa = m_sharpCmd == 6 ? (m_sector | addr) : (m_sector | lo);
            m_flash.write(pa, m_sharpData, now);
            m_state = 0;
        } else {
            m_pa = addr;                    // the program address; the data next
            m_state = 2;
        }
        return 0xFF;
    default:
        m_state = 7;
        return 0xFF;
    }
}

std::vector<uint8_t> Rex::saveState() const {
    std::vector<uint8_t> v(m_flash.data());
    auto put32 = [&v](uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((uint8_t)(x >> (8 * i))); };
    put32((uint32_t)m_state); put32((uint32_t)m_key); put32(m_sector); put32(m_flashSel);
    put32(m_pa); put32(m_ret); put32(m_sharpData); put32((uint32_t)m_sharpCmd);
    return v;
}

void Rex::loadState(const std::vector<uint8_t>& s) {
    if (s.size() != SIZE + 32) return;
    std::memcpy(m_flash.data().data(), s.data(), SIZE);
    const uint8_t* p = s.data() + SIZE;
    auto get32 = [&p]() { uint32_t x = 0; for (int i = 0; i < 4; ++i) x |= (uint32_t)*p++ << (8 * i); return x; };
    m_state = (int)get32(); m_key = (int)get32(); m_sector = get32(); m_flashSel = get32() != 0;
    m_pa = get32(); m_ret = (uint8_t)get32(); m_sharpData = (uint8_t)get32(); m_sharpCmd = (int)get32();
    m_flash.reset();
}
