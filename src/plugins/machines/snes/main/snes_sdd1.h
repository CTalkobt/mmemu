#pragma once

// The S-DD1 in Star Ocean and Street Fighter Alpha 2 (#15).
//
// After Mesen2 (Core/SNES/Coprocessors/SDD1/), GPL-3.0, Sour and the Mesen
// contributors; the decompressor is Andreas Naive's (public domain). The
// MMC maps four 1 MB ROM blocks at $C0-$FF ($4804-$4807) and can mirror
// $20-$3F/$A0-$BF onto $00-$1F/$80-$9F. When a DMA channel enabled in
// $4800 and $4801 reads its source address in $C0-$FF, the S-DD1 returns
// decompressed graphics instead of ROM. It watches the DMA registers
// ($43x2-$43x6) for the address and length.

#include "snes_sdd1_decomp.h"

#include <cstdint>
#include <vector>

namespace snes {

class Sdd1 {
public:
    Sdd1(const std::vector<uint8_t>* rom, std::vector<uint8_t>* sram);

    void reset();
    /** The S-CPU's view: -1 where the cartridge doesn't answer. */
    int cpuRead(uint8_t bank, uint16_t a, bool peek);
    /** True if the cartridge took the write. */
    bool cpuWrite(uint8_t bank, uint16_t a, uint8_t v);
    /** A write to the DMA registers ($4300-$437F), watched for addresses and lengths. */
    void dmaRegWrite(uint16_t a, uint8_t v);

private:
    uint32_t romOffset(uint32_t addr) const;
    uint8_t romRead(uint32_t addr) const;

    const std::vector<uint8_t>* m_rom;
    std::vector<uint8_t>* m_sram;
    Sdd1Decomp m_decomp;
    Sdd1Reader m_reader;
    uint8_t m_allowDma = 0, m_nextDma = 0, m_banks[4] = {0, 1, 2, 3};
    uint32_t m_dmaAddr[8] = {};
    uint16_t m_dmaLen[8] = {};
    bool m_needInit = true;
};

}  // namespace snes
