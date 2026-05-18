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

private:
    // VIC-IV extended register accessors (28-bit physical addresses)
    uint32_t getScreenBase() const;
    uint32_t getCharBase() const;

    // Extended registers $D048-$D07F (56 bytes, VIC-IV only)
    uint8_t m_extRegs[64]; // indexed as offset - 0x40

    // Internal color RAM (32 KB)
    std::vector<uint8_t> m_colorRamExt;
};
