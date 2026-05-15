#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/isignal_line.h"
#include <cstdint>
#include <string>

/**
 * ANTIC (Alpha-Numeric Television Interface Controller) for Atari 8-bit.
 * 
 * Handles display list interpretation, DMA (cycle stealing), and 
 * generates video timing/data for GTIA.
 */
class Antic : public IOHandler, public IVideoOutput {
public:
    static constexpr int FRAME_W = 384;
    static constexpr int FRAME_H = 262; // NTSC: 262 lines
    static constexpr int DISPLAY_W = 320;
    static constexpr int DISPLAY_H = 192; // Standard 192 scanlines display

    Antic() : m_name("ANTIC"), m_baseAddr(0xD400) { Antic::reset(); }
    ~Antic() override = default;

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------

    void setDmaBus(IBus* bus) { m_dmaBus = bus; }
    void setHaltLine(ISignalLine* line) { m_haltLine = line; }
    void setNmiLine(ISignalLine* line) override { m_nmiLine = line; }

    // -----------------------------------------------------------------------
    // IOHandler interface
    // -----------------------------------------------------------------------

    const char* name()     const override { return m_name.c_str(); }
    void        setName(const std::string& name) override { m_name = name; }
    uint32_t    baseAddr() const override { return m_baseAddr; }
    uint32_t    addrMask() const override { return 0x000F; } // 16 registers

    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t  val) override;
    void reset()  override;
    void tick(uint64_t cycles) override;

    // -----------------------------------------------------------------------
    // IVideoOutput interface
    // -----------------------------------------------------------------------

    VideoDimensions getDimensions() const override {
        return {FRAME_W, FRAME_H, DISPLAY_W, DISPLAY_H};
    }
    void renderFrame(uint32_t* buffer) override;

    // -----------------------------------------------------------------------
    // Registers ($D400–$D40F)
    // -----------------------------------------------------------------------
    enum Reg {
        DMACTL  = 0x00, // DMA Control
        CHACTL  = 0x01, // Character Control
        DLISTL  = 0x02, // Display List Pointer Low
        DLISTH  = 0x03, // Display List Pointer High
        HSCROL  = 0x04, // Horizontal Scroll
        VSCROL  = 0x05, // Vertical Scroll
        PMBASE  = 0x07, // Player-Missile Base Address
        CHBASE  = 0x09, // Character Base Address
        WSYNC   = 0x0A, // Wait for Horizontal Sync
        VCOUNT  = 0x0B, // Vertical Line Counter
        NMIEN   = 0x0E, // NMI Enable
        NMIST   = 0x0F  // NMI Status / Reset
    };

    // DMACTL bits
    static constexpr uint8_t DMA_DL      = 0x20; // Display List DMA
    static constexpr uint8_t DMA_PM_RESO = 0x10; // PM Resolution (1=single, 0=double)
    static constexpr uint8_t DMA_PM_MISS = 0x08; // Missile DMA
    static constexpr uint8_t DMA_PM_PLAY = 0x04; // Player DMA
    static constexpr uint8_t DMA_PF_MASK = 0x03; // Playfield DMA (0=none, 1=narrow, 2=standard, 3=wide)

    // NMIEN bits
    static constexpr uint8_t NMI_VBI = 0x40; // Vertical Blank Interrupt
    static constexpr uint8_t NMI_DLI = 0x80; // Display List Interrupt

private:
    void updateNmi();
    void executeDisplayList();
    uint8_t fetchByte(uint16_t addr);

    std::string m_name;
    uint32_t    m_baseAddr;

    uint8_t     m_regs[16];
    uint64_t    m_cycleAccum = 0;
    uint16_t    m_vcount = 0;
    uint16_t    m_hcount = 0; // 0-113 cycles per line (1.79MHz / 15.7kHz)

    // DMA state
    IBus*       m_dmaBus = nullptr;
    ISignalLine* m_haltLine = nullptr;
    ISignalLine* m_nmiLine = nullptr;

    uint16_t    m_dlistPtr = 0;
    uint16_t    m_memoryScanAddr = 0;
    bool        m_wsync = false;
    uint8_t     m_nmist = 0;
    uint64_t    m_lastCycles = 0;

    // Buffer for current scanline data (simplified)
    uint8_t     m_scanlineData[FRAME_W];
};
