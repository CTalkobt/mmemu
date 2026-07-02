#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/isignal_line.h"
#include <cstdint>
#include <string>

/**
 * MOS 6567/6569 VIC-II Video Interface Chip.
 *
 * Implements the video and interrupt logic for the Commodore 64.
 *   - 6567: NTSC, 65 cycles/line × 263 lines, 504 pixels/line total.
 *   - 6569: PAL,  63 cycles/line × 312 lines.
 *
 * Supported video modes (selected by $D011 ECM/BMM and $D016 MCM bits):
 *   Standard text, Multicolor text, Standard bitmap, Multicolor bitmap,
 *   Extended background color (ECB).
 *
 * Sprite engine: 8 hardware sprites, 24×21 pixels each, monochrome or
 *   multicolor, with independent X/Y expansion, priority control, and
 *   hardware sprite-sprite / sprite-background collision detection.
 *
 * VIC-II banking: all DMA accesses use a dedicated IBus* whose address space
 *   matches the 16 KB VIC bank selected by CIA #2 port A bits 0–1 (inverted).
 *   The machine factory sets m_dmaBus and calls setBankBase() when CIA2 changes.
 *   Within the bank, the character ROM is always visible at offsets $1000–$1FFF
 *   and $9000–$9FFF regardless of CPU banking (see setCharRom()).
 *
 * Raster IRQ: fires via ISignalLine when the raster counter matches $D012
 *   (plus bit 8 from $D011 bit 7).  The IRQ flag is in $D019 bit 0; the host
 *   clears it by writing 1 to that bit.
 *
 * renderFrame() renders a complete NTSC or PAL frame (FRAME_W × FRAME_H pixels,
 *   RGBA8888) at the point it is called — typically once per emulated frame from
 *   the GUI timer.  The raster is advanced independently in tick().
 *
 * Usage:
 *   vic2->setDmaBus(systemBus);
 *   vic2->setCharRom(charRomPtr, 4096);
 *   vic2->setIrqLine(irq);
 *   vic2->setBankBase(0x0000);   // default; updated by CIA2 port-A change
 *   ioRegistry->registerHandler(vic2);
 */
class VIC2 : public IOHandler, public IVideoOutput {
public:
    // Total rendered frame dimensions (includes border).
    static constexpr int FRAME_W    = 384;
    static constexpr int FRAME_H    = 272;
    // Visible display area start within the frame.
    static constexpr int DISPLAY_X  = 32;  // left border width in pixels
    static constexpr int DISPLAY_Y  = 36;  // top border height in lines
    static constexpr int DISPLAY_W  = 320; // 40 columns × 8 pixels
    static constexpr int DISPLAY_H  = 200; // 25 rows × 8 pixels

    // Timing defaults (PAL 6569).  Call setPal(false) for NTSC 6567.
    static constexpr int PAL_CYCLES_PER_LINE  = 63;
    static constexpr int PAL_LINES_PER_FRAME  = 312;
    static constexpr int NTSC_CYCLES_PER_LINE = 65;
    static constexpr int NTSC_LINES_PER_FRAME = 263;

    // ---------------------------------------------------------------------------

    VIC2() : m_name("VIC-II"), m_baseAddr(0xD000) { VIC2::reset(); }
    VIC2(const std::string& name, uint32_t baseAddr);
    ~VIC2() override = default;

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------

    void setName(const std::string& name) override { m_name = name; }
    void setBaseAddr(uint32_t addr) override       { m_baseAddr = addr; }

    /** Bus used for all DMA reads (screen RAM, bitmap, sprite data). */
    void setDmaBus(IBus* bus)      override { m_dmaBus = bus; }

    /** Pointer to the 4 KB character ROM image (always visible at VIC bank
     *  offsets $1000–$1FFF and $9000–$9FFF regardless of CPU banking). */
    void setCharRom(const uint8_t* data, uint32_t size) override { m_charRom = data; m_charRomSize = size; }

    /** Direct pointer to the 1 KB × 4-bit color RAM (1024 bytes, lower nibble
     *  used).  Mirrors the real VIC-II's dedicated 4-bit color RAM connection —
     *  bypasses the DMA bus so CPU banking / overlays don't affect rendering. */
    void setColorRam(const uint8_t* data) override { m_colorRam = data; }

    /** 16 KB VIC bank base address in system memory (0x0000/0x4000/0x8000/0xC000). */
    void setBankBase(uint32_t base) override { m_bankBase = base & 0xC000; }

    void setIrqLine(ISignalLine* line) override { m_irqLine = line; }

    /** Select PAL (true, default) or NTSC (false) timing. */
    void setPal(bool pal) {
        m_cyclesPerLine = pal ? PAL_CYCLES_PER_LINE : NTSC_CYCLES_PER_LINE;
        m_linesPerFrame = pal ? PAL_LINES_PER_FRAME : NTSC_LINES_PER_FRAME;
    }
    bool isPal() const { return m_linesPerFrame == PAL_LINES_PER_FRAME; }

    /** Set shared stall backlog counter for bus contention (#20).
     *  When a badline occurs, the VIC adds ~40 cycles to this counter.
     *  The machine scheduler stalls the CPU while backlog > 0. */
    void setStallBacklog(int* backlog) { m_stallBacklog = backlog; }

    /** Set extra badline cost (0-3, matching MEGA65 $D710 bits 4-5). */
    void setBadlineExtraCycles(int extra) { m_badlineExtra = extra & 3; }

    /** Enable/disable badline emulation (MEGA65 $D710 bit 0). */
    void setBadlineEnable(bool en) { m_badlineEnable = en; }

    void setLogger(void* handle, void (*logFn)(void*, int, const char*)) {
        m_logger = handle;
        m_logNamed = logFn;
    }

    void log(int level, const char* msg) const;

    // -----------------------------------------------------------------------
    // IOHandler interface  ($D000–$D02E, 47 registers, with $D02F–$D3FF mirrored)
    // -----------------------------------------------------------------------

    const char* name()     const override { return m_name.c_str(); }
    uint32_t    baseAddr() const override { return m_baseAddr; }
    uint32_t    addrMask() const override { return 0x003F; } // 64-byte window

    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t  val) override;
    void reset()  override;
    void tick(uint64_t cycles) override;
    void getDeviceInfo(DeviceInfo& out) const override;

    // -----------------------------------------------------------------------
    // IVideoOutput interface
    // -----------------------------------------------------------------------

    VideoDimensions getDimensions() const override {
        return {FRAME_W, FRAME_H, DISPLAY_W, DISPLAY_H};
    }
    void renderFrame(uint32_t* buffer) override;

    /** Manual DMA peek for testing and diagnostics. */
    virtual uint8_t dmaPeek(uint32_t offset) const;

    // -----------------------------------------------------------------------
    // Register indices (relative to base address)
    // -----------------------------------------------------------------------
    enum Reg {
        // Sprites 0–7 X/Y ($D000–$D00F)
        SP0X = 0x00, SP0Y = 0x01, SP1X = 0x02, SP1Y = 0x03,
        SP2X = 0x04, SP2Y = 0x05, SP3X = 0x06, SP3Y = 0x07,
        SP4X = 0x08, SP4Y = 0x09, SP5X = 0x0A, SP5Y = 0x0B,
        SP6X = 0x0C, SP6Y = 0x0D, SP7X = 0x0E, SP7Y = 0x0F,
        MSIGX = 0x10, // Sprite X MSBs
        SCR1  = 0x11, // Control 1: scroll-Y, 24/25 rows, display en, BMM, ECM, raster bit 8
        RASTER= 0x12, // Raster counter/compare
        LPX   = 0x13,
        LPY   = 0x14,
        SPENA = 0x15, // Sprite enable
        SCR2  = 0x16, // Control 2: scroll-X, 38/40 cols, MCM
        YXPAND= 0x17, // Sprite Y expansion
        VMEM  = 0x18, // Memory pointers: VM[14:11] bits 7-4, CB[13:11] bits 3-1
        IRQ   = 0x19, // IRQ status (write 1 to clear bits)
        IRQEN = 0x1A, // IRQ enable mask
        SPBGPR= 0x1B, // Sprite-BG priority
        SPMC  = 0x1C, // Sprite multicolor enable
        XXPAND= 0x1D, // Sprite X expansion
        SSCOL = 0x1E, // Sprite-sprite collision
        SBCOL = 0x1F, // Sprite-BG collision
        EXTCOL= 0x20, // Border color
        BGCOL0= 0x21, BGCOL1= 0x22, BGCOL2= 0x23, BGCOL3= 0x24,
        SPMC0 = 0x25, SPMC1 = 0x26,
        SP0COL= 0x27, SP1COL= 0x28, SP2COL= 0x29, SP3COL= 0x2A,
        SP4COL= 0x2B, SP5COL= 0x2C, SP6COL= 0x2D, SP7COL= 0x2E
    };

    // SCR1 bit masks
    static constexpr uint8_t SCR1_RASTER8 = 0x80;
    static constexpr uint8_t SCR1_ECM     = 0x40;
    static constexpr uint8_t SCR1_BMM     = 0x20;
    static constexpr uint8_t SCR1_DEN     = 0x10;
    static constexpr uint8_t SCR1_RSEL    = 0x08; // 1=25 rows, 0=24 rows
    static constexpr uint8_t SCR1_YSCROLL = 0x07;

    // SCR2 bit masks
    static constexpr uint8_t SCR2_CSEL    = 0x08; // 1=40 cols, 0=38 cols
    static constexpr uint8_t SCR2_MCM     = 0x10;

    // IRQ bits
    static constexpr uint8_t IRQ_RST  = 0x01; // raster
    static constexpr uint8_t IRQ_MBC  = 0x02; // sprite-BG collision
    static constexpr uint8_t IRQ_MMC  = 0x04; // sprite-sprite collision
    static constexpr uint8_t IRQ_LP   = 0x08; // light pen
    static constexpr uint8_t IRQ_ANY  = 0x80; // set when any enabled IRQ fires

protected:
    // Rendering helpers
    uint32_t palette(uint8_t index) const;
    uint8_t  charRomByte(uint32_t offset) const;

    virtual uint32_t screenBase() const;
    virtual uint32_t charBitmapBase() const;

    void renderBackground(uint32_t* buf);
    void renderSprites(uint32_t* buf);
    void updateIrq();

    std::string  m_name;
    uint32_t     m_baseAddr;
    void*        m_logger = nullptr;
    void         (*m_logNamed)(void*, int, const char*) = nullptr;

    uint8_t  m_regs[64];            // 47 used, rest pad to 64
    uint64_t m_cycleAccum = 0;      // cycles since last frame start
    uint16_t m_rasterLine = 0;      // current raster line
    int      m_cyclesPerLine = PAL_CYCLES_PER_LINE;
    int      m_linesPerFrame = PAL_LINES_PER_FRAME;

    // Bus contention (#20)
    int*     m_stallBacklog = nullptr;   // shared counter with scheduler
    int      m_badlineExtra = 0;         // extra cycles per badline (0-3)
    bool     m_badlineEnable = true;     // badline emulation enabled

    // DMA and ROM
    IBus*           m_dmaBus     = nullptr;
    uint32_t        m_bankBase   = 0x0000;
    const uint8_t*  m_charRom    = nullptr;
    uint32_t        m_charRomSize = 0;
    const uint8_t*  m_colorRam   = nullptr;  // direct 4-bit color RAM connection

    ISignalLine*    m_irqLine    = nullptr;

    // Collision state (cleared on read)
    uint8_t m_sprSprColl = 0;
    uint8_t m_sprBgColl  = 0;
};
