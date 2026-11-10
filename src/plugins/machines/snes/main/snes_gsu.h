#pragma once

// The Super FX (GSU-1/GSU-2, the MARIO chip) in Star Fox, Yoshi's Island,
// Stunt Race FX, Doom... (#15).
//
// After Mesen2 (Core/SNES/Coprocessors/GSU/), GPL-3.0, Sour and the Mesen
// contributors. A 16-bit RISC CPU at the master clock (21.477 MHz, or half
// with CLSR clear) with sixteen registers, a 512-byte instruction cache,
// buffered ROM reads (R14) and RAM writes, an 8x8/16x16 multiplier, and
// the PLOT unit that draws pixels into SNES tile format in its RAM through
// two 8-pixel caches.
//
// The cartridge's map (as Mesen):
//   SNES:  $00-$3F/$80-$BF:$3000-$3FFF   the GSU's registers and cache
//          $00-$3F/$80-$BF:$6000-$7FFF   the first 8 KB of its RAM
//          $70-$71/$F0-$F1               its RAM
//          $00-$3F/$80-$BF:$8000-$FFFF   ROM, LoROM; $40-$5F/$C0-$DF ROM, 64 KB a bank
//   GSU:   $00-$3F (both halves) and $40-$5F ROM, $70-$71 RAM
// While the GSU runs with the ROM (or RAM) given to it (SCMR), the SNES
// reads fixed vector values from ROM (and 0 from RAM).

#include <cstdint>
#include <vector>

namespace snes {

class Gsu {
public:
    struct Flags {
        bool z = false, cy = false, s = false, ov = false, g = false, r = false;
        bool alt1 = false, alt2 = false, il = false, ih = false, b = false, irq = false;
        uint8_t low() const { return (uint8_t)(z << 1 | cy << 2 | s << 3 | ov << 4 | g << 5 | r << 6); }
        uint8_t high() const { return (uint8_t)(alt1 | alt2 << 1 | il << 2 | ih << 3 | b << 4 | irq << 7); }
    };
    struct PixelCache { uint8_t x = 0, y = 0, pixels[8] = {}, valid = 0; };

    /** `rom`: the cartridge's ROM (not copied); `ramSize`: the GSU's RAM. */
    Gsu(const std::vector<uint8_t>* rom, uint32_t ramSize);

    void reset();
    /** Run up to master clock `master`. */
    void runTo(uint64_t master);

    /** The SNES side: $3000-$3FFF. */
    uint8_t readReg(uint16_t addr);
    uint8_t peekReg(uint16_t addr) const;
    void writeReg(uint16_t addr, uint8_t v);
    /** ROM for the SNES (offset into the ROM image): vector values while the GSU owns it. */
    uint8_t cpuReadRom(uint32_t offset, uint16_t addr) const;
    /** RAM for the SNES (offset into the GSU's RAM): 0 / ignored while the GSU owns it. */
    uint8_t cpuReadRam(uint32_t offset) const;
    void cpuWriteRam(uint32_t offset, uint8_t v);

    /** The IRQ to the SNES (STOP with IRQs enabled, until $3031 is read). */
    bool irq() const { return m_sfr.irq; }
    bool running() const { return m_sfr.g; }
    uint16_t reg(int i) const { return m_r[i & 15]; }
    std::vector<uint8_t>& ram() { return m_ram; }

private:
    void exec();
    uint8_t readGsu(uint32_t addr);
    void writeGsu(uint32_t addr, uint8_t v);
    void initCache(uint16_t cacheAddr);
    uint8_t readOperand();
    uint8_t readOpcode();
    uint8_t readProgramByte();
    uint16_t src() const { return m_r[m_srcReg]; }
    void writeDest(uint16_t v) { writeReg16(m_destReg, v); }
    void writeReg16(uint8_t r, uint16_t v);
    void resetFlags();
    void invalidateCache();
    void waitRom();
    void waitRam();
    void waitForRomAccess();
    void waitForRamAccess();
    void updateRunning();
    uint8_t readRomBuffer();
    uint8_t readRamBuffer(uint16_t addr);
    void writeRam(uint16_t addr, uint8_t v);
    void step(uint64_t cycles);
    uint8_t clk(uint8_t fast, uint8_t slow) const { return m_clockSelect ? fast : slow; }

    // Instructions
    void branch(bool taken);
    void stop();
    void cache();
    void jmp(uint8_t r);
    void to(uint8_t r);
    void from(uint8_t r);
    void with(uint8_t r);
    void store(uint8_t r);
    void load(uint8_t r);
    void loop();
    void merge();
    void swap();
    void plotRpix();
    void colorCmode();
    void add(uint8_t r);
    void subCmp(uint8_t r);
    void mult(uint8_t r);
    void fmultLmult();
    void andBic(uint8_t r);
    void sbk();
    void link(uint8_t n);
    void sex();
    void notOp();
    void lsr();
    void rol();
    void asr();
    void ror();
    void lob();
    void hib();
    void ibtSmsLms(uint8_t r);
    void iwtLmSm(uint8_t r);
    void orXor(uint8_t r);
    void inc(uint8_t r);
    void dec(uint8_t r);
    void getcRambRomb();
    void getb();
    void setZS(uint16_t v) { m_sfr.z = v == 0; m_sfr.s = (v & 0x8000) != 0; }

    // PLOT
    uint16_t tileIndex(uint8_t x, uint8_t y) const;
    uint32_t tileAddress(uint8_t x, uint8_t y) const;
    uint8_t readPixel(uint8_t x, uint8_t y);
    bool transparent() const;
    void drawPixel(uint8_t x, uint8_t y);
    void flushPrimary(uint8_t x, uint8_t y);
    void writePixelCache(PixelCache& c);
    uint8_t color(uint8_t v) const;

    const std::vector<uint8_t>* m_rom;
    std::vector<uint8_t> m_ram;

    uint64_t m_cycles = 0;
    uint16_t m_r[16] = {};
    Flags m_sfr;
    uint8_t m_latch = 0;
    uint8_t m_pbr = 0, m_romBank = 0, m_ramBank = 0;
    bool m_irqDisabled = false, m_highSpeed = false, m_clockSelect = false, m_backupRam = false;
    uint8_t m_screenBase = 0, m_plotBpp = 2, m_screenHeight = 0;
    bool m_ramAccess = false, m_romAccess = false;
    uint16_t m_cacheBase = 0;
    bool m_plotTransparent = false, m_plotDither = false, m_colorHigh = false, m_colorFreeze = false, m_objMode = false;
    uint8_t m_colorReg = 0, m_srcReg = 0, m_destReg = 0;
    uint8_t m_romBuffer = 0, m_romDelay = 0;
    uint8_t m_programBuffer = 0x01;
    uint16_t m_ramWriteAddr = 0;
    uint8_t m_ramWriteValue = 0, m_ramDelay = 0;
    uint16_t m_ramAddr = 0;
    PixelCache m_primary, m_secondary;

    uint8_t m_cache[512] = {};
    bool m_cacheValid[32] = {};
    bool m_waitRomAccess = false, m_waitRamAccess = false, m_stopped = true, m_r15Changed = false;
};

}  // namespace snes
