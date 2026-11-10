// The S-DD1 after Mesen2 (Core/SNES/Coprocessors/SDD1/Sdd1.cpp, Sdd1Mmc.cpp),
// GPL-3.0, Sour and the Mesen contributors.

#include "snes_sdd1.h"

#include <cstring>

namespace snes {

Sdd1::Sdd1(const std::vector<uint8_t>* rom, std::vector<uint8_t>* sram) : m_rom(rom), m_sram(sram) {
    m_reader = [this](uint32_t addr) { return romRead(addr); };
    reset();
}

void Sdd1::reset() {
    m_allowDma = m_nextDma = 0;
    for (int i = 0; i < 4; ++i) m_banks[i] = (uint8_t)i;
    std::memset(m_dmaAddr, 0, sizeof m_dmaAddr);
    std::memset(m_dmaLen, 0, sizeof m_dmaLen);
    m_needInit = true;
}

uint32_t Sdd1::romOffset(uint32_t addr) const {
    if (!(addr & 0x400000)) {
        // LoROM; $4805/$4807 bit 7 makes $20-$3F ($A0-$BF) mirror $00-$1F ($80-$9F)
        const bool mirror = (addr & 0x800000) ? (m_banks[3] & 0x80) : (m_banks[1] & 0x80);
        const uint32_t bank = (addr >> 16) & (mirror ? 0x1F : 0x3F);
        return bank * 0x8000 + (addr & 0x7FFF);
    }
    // $C0-$FF: a 1 MB block for each 16 banks
    const uint8_t block = (uint8_t)((addr >> 20) - 0x0C);
    return (uint32_t)(m_banks[block & 3] & 0x0F) * 0x100000 + (addr & 0xFFFFF);
}

uint8_t Sdd1::romRead(uint32_t addr) const {
    return m_rom->empty() ? 0 : (*m_rom)[romOffset(addr & 0xFFFFFF) % m_rom->size()];
}

int Sdd1::cpuRead(uint8_t bank, uint16_t a, bool peek) {
    const uint32_t addr = (uint32_t)bank << 16 | a;
    if (!(bank & 0x40)) {
        if (a >= 0x4800 && a <= 0x4807) {
            switch (a & 7) {
            case 0: return m_allowDma;
            case 1: return m_nextDma;
            case 4: case 5: case 6: case 7: return m_banks[a & 3];
            default: return -1;
            }
        }
        if (a >= 0x6000 && a < 0x8000) return m_sram->empty() ? -1 : (*m_sram)[((uint32_t)(bank & 0x3F) * 0x2000 + (a - 0x6000u)) % m_sram->size()];
        if (a >= 0x8000) return romRead(addr);
        return -1;
    }
    if (bank >= 0x70 && bank <= 0x73) return m_sram->empty() ? -1 : (*m_sram)[((uint32_t)(bank - 0x70) << 16 | a) % m_sram->size()];
    if (bank < 0xC0) return -1;
    // A DMA channel reading its source address gets decompressed data
    const uint8_t active = m_nextDma & m_allowDma;
    if (active && !peek) {
        for (int i = 0; i < 8; ++i) {
            if (!(active & (1 << i)) || addr != m_dmaAddr[i]) continue;
            if (m_needInit) { m_decomp.Init(&m_reader, addr); m_needInit = false; }
            const uint8_t v = m_decomp.GetDecompressedByte();
            if (--m_dmaLen[i] == 0) { m_needInit = true; m_nextDma &= (uint8_t)~(1 << i); }
            return v;
        }
    }
    return romRead(addr);
}

bool Sdd1::cpuWrite(uint8_t bank, uint16_t a, uint8_t v) {
    if (!(bank & 0x40)) {
        if (a >= 0x4800 && a <= 0x4807) {
            switch (a & 7) {
            case 0: m_allowDma = v; break;
            case 1: m_nextDma = v; break;
            case 4: case 5: case 6: case 7: m_banks[a & 3] = v; break;
            default: break;
            }
            return true;
        }
        if (a >= 0x6000 && a < 0x8000) {
            if (!m_sram->empty()) (*m_sram)[((uint32_t)(bank & 0x3F) * 0x2000 + (a - 0x6000u)) % m_sram->size()] = v;
            return true;
        }
        return a >= 0x8000;
    }
    if (bank >= 0x70 && bank <= 0x73) {
        if (!m_sram->empty()) (*m_sram)[((uint32_t)(bank - 0x70) << 16 | a) % m_sram->size()] = v;
        return true;
    }
    return bank >= 0xC0;
}

void Sdd1::dmaRegWrite(uint16_t a, uint8_t v) {
    if (a < 0x4300 || a > 0x437A) return;
    const int ch = (a >> 4) & 7;
    switch (a & 0x0F) {
    case 0x2: m_dmaAddr[ch] = (m_dmaAddr[ch] & 0xFFFF00) | v; break;
    case 0x3: m_dmaAddr[ch] = (m_dmaAddr[ch] & 0xFF00FF) | (uint32_t)v << 8; break;
    case 0x4: m_dmaAddr[ch] = (m_dmaAddr[ch] & 0x00FFFF) | (uint32_t)v << 16; break;
    case 0x5: m_dmaLen[ch] = (uint16_t)((m_dmaLen[ch] & 0xFF00) | v); break;
    case 0x6: m_dmaLen[ch] = (uint16_t)((m_dmaLen[ch] & 0x00FF) | v << 8); break;
    default: break;
    }
}

}  // namespace snes
