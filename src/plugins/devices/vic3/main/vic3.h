#pragma once

#include "plugins/devices/vic2/main/vic2.h"
#include <cstdint>

/**
 * CSG 4567 VIC-III Video Interface Chip (Commodore 65).
 *
 * Extends the VIC-II with:
 * - 256-color palette ($D100-$D3FF: R, G, B channels)
 * - VIC-III control registers ($D030-$D031)
 * - 80-column text mode (H640)
 * - Extended attributes with 8-bit colour (ATTR)
 * - Bitplane graphics mode (BPM) with up to 8 planes
 * - C65 ROM banking via $D030
 * - Interlaced 400-line mode (V400)
 * - Personality lock (C64 vs C65 mode via KEY register)
 *
 * Register map beyond VIC-II:
 *   $D030: Banking/control (CRAM2K, EXTSYNC, PAL, ROM8/A/C/E, CROM9)
 *   $D031: Mode control (H640, FAST, ATTR, BPM, V400, H1280, MONO, INT)
 *   $D032: Bitplane enable (8 bits, one per plane)
 *   $D033-$D03A: Bitplane addresses (even/odd lines)
 *   $D03B: Bitplane complement flags
 *   $D03C-$D03D: Bitplane X/Y offsets
 *   $D03E-$D03F: H/V position verniers
 *   $D040-$D047: DAT (Display Address Translator) bitplane ports
 *   $D100-$D1FF: Palette RED (256 entries)
 *   $D200-$D2FF: Palette GREEN
 *   $D300-$D3FF: Palette BLUE
 */
class VIC3 : public VIC2 {
public:
    VIC3();
    VIC3(const std::string& name, uint32_t baseAddr);
    ~VIC3() override = default;

    // IOHandler overrides
    const char* name() const override { return m_vic3Name.c_str(); }
    uint32_t addrMask() const override { return 0x03FF; } // 1 KB window ($D000-$D3FF)

    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t  val) override;
    void reset() override;

    // IVideoOutput override — adds 80-col, bitplane, extended attr rendering
    void renderFrame(uint32_t* buffer) override;

    // Personality management (C64 locked vs C65/MEGA65 unlocked)
    bool isLocked() const { return m_locked; }
    void setLocked(bool locked) { m_locked = locked; }

    // --- $D031 bit constants ---
    static constexpr uint8_t D031_INT   = 0x01;
    static constexpr uint8_t D031_MONO  = 0x02;
    static constexpr uint8_t D031_H1280 = 0x04;
    static constexpr uint8_t D031_V400  = 0x08;
    static constexpr uint8_t D031_BPM   = 0x10;
    static constexpr uint8_t D031_ATTR  = 0x20;
    static constexpr uint8_t D031_FAST  = 0x40;
    static constexpr uint8_t D031_H640  = 0x80;

    // --- $D030 bit constants ---
    static constexpr uint8_t D030_CRAM2K  = 0x01;
    static constexpr uint8_t D030_EXTSYNC = 0x02;
    static constexpr uint8_t D030_PAL     = 0x04;
    static constexpr uint8_t D030_ROM8    = 0x08;
    static constexpr uint8_t D030_ROMA    = 0x10;
    static constexpr uint8_t D030_ROMC    = 0x20;
    static constexpr uint8_t D030_CROM9   = 0x40;
    static constexpr uint8_t D030_ROME    = 0x80;

    // Register offsets (relative to base) for VIC-III range
    static constexpr uint8_t REG_D030 = 0x30;
    static constexpr uint8_t REG_D031 = 0x31;
    static constexpr uint8_t REG_BPEN = 0x32; // Bitplane enable
    // $D033-$D03A: bitplane addresses
    // $D03B: bitplane complement
    // $D03C: BPX, $D03D: BPY
    // $D03E: HPOS, $D03F: VPOS
    // $D040-$D047: DAT ports

    // Palette access
    uint32_t getPaletteRGBA(uint8_t index, uint8_t bank = 0) const;

    // $D030/$D031 accessors
    uint8_t d030() const { return m_regs[REG_D030]; }
    uint8_t d031() const { return m_regs[REG_D031]; }

    /** Returns the 0-3 bank index used for $D100-$D3FF palette editing. */
    virtual uint8_t getEditPalBank() const { return 0; }

protected:
    void initPalette();
    virtual void renderBackground80col(uint32_t* buf);
    void renderBitplanes(uint32_t* buf);

    std::string m_vic3Name;
    bool m_locked = true;

    // Palette: 1024 entries (4 banks of 256), separate R/G/B channels
    uint8_t m_paletteR[1024];
    uint8_t m_paletteG[1024];
    uint8_t m_paletteB[1024];
};
