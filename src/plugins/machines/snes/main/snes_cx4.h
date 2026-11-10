#pragma once

// The Capcom Cx4 (Hitachi HG51B169) in Mega Man X2 and X3 (#15).
//
// After Mesen2 (Core/SNES/Coprocessors/CX4/), GPL-3.0, Sour and the Mesen
// contributors. A 24-bit DSP at 20 MHz running 16-bit instructions from a
// two-page, 256-instruction program cache filled from the cartridge ROM,
// with 3 KB of data RAM, sixteen 24-bit registers, a 48-bit multiplier, a
// built-in data ROM of maths tables, an 8-level call stack, DMA between ROM,
// battery RAM and its data RAM, and buffered bus accesses. The SNES sees its
// data RAM and registers at $6000-$7FFF; ROM and battery RAM keep the
// LoROM layout.

#include <cstdint>
#include <vector>

namespace snes {

class Cx4 {
public:
    static const uint32_t DATA_ROM[1024];

    /** `rom` and `sram` belong to the cartridge. */
    Cx4(const std::vector<uint8_t>* rom, std::vector<uint8_t>* sram);

    void reset();
    /** Run up to master clock `master`. */
    void runTo(uint64_t master);

    /** The SNES side: $6000-$7FFF in banks $00-$3F/$80-$BF. */
    uint8_t read(uint16_t addr) const;
    void write(uint16_t addr, uint8_t v);
    bool irq() const { return m_irqLine; }

    uint32_t reg(int i) const { return m_regs[i & 15]; }
    uint8_t dataRam(uint16_t a) const { return m_dataRam[a % 0xC00]; }

private:
    enum Kind { NONE, ROM, SRAM, REGS };
    Kind kind(uint32_t addr) const;
    uint8_t busRead(uint32_t addr);
    void busWrite(uint32_t addr, uint8_t v);
    uint8_t accessDelay(uint32_t addr) const;

    void step(uint64_t cycles);
    void exec(uint16_t op);
    void switchCachePage();
    bool processCache(uint64_t target);
    void processDma(uint64_t target);
    bool busy() const { return m_cacheEnabled || m_dmaEnabled || m_busDelay > 0; }
    bool running() const { return busy() || !m_stopped; }
    void stop();

    uint32_t source(uint8_t src);
    void writeRegister(uint8_t reg, uint32_t v);
    void setA(uint32_t v) { m_a = v & 0xFFFFFF; }
    void setZN() { m_zero = m_a == 0; m_negative = m_a & 0x800000; }
    uint32_t add(uint32_t a, uint32_t b);
    uint32_t sub(uint32_t a, uint32_t b);
    void branch(bool taken, uint8_t far, uint8_t dest);
    void jsr(bool taken, uint8_t far, uint8_t dest);
    void skip(uint8_t flag, uint8_t ifSet);
    uint16_t ramAddr(uint32_t a) const { uint16_t r = (uint16_t)(a & 0xFFF); return r >= 0xC00 ? (uint16_t)(r - 0x400) : r; }

    const std::vector<uint8_t>* m_rom;
    std::vector<uint8_t>* m_sram;
    uint8_t m_dataRam[0xC00] = {};
    uint16_t m_prgRam[2][256] = {};

    uint64_t m_cycles = 0;
    uint16_t m_pb = 0, m_p = 0;
    uint8_t m_pc = 0, m_sp = 0;
    uint32_t m_a = 0, m_stack[8] = {};
    uint64_t m_mult = 0;
    uint32_t m_romBuffer = 0;
    uint8_t m_ramBuffer[3] = {};
    uint32_t m_mdr = 0, m_mar = 0, m_dpr = 0, m_regs[16] = {};
    bool m_negative = false, m_zero = false, m_carry = false, m_overflow = false, m_irqFlag = false, m_irqLine = false;
    bool m_stopped = true, m_locked = false, m_irqDisabled = false, m_singleRom = true;
    uint8_t m_romDelay = 3, m_ramDelay = 3;
    // The buffered bus access
    bool m_busEnabled = false, m_busReading = false, m_busWriting = false;
    uint8_t m_busDelay = 0;
    uint32_t m_busAddr = 0;
    // DMA
    uint32_t m_dmaSrc = 0, m_dmaDest = 0, m_dmaPos = 0;
    uint16_t m_dmaLen = 0;
    bool m_dmaEnabled = false;
    // The program cache
    bool m_cacheEnabled = false, m_cacheLock[2] = {};
    uint8_t m_cachePage = 0, m_cachePc = 0;
    uint32_t m_cacheAddr[2] = {}, m_cacheBase = 0;
    uint16_t m_cacheBank = 0, m_cachePos = 0;
    // Suspend
    bool m_suspend = false;
    uint32_t m_suspendLeft = 0;
    uint8_t m_vectors[0x20] = {};
};

}  // namespace snes
