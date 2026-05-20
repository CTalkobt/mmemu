#pragma once

#include "plugins/devices/vic3/main/vic3.h"
#include <vector>

/**
 * VIC-IV Video Interface Chip for MEGA65.
 *
 * Extends the VIC-III (CSG 4567) with:
 * - VIC-IV specific registers ($D048–$D07F)
 * - 32 KB internal color RAM
 * - 28-bit physical addressing for screen/char/color data
 * - Full Color Mode (FCM) and Nibble Color Mode (NCM) [future]
 * - Super-Extended Attribute Mode [future]
 * - Alpha blending [future]
 *
 * Inheritance: VIC2 → VIC3 → VIC4
 */
class VIC4 : public VIC3 {
public:
    VIC4();
    ~VIC4() override = default;

    // IOHandler overrides
    const char* name() const override { return "VIC-IV"; }

    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t  val) override;
    void reset() override;
    void tick(uint64_t cycles) override;

    // IVideoOutput overrides
    void renderFrame(uint32_t* buffer) override;

    // --- $D054 bit constants ---
    static constexpr uint8_t D054_CHR16  = 0x01; // 16-bit character numbers
    static constexpr uint8_t D054_FCLRLO = 0x02; // FCM for char ≤ $FF
    static constexpr uint8_t D054_FCLRHI = 0x04; // FCM for char > $FF
    static constexpr uint8_t D054_SMTH   = 0x08; // Horizontal smoothing
    static constexpr uint8_t D054_SPRH640= 0x10; // Sprite H640 mode
    static constexpr uint8_t D054_VFAST  = 0x20; // 40 MHz mode
    static constexpr uint8_t D054_PALEMU = 0x40; // PAL CRT emulation
    static constexpr uint8_t D054_ALPHEN = 0x80; // Alpha compositor enable

    // Extended register accessors
    uint32_t getScreenBase() const;
    uint32_t getCharBase() const;
    uint16_t getColBase() const;
    uint8_t  d054() const { return m_extRegs[0x14]; } // $D054 - $D040 = 0x14

    // Display geometry accessors
    int getChrCount() const;    // $D05E: characters per row (0 = use default)
    int getDispRows() const;    // $D07B: text rows to display (0 = use default)
    uint16_t getLineStep() const; // $D058-$D059: bytes between rows (0 = auto)

private:
    void renderFCM(uint32_t* buf);

    // Extended registers $D048-$D07F (indexed as $D0xx - $D040)
    uint8_t m_extRegs[64];

    // Internal color RAM (32 KB)
    std::vector<uint8_t> m_colorRamExt;
};
