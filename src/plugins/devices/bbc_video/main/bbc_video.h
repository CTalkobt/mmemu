#pragma once

#include "libdevices/main/ivideo_output.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * The BBC Micro's video, after b-em (video.c, GPL-2.0-or-later): the 6845
 * CRTC at &FE00/&FE01, the video ULA at &FE20 (control) and &FE21
 * (palette), and the SAA5050 teletext chip for Mode 7.
 *
 * clock() runs the CRTC a 2 MHz cycle at a time, as b-em's video_poll: a
 * character every cycle with the ULA's fast clock (Modes 0-3), every other
 * cycle with the slow one (Modes 4-7). Bitmap modes take their bytes from
 * (MA << 3 | RA) with the screen-size wrap set by the System VIA's latch
 * (IC32); teletext takes them from &7C00 (MA bit 13). The cursor, the
 * blank lines of Modes 3 and 6, flashing colours and teletext flash are as
 * b-em has them. VSYNC drives the System VIA's CA1 through onVsync.
 *
 * The picture is 832 x 624: each scan line is drawn twice, Mode 7 with the
 * SAA5050's character rounding (b-em's 16 x 20 teletext font, both fields).
 */
class BbcVideo : public IVideoOutput {
public:
    static constexpr int WIDTH = 832;
    static constexpr int HEIGHT = 624;
    static constexpr int LINES = HEIGHT / 2;
    static constexpr int X_ORIGIN = 224;        // b-em x of the picture's left edge

    BbcVideo();

    /** 32 KB of RAM the video reads. */
    void setMemory(const uint8_t* ram) { m_ram = ram; }
    /** b-em's teletext font (BEMTTX01, 16 x 10 bytes per character, three banks). */
    bool loadTeletextFont(const std::string& path, std::string* err = nullptr);
    /** Set the font from bytes (tests). */
    bool setTeletextFont(const std::vector<uint8_t>& data, std::string* err = nullptr);
    /** The System VIA's latch bits 4-5: the screen size for the address wrap. */
    void setScreenSize(int size) { m_scrSize = size & 3; }
    std::function<void(bool)> onVsync;          // System VIA CA1

    void reset();
    /** Run `cycles` 2 MHz cycles. */
    void clock(uint64_t cycles);

    // &FE00-&FE07 (6845: address, data) and &FE20-&FE2F (the ULA)
    void crtcWrite(uint16_t addr, uint8_t v);
    uint8_t crtcRead(uint16_t addr) const;
    void ulaWrite(uint16_t addr, uint8_t v);
    void latchLightPen();

    uint8_t crtcReg(int r) const { return m_crtc[r & 31]; }
    uint8_t ulaControl() const { return m_ulaCtrl; }
    uint64_t frames() const { return m_frames; }

    VideoDimensions getDimensions() const override { return {WIDTH, HEIGHT, WIDTH, HEIGHT}; }
    /** The last complete frame. */
    void renderFrame(uint32_t* buffer) override;

private:
    enum CrtcMode { TELETEXT = 0, HIFREQ = 1, LOFREQ = 2 };
    void setReg(int reg, uint8_t v);
    void put(int x, int y, uint32_t c);
    void puts(int x, int y, int n, uint32_t c);
    uint32_t get(int x, int y) const;
    void mode7Render(uint8_t dat);
    void endFrame();

    const uint8_t* m_ram = nullptr;
    std::vector<uint32_t> m_canvas, m_frame;

    // 6845
    uint8_t m_crtc[32] = {};
    int m_crtcIndex = 0;
    int m_hc = 0, m_vc = 0, m_sc = 0, m_vadj = 0;
    uint16_t m_ma = 0, m_maBack = 0, m_ttxBank = 0x3C00;
    int m_vdispen = 0, m_dispen = 0;
    CrtcMode m_crtcMode = TELETEXT;

    // ULA
    uint8_t m_ulaCtrl = 0;
    uint32_t m_ulaPal[16] = {};
    uint8_t m_ulaPalBak[16] = {};
    int m_ulaMode = 0;
    uint8_t m_table4bpp[4][256][16] = {};

    // SAA5050
    std::vector<uint8_t> m_font;
    int m_m7Width = 16, m_m7BytesPerChar = 160;
    const uint8_t *m_m7Chars = nullptr, *m_m7Graph = nullptr, *m_m7SepGraph = nullptr;
    const uint8_t *m_m7P = nullptr, *m_m7HeldP = nullptr;
    uint32_t m_m7Lookup[8][8][16] = {};
    int m_m7Col = 7, m_m7Bg = 0, m_m7Sep = 0;
    int m_m7Dbl = 0, m_m7NextDbl = 0, m_m7WasDbl = 0, m_m7Gfx = 0;
    int m_m7Flash = 0, m_m7FlashOn = 0, m_m7FlashTime = 0;
    uint8_t m_m7Buf[2] = {};
    uint8_t m_m7HeldChar = 0x20, m_m7HoldChar = 0;

    // Raster
    int m_scrx = 0, m_scry = 0, m_scrSize = 0;
    int m_vsyncTime = 0, m_hvblCount = 0, m_frameOdd = 0;
    int m_con = 0, m_cdraw = 0, m_coff = 0, m_cursorOn = 0, m_frCount = 0;
    int m_charsLeft = 0, m_oddClock = 0;
    uint64_t m_frames = 0;
};
