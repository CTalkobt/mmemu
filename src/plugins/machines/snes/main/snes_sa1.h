#pragma once

// The SA-1 in Super Mario RPG, Kirby Super Star, Kirby's Dream Land 3 (#15).
//
// After Mesen2 (Core/SNES/Coprocessors/SA1/), GPL-3.0, Sour and the Mesen
// contributors. A second 65C816 at 10.74 MHz (half the master clock; a
// BW-RAM access takes two of its cycles) with 2 KB of internal RAM (I-RAM),
// sharing the cartridge's ROM and battery RAM (BW-RAM) with the S-CPU:
//   - the MMC: four 1 MB ROM blocks for $00-$1F/$20-$3F/$80-$9F/$A0-$BF
//     (LoROM) and $C0-$FF (64 KB banks), BW-RAM windows at $6000-$7FFF;
//   - messages and interrupts both ways, the S-CPU's NMI/IRQ vectors
//     replaceable, the SA-1's own reset/NMI/IRQ vectors;
//   - DMA between ROM, BW-RAM and I-RAM, and the character conversion DMA
//     (types 1 and 2) to SNES tile format;
//   - the arithmetic unit (multiply, divide, multiply-accumulate) and the
//     variable-length bit reader;
//   - BW-RAM bitmap views (2 or 4 bits a pixel) and write protection.
// Left out, as in Mesen: the H/V timers, and the cycle penalties when both
// CPUs want the same memory.

#include "plugins/65816/main/cpu65816.h"
#include "libmem/main/ibus.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace snes {

class Sa1 {
public:
    /** `rom` and `bwram` (the cartridge's battery RAM) belong to the cartridge. */
    Sa1(const std::vector<uint8_t>* rom, std::vector<uint8_t>* bwram);
    ~Sa1();

    void reset();
    /** Run the SA-1 up to master clock `master`. */
    void runTo(uint64_t master);

    /** The S-CPU's view: -1 where the cartridge doesn't answer. */
    int cpuRead(uint8_t bank, uint16_t a, bool peek);
    /** True if the SA-1 cartridge took the write. */
    bool cpuWrite(uint8_t bank, uint16_t a, uint8_t v);
    /** The IRQ to the S-CPU. */
    bool cpuIrq() const;

    W65C816& cpu() { return *m_cpu; }
    const uint8_t* iram() const { return m_iram; }
    uint64_t cycles() const { return m_cycles; }

private:
    class Bus;
    friend class Bus;

    // The SA-1's own memory
    uint8_t sa1Read(uint32_t addr);
    void sa1Write(uint32_t addr, uint8_t v);
    int32_t romOffset(uint8_t bank, uint16_t a) const;
    uint8_t romByte(uint32_t off) const;

    // Registers
    void cpuRegWrite(uint16_t a, uint8_t v);
    uint8_t cpuRegRead(uint16_t a);
    void sa1RegWrite(uint16_t a, uint8_t v);
    uint8_t sa1RegRead(uint16_t a);
    void sharedRegWrite(uint16_t a, uint8_t v);
    void processInterrupts();

    // BW-RAM
    uint8_t& bw(uint32_t off) { return (*m_bwram)[off % m_bwram->size()]; }
    bool bwWritable(uint32_t off) const;
    void bwWrite(uint32_t off, uint8_t v) { if (!m_bwram->empty() && bwWritable(off)) bw(off) = v; }
    uint8_t bitmapRead(uint32_t addr);
    void bitmapWrite(uint32_t addr, uint8_t v);
    bool iramWritable(uint16_t a, uint8_t protect) const { return (protect >> ((a >> 8) & 7)) & 1; }

    void runDma();
    void processMath();
    void incVarLen();
    uint8_t charConvType1(uint32_t addr);
    void charConvType2();

    const std::vector<uint8_t>* m_rom;
    std::vector<uint8_t>* m_bwram;
    uint8_t m_iram[0x800] = {};
    std::unique_ptr<Bus> m_bus;
    std::unique_ptr<W65C816> m_cpu;
    uint64_t m_cycles = 0;
    uint64_t m_extra = 0;          // BW-RAM wait cycles during an instruction
    uint64_t m_c0 = 0;             // the CPU's cycle count when the instruction began
    /** The SA-1's time, part way through an instruction too. */
    uint64_t now() const { return m_cycles + (m_cpu->cycles() - m_c0) + m_extra; }
    uint8_t m_openBus = 0;

    // The state (Mesen's Sa1State)
    uint16_t m_sa1ResetVec = 0, m_sa1IrqVec = 0, m_sa1NmiVec = 0;
    bool m_sa1IrqReq = false, m_sa1IrqEn = false, m_sa1NmiReq = false, m_sa1NmiEn = false;
    bool m_sa1Wait = false, m_sa1Reset = false, m_dmaIrqEn = false, m_timerIrqEn = false;
    uint8_t m_sa1Msg = 0, m_cpuMsg = 0;
    uint16_t m_cpuIrqVec = 0, m_cpuNmiVec = 0;
    bool m_useCpuIrqVec = false, m_useCpuNmiVec = false, m_cpuIrqReq = false, m_cpuIrqEn = false;
    bool m_ccIrqFlag = false, m_ccIrqEn = false, m_ccDmaActive = false;
    uint8_t m_ccBpp = 8, m_ccFormat = 0, m_ccWidth = 0, m_ccCounter = 0;
    uint8_t m_cpuBwBank = 0, m_sa1BwBank = 0;
    bool m_cpuBwWe = false, m_sa1BwMode = false, m_sa1BwWe = false, m_bw2bpp = false;
    uint8_t m_bwProtect = 0;
    uint8_t m_brf1[8] = {}, m_brf2[8] = {};
    uint8_t m_cpuIramWp = 0, m_sa1IramWp = 0;
    uint32_t m_dmaSrc = 0, m_dmaDest = 0;
    uint16_t m_dmaSize = 0;
    bool m_dmaEnabled = false, m_dmaPriority = false, m_dmaCc = false, m_dmaCcAuto = false;
    uint8_t m_dmaDestDev = 0, m_dmaSrcDev = 0;
    bool m_dmaRunning = false, m_dmaIrqFlag = false;
    uint8_t m_mathOp = 0;
    uint16_t m_ma = 0, m_mb = 0;
    uint64_t m_mathStart = 0, m_mathResult = 0;
    bool m_mathPending = false;
    uint8_t m_mathOverflow = 0;
    bool m_vlAutoInc = false;
    uint8_t m_vlBits = 16, m_vlBit = 0;
    uint32_t m_vlAddr = 0;
    uint8_t m_banks[4] = {0, 1, 2, 3};
};

}  // namespace snes
