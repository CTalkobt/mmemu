#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/ivideo_inspect.h"
#include "libdevices/main/isignal_line.h"
#include <cstdint>
#include <string>
#include <vector>

class GTIA;

/**
 * ANTIC (Alphanumeric Television Interface Controller), Atari 8-bit.
 *
 * Clocked once per CPU cycle (NTSC: 114 cycles a line, 262 lines). Each
 * line of the display (scan lines 8-247) runs the display list: blank
 * lines, jumps (JMP, JVB), character modes 2-7 and map modes 8-F, with LMS,
 * DLI, horizontal and vertical fine scrolling, narrow/normal/wide playfields,
 * CHACTL (inverse, blank, upside down) and CHBASE. Each line's playfield is
 * handed to GTIA, which adds the players and missiles and colours it; ANTIC
 * fetches the player/missile data (PMBASE, single or double line).
 *
 * Interrupts: NMI for display-list interrupts (on a mode line's last scan
 * line) and the vertical blank (line 248), NMIST/NMIRES. WSYNC stops the
 * CPU to the end of the line; ANTIC's DMA (display list, playfield,
 * character data, players/missiles, refresh) takes cycles from the CPU.
 * The machine's scheduler asks cpuBlocked() before each CPU step.
 */
class Antic : public IOHandler, public IVideoOutput, public IVideoInspect {
public:
    static constexpr int CYCLES_PER_LINE = 114;
    static constexpr int LINES_NTSC = 262;
    static constexpr int LINES_PAL = 312;
    static constexpr int FIRST_LINE = 8;      ///< first display scan line
    static constexpr int VBLANK_LINE = 248;   ///< vertical blank starts
    static constexpr int FRAME_W = 384;       ///< colour clocks 32-223, two pixels each
    static constexpr int FRAME_H = VBLANK_LINE - FIRST_LINE;   // 240
    static constexpr int RENDER_CYCLE = 104;  ///< line drawn, WSYNC released

    Antic() : m_name("ANTIC"), m_baseAddr(0xD400) { Antic::reset(); }
    ~Antic() override = default;

    // Configuration
    void setDmaBus(IBus* bus) { m_dmaBus = bus; }
    void setGtia(GTIA* gtia)  { m_gtia = gtia; }
    void setNmiLine(ISignalLine* line) override { m_nmiLine = line; }
    void setPal(bool pal) { m_lines = pal ? LINES_PAL : LINES_NTSC; }
    /** RESET key (400/800): NMI with NMIST bit 5. */
    void pressResetKey();

    // IOHandler
    const char* name()     const override { return m_name.c_str(); }
    void        setName(const std::string& name) override { m_name = name; }
    uint32_t    baseAddr() const override { return m_baseAddr; }
    uint32_t    addrMask() const override { return 0x000F; }
    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t  val) override;
    void reset()  override;
    void tick(uint64_t cycles) override;
    std::vector<std::string> deviceAliases() const override { return {"ANTIC", "ATARI_ANTIC"}; }

    /**
     * True while the CPU may not run this cycle: WSYNC until the end of the
     * line, or a DMA cycle ANTIC takes. The scheduler then ticks the devices
     * for one cycle without the CPU.
     */
    bool cpuBlocked() const { return m_wsync || m_stealPending > 0; }
    /** The scheduler ran a cycle without the CPU: it repays a DMA cycle. */
    void consumeBlockedCycle() { if (m_stealPending > 0) --m_stealPending; }
    /** Machine cycles elapsed (for real-time pacing; the CPU's own count misses DMA). */
    uint64_t totalCycles() const { return m_totalCycles; }

    // IVideoOutput
    VideoDimensions getDimensions() const override { return {FRAME_W, FRAME_H, FRAME_W, FRAME_H}; }
    void renderFrame(uint32_t* buffer) override;

    // Read access for tests and the debugger
    uint16_t vcount() const { return m_line; }
    uint16_t hcount() const { return m_cycle; }
    uint16_t displayListPc() const { return m_dlPc; }
    uint16_t memoryScan() const { return m_memScan; }
    uint64_t frameCount() const { return m_frames; }

    // IVideoInspect (the Graphics (ANTIC/GTIA) pane, CLI gfx, MCP render_graphics):
    // sprites 0-3 are players 0-3 and sprite 4 the missiles, from PMBASE at
    // the DMACTL resolution; the character set is CHBASE, in the text mode
    // the display list uses most; colours from GTIA's registers and palette.
    int      paletteSize() const override { return 256; }
    uint32_t paletteColor(int index) const override;
    uint8_t  peekVideo(uint32_t addr) const override;
    int      spriteCount() const override { return 5; }
    bool     spriteInfo(int n, SpriteInfo& out) const override;
    bool     charsetInfo(CharsetInfo& out) const override;
    /** The ANTIC text mode (2-7) on most display-list lines, 0 if none. */
    int      mainTextMode() const;

    enum Reg {
        DMACTL = 0x00, CHACTL = 0x01, DLISTL = 0x02, DLISTH = 0x03,
        HSCROL = 0x04, VSCROL = 0x05, PMBASE = 0x07, CHBASE = 0x09,
        WSYNC  = 0x0A, VCOUNT = 0x0B, PENH = 0x0C, PENV = 0x0D,
        NMIEN  = 0x0E, NMIST  = 0x0F   // write: NMIRES
    };

    // DMACTL bits
    static constexpr uint8_t DMA_PF_MASK = 0x03;  ///< 0 off, 1 narrow, 2 normal, 3 wide
    static constexpr uint8_t DMA_MISSILE = 0x04;
    static constexpr uint8_t DMA_PLAYER  = 0x08;  ///< also fetches the missiles
    static constexpr uint8_t DMA_PM_1LINE = 0x10; ///< single-line resolution
    static constexpr uint8_t DMA_DL      = 0x20;

    // NMIEN / NMIST bits
    static constexpr uint8_t NMI_DLI   = 0x80;
    static constexpr uint8_t NMI_VBI   = 0x40;
    static constexpr uint8_t NMI_RESET = 0x20;

private:
    void startLine();
    void fetchInstruction();
    void buildPlayfieldLine(uint8_t* pf);
    void drawLine();
    void raiseNmi(uint8_t bit);
    uint8_t fetch(uint16_t addr);

    std::string m_name;
    uint32_t    m_baseAddr;
    IBus*        m_dmaBus = nullptr;
    GTIA*        m_gtia = nullptr;
    ISignalLine* m_nmiLine = nullptr;

    uint8_t  m_regs[16] = {};
    int      m_lines = LINES_NTSC;
    uint16_t m_line = 0;          // scan line (VCOUNT * 2)
    uint16_t m_cycle = 0;         // cycle in the line
    uint64_t m_frames = 0;
    uint64_t m_totalCycles = 0;

    // NMI
    uint8_t  m_nmist = 0;
    int      m_nmiHold = 0;       // cycles the NMI line stays asserted

    // CPU blocking
    bool     m_wsync = false;
    int      m_stealPending = 0;  // DMA cycles still to take this line

    // Display list state
    uint16_t m_dlPc = 0;          // display list counter
    uint16_t m_memScan = 0;       // memory scan counter (12-bit wrap)
    uint8_t  m_instr = 0;         // current instruction
    int      m_mode = 0;          // its mode (0 blank, 2-15)
    int      m_row = 0;           // scan line within the mode line
    int      m_lastRow = 0;       // its last scan line
    bool     m_waitVbl = false;   // after JVB: idle until the vertical blank
    bool     m_prevVscrol = false;
    bool     m_dliThisLine = false;
    uint8_t  m_lineData[64] = {}; // bytes fetched for the mode line
    int      m_lineBytes = 0;
    int      m_hscrolShift = 0;   // pixels (half colour clocks) of fine scroll
    int      m_hsFetchWidth = 2;  // playfield width the line was fetched at (wider with HSCROL)

    std::vector<uint32_t> m_frame;  // FRAME_W x FRAME_H, 0xAARRGGBB
};
