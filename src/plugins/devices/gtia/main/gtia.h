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

    std::vector<std::string> deviceAliases() const override { return {"GTIA", "ATARI_GTIA"}; }

    // -----------------------------------------------------------------------
    // Console Switches
    // -----------------------------------------------------------------------
    void setConsoleSwitches(uint8_t bits) { m_consoleSwitches = bits & 0x07; }

    /** Console keys (CONSOL bits, active low): START, SELECT, OPTION. */
    static constexpr uint8_t CONSOL_START  = 0x01;
    static constexpr uint8_t CONSOL_SELECT = 0x02;
    static constexpr uint8_t CONSOL_OPTION = 0x04;
    void setConsoleKey(uint8_t bit, bool pressed) {
        if (pressed) m_consoleSwitches &= (uint8_t)~bit; else m_consoleSwitches |= bit;
    }
    /** Joystick trigger n (0-3) pressed. */
    void setTrigger(int n, bool pressed) { if (n >= 0 && n < 4) m_trig[n] = pressed; }
    /** PAL chip: the PAL register reads 1 and colours come from the PAL palette. */
    void setPal(bool pal) { m_isPal = pal; }
    bool isPal() const { return m_isPal; }

    // -----------------------------------------------------------------------
    // Scanline composition (ANTIC hands GTIA each line's playfield)
    // -----------------------------------------------------------------------

    /** Visible line: colour clocks 32-223 (a wide playfield), two pixels each. */
    static constexpr int LINE_PIXELS = 384;
    static constexpr int FIRST_COLOUR_CLOCK = 32;

    /** Playfield class of a pixel, as ANTIC produces it. */
    enum : uint8_t { PF_BK = 0, PF_0 = 1, PF_1 = 2, PF_2 = 3, PF_3 = 4 };
    /** With PF_2: a lit high-resolution pixel (PF2 hue, PF1 luminance). */
    static constexpr uint8_t PF_HIRES = 0x10;
    /** A pixel of a high-resolution line (modes 2, 3, F): PF2 collides only where lit. */
    static constexpr uint8_t PF_HIRES_MODE = 0x20;
    /** Inside the playfield window (GTIA modes 9-11 colour the border apart). */
    static constexpr uint8_t PF_WINDOW = 0x40;

    /**
     * Compose one scanline: the playfield (LINE_PIXELS classes) with the
     * players and missiles, by PRIOR's priority, into 0xAARRGGBB pixels.
     * Updates the collision registers.
     */
    void renderLine(const uint8_t* pf, uint32_t* out);

    /**
     * Player/missile DMA from ANTIC: stored if GRACTL enables it. With a
     * VDELAY bit set, that object ignores DMA on even scan lines (it moves
     * down a line in double-line resolution).
     */
    void dmaPlayer(int n, uint8_t data, bool evenLine = false) {
        if (!(m_regs[GRACTL] & 0x02)) return;
        if (evenLine && (m_regs[VDELAY] & (0x10 << n))) return;
        m_regs[GRAFP0 + n] = data;
    }
    void dmaMissiles(uint8_t data, bool evenLine = false) {
        if (!(m_regs[GRACTL] & 0x01)) return;
        uint8_t keep = 0;   // missiles held by VDELAY keep their bits
        if (evenLine)
            for (int m = 0; m < 4; ++m) if (m_regs[VDELAY] & (1 << m)) keep |= (uint8_t)(3 << (m * 2));
        m_regs[GRAFM] = (uint8_t)((m_regs[GRAFM] & keep) | (data & ~keep));
    }

    /** A write register's current value (colours, positions, PRIOR...). */
    uint8_t reg(int r) const { return m_regs[r & 0x1F]; }

    /**
     * Get the 0xAARRGGBB colour for an Atari colour index (0-255): hue in
     * bits 7-4, luminance in bits 3-0 (NTSC palette, or PAL's).
     */
    static uint32_t getARGB(uint8_t index, bool pal = false);

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
    bool        m_trig[4] = {false, false, false, false};
    bool        m_trigLatched[4] = {false, false, false, false};   // GRACTL bit 2
    bool        m_isPal = false;

    // Colour tables
    static const uint32_t s_palette[256];
    static const uint32_t s_palettePal[256];
};
