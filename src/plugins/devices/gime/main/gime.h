#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace gime_font {
/** The GIME's character generator: 128 characters x 12 rows, bit 7 leftmost. */
extern const uint8_t GLYPHS[128][12];
}

/**
 * The TCC1014 "GIME" (Graphics, Interrupt and Memory Enhancer; Tandy's
 * ACVC) of the TRS-80 Color Computer 3: the MMU, the video generator, the
 * interrupt controller and timer, and the parts of the MC6883 SAM it
 * replaces. After Sockmaster's GIME register reference, the Color Computer 3
 * service manual and XRoar's tcc1014.c (measured timings; GPL, not copied).
 *
 * Time is kept in ticks of the 14.31818 MHz pixel clock (a PAL board's
 * 14.2373 MHz on a coco3p): a CPU cycle takes 16 (0.89 MHz) or 8 (1.79 MHz,
 * the SAM R1 bit at $FFD9) and a scan line 912.
 *
 * MMU ($FF90 MMUEN, $FF91 TR, $FFA0-$FFAF): the CPU's 64 KB in eight 8 KB
 * slots, each mapped by a page register of task 0 ($FFA0-$FFA7) or task 1
 * ($FFA8-$FFAF) to one of 64 8 KB pages of the 512 KB physical space. With
 * the MMU off the slots hold pages $38-$3F. The registers keep six bits; on
 * a 128 KB machine only physical address lines A0-A16 reach the RAM, so
 * page $00 and page $30 are the same RAM (the stock machine's 128 KB is
 * pages $30-$3F). $FE00-$FEFF with MC3 is always page $3F (the constant
 * page, for the secondary vectors). $FF00-$FFFF is never mapped.
 *
 * ROM (the SAM's TY bit at $FFDE/$FFDF clear): pages $3C-$3F are ROM, by
 * INIT0's MC1/MC0: 0x 16K internal ($3C-$3D) and 16K external ($3E-$3F,
 * the cartridge's /CTS), 10 32K internal, 11 32K external. The ROM's
 * address pins are on the CPU bus, so the internal ROM byte is CPU address
 * A0-A14 whatever page was mapped. $FFE0-$FFFF (the vectors) are always
 * the internal ROM. With TY set everything below $FF00 is RAM.
 *
 * Interrupts ($FF92 IRQENR, $FF93 FIRQENR; INIT0 IEN/FEN): bit 5 TMR (the
 * timer reached zero), 4 HBORD (the right border starts), 3 VBORD (the
 * bottom border starts), 2 EI2 (serial: a falling edge on the RS-232 pin
 * 4), 1 EI1 (keyboard: any of PIA0 PA0-PA6 low), 0 EI0 (cartridge: a
 * falling edge of CART). An event sets its bit in the IRQ status if
 * enabled there, and in the FIRQ status if enabled there; reading the
 * register returns the status and clears it; writing it clears the
 * status of sources it disables, and raises those it newly enables whose
 * condition holds now (the keyboard held, a stopped timer). /IRQ is the
 * IRQ status with IEN, /FIRQ the FIRQ status with FEN.
 *
 * Timer ($FF94 bits 0-3, $FF95; INIT1 TINS): 12 bits, counting at the line
 * rate (TINS 0: 63.695 us) or 3.579545 MHz (TINS 1: 279.365 ns). Writing
 * $FF94 (re)starts it from the value; $FF95 only stores. At zero it raises
 * TMR, flips the text blink and reloads; a value of 0 stops it. The 1986
 * GIME counts value + 2 and the 1987 value + 1 (Sockmaster).
 *
 * Video: see renderLine(). The VDG-compatible mode (INIT0 COCO) takes its
 * mode from PIA1 port B as written ($FF22, snooped by the GIME) and the
 * SAM's V and F bits; the CoCo 3 modes from VMODE ($FF98), VRES ($FF99),
 * BRDR ($FF9A), VSC ($FF9C), the vertical offset ($FF9D/$FF9E: address
 * bits 3-18) and the horizontal offset ($FF9F: HVEN, a 256-byte virtual
 * row, and X in 2-byte steps). Colours go through the 16 palette registers
 * ($FFB0-$FFBF, six bits: RGB as R1 G1 B1 R0 G0 B0; composite as two bits of
 * intensity and four of hue).
 */
class Gime {
public:
    /** The 1986 GIME (TCC1014 "86") or the 1987 one. */
    enum class Revision { G1986, G1987 };
    /** What a pixel's six bits mean: an RGB monitor (CM-8) or composite (a TV). */
    enum class Monitor { Rgb, Composite };

    static constexpr int LINE_TICKS = 912;     ///< a scan line in pixel-clock ticks
    static constexpr int OUT_W = 704;          ///< the frame: the 640-tick active area and 32 ticks of border each side
    static constexpr int OUT_H = 240;          ///< lines shown

    /** What the CPU's address selects. */
    enum Select { S_RAM, S_ROM, S_CTS, S_PIA0, S_PIA1, S_SCS, S_GIME, S_NONE };
    struct Decode {
        Select s;
        uint32_t phys;      ///< S_RAM: the byte in the RAM (masked to its size)
    };

    // Interrupt bits ($FF92 / $FF93)
    static constexpr uint8_t INT_TMR = 0x20, INT_HBORD = 0x10, INT_VBORD = 0x08,
                             INT_EI2 = 0x04, INT_EI1 = 0x02, INT_EI0 = 0x01;

    explicit Gime(Revision rev = Revision::G1987);

    void setRevision(Revision r) { m_rev = r; }
    Revision revision() const { return m_rev; }
    /** The RAM fitted: 128 or 512 KB. The machine owns the RAM (setRam). */
    void setRamKb(int kb) { m_ramMask = kb >= 512 ? 0x7FFFF : 0x1FFFF; }
    uint32_t ramMask() const { return m_ramMask; }
    void setRam(const uint8_t* ram) { m_ram = ram; }
    void setMonitor(Monitor m) { m_monitor = m; }
    Monitor monitor() const { return m_monitor; }
    /** A PAL board: composite is made from the RGB signals (no NTSC hues). */
    void setPal(bool pal) { m_pal = pal; }

    /** /RESET: every register 0 (ROM mapped, MMU off, COCO clear), the SAM bits 0. */
    void reset();

    // The CPU's side
    Decode decode(uint16_t a) const;
    /** $FF90-$FFDF: true if the GIME drives the data bus (a register it lets
     *  be read); reading IRQENR / FIRQENR acknowledges. */
    bool read(uint16_t a, uint8_t& v);
    bool peek(uint16_t a, uint8_t& v) const;
    void write(uint16_t a, uint8_t v);
    /** The GIME watches writes to PIA1 ($FF22 data, $FF23 control) for the VDG mode lines. */
    void snoopPia1(uint16_t a, uint8_t v);
    /** Pixel-clock ticks a CPU cycle takes (16, or 8 with R1). */
    int cycleTicks() const { return m_r1 ? 8 : 16; }

    // Interrupt inputs (level, true = the condition holds): EI0 the
    // cartridge (CART low), EI1 the keyboard (a row low), EI2 serial
    void setInterruptInput(uint8_t bit, bool active);
    bool irq() const { return m_irqOut; }
    bool firq() const { return m_firqOut; }
    uint8_t irqStatus() const { return m_irqState; }
    uint8_t firqStatus() const { return m_firqState; }

    /** Time passes: lines, sync edges, interrupts and the timer. */
    void advance(uint64_t ticks);
    uint64_t now() const { return m_now; }
    std::function<void(bool level, uint64_t at)> onHs;   ///< HSYNC (to PIA0 CA1)
    std::function<void(bool level, uint64_t at)> onFs;   ///< FSYNC (to PIA0 CB1)
    std::function<void()> onIrq;                          ///< /IRQ or /FIRQ may have changed

    // State, for tests and the debugger
    uint8_t reg(int n) const { return m_reg[n & 15]; }
    uint8_t mmu(int n) const { return m_mmu[n & 15]; }
    uint8_t palette(int n) const { return m_palette[n & 15]; }
    uint16_t samBits() const { return m_sam; }
    bool coco() const { return m_reg[0] & 0x80; }
    bool mmuEnabled() const { return m_reg[0] & 0x40; }
    int task() const { return m_reg[1] & 1; }
    bool fast() const { return m_r1; }
    int timerCount() const { return m_timerCount; }
    bool blink() const { return m_blink; }
    int scanline() const { return (int)m_scanline; }
    int lineTick() const { return (int)m_lineTick; }
    /** Lines per field: 263 (60 Hz) or 314 (50 Hz, VMODE H50). */
    int linesPerField() const { return (int)m_lF; }

    /** The frame: OUT_W x OUT_H six-bit colours. */
    const uint8_t* frame() const { return m_frame.data(); }
    uint8_t pixel(int x, int y) const { return m_frame[(size_t)y * OUT_W + (size_t)x]; }
    /** The frame line a scan line shows on (-1 if off the frame). */
    int frameLine(int scanline) const;
    /** Where the 640-tick active area starts in the frame (32); a 512-tick one is 64 further. */
    int activeX() const { return m_activeX; }
    /** The first frame line of the active area (from the current mode). */
    int activeY() const;
    /** The frame as 0xAARRGGBB through the monitor's colours. */
    void renderArgb(uint32_t* out) const;
    /** A colour on an RGB monitor (R1 G1 B1 R0 G0 B0 -> 0x00, 0x55, 0xAA, 0xFF). */
    static uint32_t rgb(uint8_t c);
    /** A colour on an NTSC composite monitor (intensity bits 4-5, hue 0-3; hue 0 grey). */
    static uint32_t composite(uint8_t c);
    /** The colour on the monitor fitted (and MOCH: grey on composite). */
    uint32_t colour(uint8_t c) const;

    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    struct Timing {
        int timerOffset;
        int tHS;
        int tBP[2][2];          // [hi-res][gfx]
        int tLB[2][2][2];       // [hi-res][40-byte][gfx]
        int tAA[2];             // [40-byte]
        int tHsVb[2][2][2];     // [hi-res][40-byte][gfx]
    };
    static const Timing TIMING[2];
    const Timing& timing() const { return TIMING[m_rev == Revision::G1987 ? 1 : 0]; }

    void writeReg(int r, uint8_t v);
    void samChanged();
    void modeChanged();
    void raise(uint8_t bits);
    void outputs();
    void restartTimer();
    void timerExpired();
    void hsFall();
    void vbPoint();
    void hbPoint();
    void renderLine();
    uint8_t fetch();
    int hiRes() const { return coco() ? 0 : (m_reg[9] >> 4) & 1; }
    int wide() const { return coco() ? 0 : (m_reg[9] >> 2) & 1; }
    int gfx() const { return (m_reg[8] & 0x80) ? 1 : 0; }

    Revision m_rev;
    Monitor m_monitor = Monitor::Composite;
    bool m_pal = false;
    uint32_t m_ramMask = 0x1FFFF;
    const uint8_t* m_ram = nullptr;

    uint8_t m_reg[16] = {};
    uint8_t m_mmu[16] = {};
    uint8_t m_palette[16] = {};
    uint16_t m_sam = 0;
    bool m_ty = false, m_r1 = false;
    uint8_t m_samV = 0;
    uint16_t m_samF = 0;
    uint8_t m_pia1b = 0;                 // PIA1 port B as written (bits 3-7)
    bool m_pia1bData = false;            // PIA1 CRB bit 2: $FF22 is the data register

    uint8_t m_irqState = 0, m_firqState = 0;
    bool m_irqOut = false, m_firqOut = false;
    uint8_t m_inputs = 0;                // EI0-EI2 levels

    int m_timerCount = 0;
    int m_timerTicks = 0;                // TINS 1: pixel ticks to zero
    bool m_blink = false;

    // Line timing
    uint64_t m_now = 0;
    uint32_t m_lineTick = 0;
    uint32_t m_scanline = 0;
    int m_tVb = 0, m_tRb = 0, m_tAa = 0;
    bool m_vbDone = false, m_hbDone = false, m_hsRisen = false;

    // Vertical state
    uint32_t m_lF = 263, m_lTB = 36, m_lAA = 192;
    bool m_sync = true, m_active = false;
    uint32_t m_lcount = 0;

    // Video address
    uint32_t m_b = 0;
    uint32_t m_row = 0, m_rowMask = 0;
    bool m_rowAdvance = false;
    uint32_t m_xoff = 0, m_stride = 32, m_bpr = 32;
    int m_resolution = 1;
    uint8_t m_border = 0;
    bool m_haveCache = false;
    uint8_t m_cache = 0;
    uint32_t m_fetchX = 0;

    int m_activeX = 32;
    std::vector<uint8_t> m_frame;
};
