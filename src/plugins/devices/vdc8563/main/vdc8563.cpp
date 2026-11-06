// MOS 8563 VDC register and memory interface, after VICE (src/vdc/vdc-mem.c,
// vdc.c), GPL-2.0-or-later, VICE team.

#include "vdc8563.h"
#include <cstring>

namespace {
// Unused register bits read 1 (vdc-mem.c regmask)
const uint8_t REGMASK[38] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x00, 0x00,
    0xFC, 0xE0, 0x80, 0xE0, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE0,
    0x00, 0x00, 0x00, 0x00, 0x0F, 0xE0, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F
};
constexpr uint8_t REVISION = 2;          // VICE's default (8563 R9)
}  // namespace

Vdc8563::Vdc8563(bool ram64k) : m_ram(ram64k ? 0x10000 : 0x4000, 0) {
    Vdc8563::reset();
}

void Vdc8563::reset() {
    std::memset(m_regs, 0, sizeof m_regs);
    m_index = 0;
    m_frameCycle = 0;
}

// R28 bit 4 selects 64 KB addressing; translate when it does not match the
// fitted chips (vdc_64k_to_16k_map / vdc_16k_to_64k_map)
uint32_t Vdc8563::map(uint16_t addr) const {
    const bool addr64 = m_regs[28] & 0x10;
    if (ram64k()) {
        if (addr64) return addr;
        return (addr & 0x80FF) | ((addr & 0x3F00) << 1) | (addr & 0x0100);
    }
    if (addr64) return (addr & 0x00FF) | ((addr & 0x7E00) >> 1);
    return addr & 0x3FFF;
}

void Vdc8563::writeData() {
    uint16_t ptr = (uint16_t)(m_regs[18] << 8 | m_regs[19]);
    m_ram[map(ptr)] = m_regs[31];
    ++ptr;
    m_regs[18] = ptr >> 8;
    m_regs[19] = ptr & 0xFF;
}

void Vdc8563::fillCopy() {
    const int len = m_regs[30] ? m_regs[30] : 256;
    uint16_t ptr = (uint16_t)(m_regs[18] << 8 | m_regs[19]);
    if (m_regs[24] & 0x80) {                               // copy
        uint16_t src = (uint16_t)(m_regs[32] << 8 | m_regs[33]);
        for (int i = 0; i < len; ++i) m_ram[map((uint16_t)(ptr + i))] = m_ram[map((uint16_t)(src + i))];
        src = (uint16_t)(src + len);
        m_regs[31] = m_ram[map((uint16_t)(src - 1))];
        m_regs[32] = src >> 8;
        m_regs[33] = src & 0xFF;
    } else {                                               // fill
        for (int i = 0; i < len; ++i) m_ram[map((uint16_t)(ptr + i))] = m_regs[31];
    }
    ptr = (uint16_t)(ptr + len);
    m_regs[18] = ptr >> 8;
    m_regs[19] = ptr & 0xFF;
}

// Vertical blank: until the display is emulated, the last 20% of a 50/60 Hz frame
uint8_t Vdc8563::status() const {
    const uint64_t frame = m_clockHz / (m_pal ? 50 : 60);
    const bool blank = frame && m_frameCycle >= frame - frame / 5;
    return 0x80 | (blank ? 0x20 : 0) | REVISION;
}

void Vdc8563::tick(uint64_t cycles) {
    const uint64_t frame = m_clockHz / (m_pal ? 50 : 60);
    if (!frame) return;
    m_frameCycle = (m_frameCycle + cycles) % frame;
}

bool Vdc8563::ioPeek(IBus*, uint32_t addr, uint8_t* val) {
    if (!(addr & 1)) { *val = status(); return true; }
    if (m_index == 31) { *val = m_ram[map((uint16_t)(m_regs[18] << 8 | m_regs[19]))]; return true; }
    *val = m_index < 38 ? (uint8_t)(m_regs[m_index] | REGMASK[m_index]) : 0xFF;
    return true;
}

bool Vdc8563::ioRead(IBus* bus, uint32_t addr, uint8_t* val) {
    if ((addr & 1) && m_index == 31) {                     // RAM, then advance
        uint16_t ptr = (uint16_t)(m_regs[18] << 8 | m_regs[19]);
        *val = m_ram[map(ptr)];
        ++ptr;
        m_regs[18] = ptr >> 8;
        m_regs[19] = ptr & 0xFF;
        return true;
    }
    return ioPeek(bus, addr, val);
}

bool Vdc8563::ioWrite(IBus*, uint32_t addr, uint8_t val) {
    if (!(addr & 1)) { m_index = val & 0x3F; return true; }
    if (m_index >= 38) return true;
    m_regs[m_index] = val;
    if (m_index == 30) fillCopy();
    else if (m_index == 31) writeData();
    return true;
}
