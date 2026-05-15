#pragma once

#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <string>
#include <vector>

/**
 * GTIA (Graphics Television Interface Adaptor) for Atari 8-bit.
 * 
 * Handles color generation, player-missile graphics (PMG) positioning,
 * collision detection, and console switches.
 */
class GTIA : public IOHandler {
public:
    GTIA() : m_name("GTIA"), m_baseAddr(0xD000) { GTIA::reset(); }
    ~GTIA() override = default;

    // -----------------------------------------------------------------------
    // IOHandler interface
    // -----------------------------------------------------------------------

    const char* name()     const override { return m_name.c_str(); }
    void        setName(const std::string& name) override { m_name = name; }
    uint32_t    baseAddr() const override { return m_baseAddr; }
    uint32_t    addrMask() const override { return 0x001F; } // 32 registers

    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t  val) override;
    void reset()  override;
    void tick(uint64_t cycles) override;

    // -----------------------------------------------------------------------
    // Console Switches
    // -----------------------------------------------------------------------
    void setConsoleSwitches(uint8_t bits) { m_consoleSwitches = bits & 0x07; }

    // -----------------------------------------------------------------------
    // Color Palette
    // -----------------------------------------------------------------------
    /**
     * Get RGBA8888 for an Atari color index (0-255).
     */
    static uint32_t getRGBA(uint8_t index);

    // -----------------------------------------------------------------------
    // Registers ($D000–$D01F)
    // -----------------------------------------------------------------------
    enum Reg {
        // Write-only registers (many share address with Read-only collision regs)
        HPOSP0  = 0x00, HPOSP1 = 0x01, HPOSP2 = 0x02, HPOSP3 = 0x03,
        HPOSM0  = 0x04, HPOSM1 = 0x05, HPOSM2 = 0x06, HPOSM3 = 0x07,
        SIZEP0  = 0x08, SIZEP1 = 0x09, SIZEP2 = 0x0A, SIZEP3 = 0x0B,
        SIZEM   = 0x0C,
        GRAFP0  = 0x0D, GRAFP1 = 0x0E, GRAFP2 = 0x0F, GRAFP3 = 0x10,
        GRAFM   = 0x11,
        COLPM0  = 0x12, COLPM1 = 0x13, COLPM2 = 0x14, COLPM3 = 0x15,
        COLPF0  = 0x16, COLPF1 = 0x17, COLPF2 = 0x18, COLPF3 = 0x19,
        COLBK   = 0x1A,
        PRIOR   = 0x1B,
        VDELAY  = 0x1C,
        GRACTL  = 0x1D,
        HITCLR  = 0x1E,
        CONSOL  = 0x1F,

        // Read-only registers (Collision)
        M0PF    = 0x00, M1PF = 0x01, M2PF = 0x02, M3PF = 0x03,
        P0PF    = 0x04, P1PF = 0x05, P2PF = 0x06, P3PF = 0x07,
        M0PL    = 0x08, M1PL = 0x09, M2PL = 0x0A, M3PL = 0x0B,
        P0PL    = 0x0C, P1PL = 0x0D, P2PL = 0x0E, P3PL = 0x0F,
        TRIG0   = 0x10, TRIG1 = 0x11, TRIG2 = 0x12, TRIG3 = 0x13,
        PAL     = 0x14
    };

private:
    std::string m_name;
    uint32_t    m_baseAddr;

    uint8_t     m_regs[32];
    uint8_t     m_collisions[16];
    uint8_t     m_consoleSwitches = 0x07; // Start, Select, Option (active low)
    bool        m_isPal = false;

    // Static color table
    static const uint32_t s_palette[256];
};
