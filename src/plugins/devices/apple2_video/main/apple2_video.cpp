// Apple II video: port of AppleWin's NTSC renderer (NTSC.cpp, NTSC_CharSet.cpp,
// Video.cpp), by William S Simms, Michael Pohoreski, Tom Charlesworth and the
// AppleWin team, GPL-2.0-or-later. See apple2_video.h.

#include "apple2_video.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr uint32_t ALPHA32_MASK = 0xFF000000u;

constexpr int HORZ_COLORBURST_BEG = 12;
constexpr int HORZ_COLORBURST_END = 16;
constexpr int MAX_HORZ = (int)Apple2Video::CYCLES_PER_LINE;     // 65
constexpr int MAX_VERT = (int)Apple2Video::LINES_PER_FRAME;     // 262
constexpr int Y_DISPLAY = (int)Apple2Video::VISIBLE_LINES;      // 192
constexpr int Y_MIXED = (int)Apple2Video::MIXED_LINE;           // 160
constexpr int HORZ_START = (int)Apple2Video::HORZ_START;        // 25

constexpr int NTSC_NUM_PHASES = 4;
constexpr int NTSC_NUM_SEQUENCES = 4096;

// ---------------------------------------------------------------------------
// Video scanner (AppleWin Video::VideoGetScannerAddress, after "Understanding
// the Apple IIe" (UTAIIe) chapters 3, 5 and 8). NTSC only.
// ---------------------------------------------------------------------------
enum class ScannerPart { Full, PartialV, PartialH };

uint16_t utaiieScannerAddress(uint32_t nCycles, bool hires, bool page2, bool store80, bool mixed,
                              bool isApple2, ScannerPart part) {
    const int kHClock0State = 0x18;  // H[543210] = 011000
    const int kHClocks = 65;
    const int kHPEClock = 40;        // HPE (horizontal preset enable) goes low
    const int kHPresetClock = 41;    // H state presets
    const int kScanLines = 262;
    const int kVLine0State = 0x100;  // V[543210CBA] = 100000000
    const int kVPresetLine = 256;

    nCycles %= (uint32_t)(kScanLines * kHClocks);

    const int nHClock = ((int)nCycles + kHPEClock) % kHClocks;
    int nHState = kHClock0State + nHClock;
    if (nHClock >= kHPresetClock) nHState -= 1;   // two 0 states
    const int h_0 = (nHState >> 0) & 1, h_1 = (nHState >> 1) & 1, h_2 = (nHState >> 2) & 1;
    const int h_3 = (nHState >> 3) & 1, h_4 = (nHState >> 4) & 1, h_5 = (nHState >> 5) & 1;

    const int nVLine = (int)nCycles / kHClocks;
    int nVState = kVLine0State + nVLine;
    if (nVLine >= kVPresetLine) nVState -= kScanLines;
    const int v_A = (nVState >> 0) & 1, v_B = (nVState >> 1) & 1, v_C = (nVState >> 2) & 1;
    const int v_0 = (nVState >> 3) & 1, v_1 = (nVState >> 4) & 1, v_2 = (nVState >> 5) & 1;
    const int v_3 = (nVState >> 6) & 1, v_4 = (nVState >> 7) & 1;

    if (hires && mixed && v_4 && v_2) hires = false;   // HIRES TIME (UTAIIe:5-7)

    const int nAddend0 = 0x0D;
    const int nAddend1 = (h_5 << 2) | (h_4 << 1) | (h_3 << 0);
    const int nAddend2 = (v_4 << 3) | (v_3 << 2) | (v_4 << 1) | (v_3 << 0);
    const int nSum = (nAddend0 + nAddend1 + nAddend2) & 0x0F;   // UTAIIe:5-9

    uint16_t nAddressH = (uint16_t)((h_0 << 0) | (h_1 << 1) | (h_2 << 2) | (nSum << 3));
    if (!hires && isApple2 && !h_5 && (!h_4 || !h_3))
        nAddressH |= 1 << 12;   // Apple II HBL: +$1000 (UTAIIe:I-4,#5)

    uint16_t nAddressV = (uint16_t)((v_0 << 7) | (v_1 << 8) | (v_2 << 9));
    const int p2a = !(page2 && !store80) ? 1 : 0;
    const int p2b = (page2 && !store80) ? 1 : 0;
    uint16_t nAddressP = 0;
    if (hires) {
        nAddressV |= (uint16_t)((v_A << 10) | (v_B << 11) | (v_C << 12));
        nAddressP |= (uint16_t)((p2a << 13) | (p2b << 14));
    } else {
        nAddressP |= (uint16_t)((p2a << 10) | (p2b << 11));
    }

    if (part == ScannerPart::PartialH) return nAddressH;
    if (part == ScannerPart::PartialV) return nAddressV;
    return (uint16_t)(nAddressP | nAddressV | nAddressH);
}

// ---------------------------------------------------------------------------
// Composite colour tables (AppleWin initChromaPhaseTables and its filters)
// ---------------------------------------------------------------------------
// AppleWin's constants are floats (3.1415926535898f, ...); keep them so the tables match
constexpr float PI = 3.1415926535898f;
constexpr float RAD_45 = PI * 0.25f;
constexpr float RAD_90 = PI * 0.5f;
constexpr float CYCLESTART = PI * 45 / 180.f;

constexpr float CHROMA_GAIN = 7.438011255f;
constexpr float CHROMA_0 = -0.7318893645f;
constexpr float CHROMA_1 = 1.2336442711f;
constexpr float LUMA_GAIN = 13.71331570f;
constexpr float LUMA_0 = -0.3961075449f;
constexpr float LUMA_1 = 1.1044202472f;
constexpr float SIGNAL_GAIN = 7.614490548f;
constexpr float SIGNAL_0 = -0.2718798058f;
constexpr float SIGNAL_1 = 0.7465656072f;

/** A 2-pole IIR filter (AppleWin's initFilter* with their static state). */
struct Filter2 {
    double x[3] = {0, 0, 0};
    double y[3] = {0, 0, 0};
    double lowPass(double z, float gain, float c0, float c1) {
        x[0] = x[1]; x[1] = x[2]; x[2] = z / gain;
        y[0] = y[1]; y[1] = y[2]; y[2] = x[0] + x[2] + (2.f * x[1]) + (c0 * y[0]) + (c1 * y[1]);
        return y[2];
    }
    double bandPass(double z) {   // initFilterChroma: inverted x[0]
        x[0] = x[1]; x[1] = x[2]; x[2] = z / CHROMA_GAIN;
        y[0] = y[1]; y[1] = y[2]; y[2] = -x[0] + x[2] + (CHROMA_0 * y[0]) + (CHROMA_1 * y[1]);
        return y[2];
    }
};

inline float clampZeroOne(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

inline uint32_t argb(float r, float g, float b) {
    return ALPHA32_MASK | ((uint32_t)(uint8_t)(r * 255) << 16) | ((uint32_t)(uint8_t)(g * 255) << 8) |
           (uint32_t)(uint8_t)(b * 255);
}

/** The tables AppleWin builds once at start-up, shared by all instances. */
struct Tables {
    uint32_t hueColorTV[NTSC_NUM_PHASES][NTSC_NUM_SEQUENCES];   // g_aHueColorTV
    uint32_t bnwColorTV[NTSC_NUM_SEQUENCES];                    // g_aBnWColorTVCustom (white)
    uint16_t pixelDoubleMaskHGR[128];
    uint16_t pixelMaskGR[16];
    uint16_t vertOffsetsHGR[MAX_VERT];
    uint16_t vertOffsetsTXT[(256 + 8) / 8];
    uint16_t horzOffsetIIP[5][MAX_HORZ];
    uint16_t horzOffsetIIE[5][MAX_HORZ];

    Tables() {
        initChroma();
        initPixelMasks();
        initScanner();
    }

    void initChroma() {
        Filter2 fSignal, fChroma, fLuma0, fLuma1;
        for (int phase = 0; phase < NTSC_NUM_PHASES; ++phase) {
            double phi = (phase * RAD_90) + CYCLESTART;
            for (int s = 0; s < NTSC_NUM_SEQUENCES; ++s) {
                int t = s;
                double y0 = 0, y1 = 0, c = 0, i = 0, q = 0;
                for (int n = 0; n < 12; ++n) {
                    const double z = (double)(0 != (t & 0x800));
                    t = t << 1;
                    for (int k = 0; k < 2; k++) {
                        const double zz = fSignal.lowPass(z, SIGNAL_GAIN, SIGNAL_0, SIGNAL_1);
                        c = fChroma.bandPass(zz);
                        y0 = fLuma0.lowPass(zz, LUMA_GAIN, LUMA_0, LUMA_1);
                        y1 = fLuma1.lowPass(zz - c, LUMA_GAIN, LUMA_0, LUMA_1);
                        c = c * 2.f;
                        i = i + (c * std::cos(phi) - i) / 8.f;
                        q = q + (c * std::sin(phi) - q) / 8.f;
                        phi += RAD_45;
                    }
                }
                (void)y0;   // used only by AppleWin's monitor tables

                // B&W TV: luma y1, then updateMonochromeTables(0xFF,0xFF,0xFF)
                const uint8_t lum = (uint8_t)(clampZeroOne((float)y1) * 255);
                const uint32_t l = (uint32_t)((lum * 0xFF) >> 8);
                bnwColorTV[s] = ALPHA32_MASK | (l << 16) | (l << 8) | l;

                // Colour TV: YIQ to RGB
                const float I_TO_R = 0.956f, I_TO_G = -0.272f, I_TO_B = -1.105f;
                const float Q_TO_R = 0.621f, Q_TO_G = -0.647f, Q_TO_B = 1.702f;
                float r32 = clampZeroOne((float)(y1 + (I_TO_R * i) + (Q_TO_R * q)));
                float g32 = clampZeroOne((float)(y1 + (I_TO_G * i) + (Q_TO_G * q)));
                float b32 = clampZeroOne((float)(y1 + (I_TO_B * i) + (Q_TO_B * q)));
                const int color = s & 15;
                if (color == 15) r32 = g32 = b32 = 1;   // NTSC_REMOVE_WHITE_RINGING
                if (color == 0) r32 = g32 = b32 = 0;    // NTSC_REMOVE_BLACK_GHOSTING
                hueColorTV[phase][s] = argb(r32, g32, b32);
            }
        }
    }

    void initPixelMasks() {
        std::memset(pixelDoubleMaskHGR, 0, sizeof(pixelDoubleMaskHGR));
        for (int byte = 0; byte < 0x80; byte++)
            for (int bit = 0; bit < 7; bit++)
                if (byte & (1 << bit)) pixelDoubleMaskHGR[byte] |= (uint16_t)(3 << (bit * 2));
        for (int c = 0; c < 16; c++)
            pixelMaskGR[c] = (uint16_t)((c << 12) | (c << 8) | (c << 4) | c);
    }

    // AppleWin GenerateVideoTables (NTSC)
    void initScanner() {
        for (uint32_t i = 0, cycle = HORZ_START; i < (uint32_t)MAX_VERT; i++, cycle += MAX_HORZ)
            vertOffsetsHGR[i] = utaiieScannerAddress(cycle, true, false, false, false, false, ScannerPart::PartialV);
        for (uint32_t i = 0, cycle = HORZ_START; i < (256 + 8) / 8; i++, cycle += MAX_HORZ * 8)
            vertOffsetsTXT[i] = utaiieScannerAddress(cycle, false, false, false, false, false, ScannerPart::PartialV);
        for (uint32_t j = 0; j < 5; j++) {
            for (uint32_t i = 0, cycle = j * 64 * MAX_HORZ; i < (uint32_t)MAX_HORZ; i++, cycle++) {
                horzOffsetIIP[j][i] = utaiieScannerAddress(cycle, false, false, false, false, true, ScannerPart::PartialH);
                horzOffsetIIE[j][i] = utaiieScannerAddress(cycle, false, false, false, false, false, ScannerPart::PartialH);
            }
        }
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

}  // namespace

// ===========================================================================
// Construction, ROM, memory, mode
// ===========================================================================

Apple2Video::Apple2Video(Model m) : m_model(m) {
    tables();
    m_back.assign((size_t)BUF_WIDTH * BUF_HEIGHT, ALPHA32_MASK);
    m_front.assign((size_t)FRAME_WIDTH * FRAME_HEIGHT, ALPHA32_MASK);
    reset();
}

// AppleWin NTSC_CharSet.cpp userVideoRom2K (Apple II / II+ type)
static void decodeVideoRom2K(uint8_t cs[256][8], const uint8_t* rom) {
    for (int ch = 0; ch < 256; ch++) {
        const int RA = ch * 8;
        for (int y = 0; y < 8; y++) {
            uint8_t n = rom[RA + y];
            // UTAII:8-30/31: in the low 1 KB, bit 7 clear means an inverse pattern
            if (RA < 1024 && !(n & 0x80)) n = n ^ 0x7f;
            uint8_t d = 0;
            for (int j = 0; j < 7; j++, n >>= 1)   // pattern is reversed (UTAII:8-30)
                d = (uint8_t)((d << 1) | (n & 1));
            cs[ch][y] = d;
        }
    }
}

// AppleWin NTSC_CharSet.cpp userVideoRom4K (IIe 2732 video ROM, UTAIIe:8-14 Table 8.3)
static void decodeVideoRom4K(uint8_t cs[2][256][8], const uint8_t* rom) {
    int RA = 0;
    for (int i = 0; i < 64; i++, RA += 8) {        // [00..3F] INVERSE / [40..7F] FLASH
        for (int y = 0; y < 8; y++) {
            cs[0][i][y] = rom[RA + y] ^ 0xff;      // dot patterns are inverted (UTAIIe:8-11)
            cs[0][i + 64][y] = rom[RA + y] ^ 0xff;
        }
    }
    RA = (1 << 10 | 0 << 9);
    for (int i = 128; i < 256; i++, RA += 8)       // [80..BF] NORMAL
        for (int y = 0; y < 8; y++) cs[0][i][y] = rom[RA + y] ^ 0xff;
    RA = (1 << 10 | 1 << 9);
    for (int i = 192; i < 256; i++, RA += 8)       // [C0..FF] NORMAL
        for (int y = 0; y < 8; y++) cs[0][i][y] = rom[RA + y] ^ 0xff;
    RA = 0;
    for (int i = 0; i < 256; i++, RA += 8)         // alt set: [00..7F] INVERSE / [80..FF] NORMAL
        for (int y = 0; y < 8; y++) cs[1][i][y] = rom[RA + y] ^ 0xff;
}

void Apple2Video::setCharRom(std::vector<uint8_t> rom) {
    m_charRom = std::move(rom);
    std::memset(m_csbits, 0, sizeof(m_csbits));
    const size_t size = m_charRom.size();
    if (size == 2048) {
        decodeVideoRom2K(m_csbits[0], m_charRom.data());
        std::memcpy(m_csbits[1], m_csbits[0], sizeof(m_csbits[0]));
    } else if (size == 4096 || size == 8192) {
        // An 8 KB (2764) ROM holds the PAL set in the low 4 KB; AppleWin's US set is the top 4 KB
        decodeVideoRom4K(m_csbits, m_charRom.data() + (size == 8192 ? 4096 : 0));
        if (m_model == Model::IIe)   // original IIe: no MouseText, [1][40..5F] = [0][40..5F]
            std::memcpy(&m_csbits[1][64], &m_csbits[0][64], 32 * 8);
    }
}

void Apple2Video::setMemory(const uint8_t* mainRam, const uint8_t* auxRam) {
    m_main = mainRam;
    m_aux = auxRam;
}

// AppleWin Video::VideoSetMode (the part after the switch is decoded) + NTSC_SetVideoMode
void Apple2Video::setMode(uint32_t vfFlags) {
    uint32_t flags = vfFlags & (VF_80COL | VF_DHIRES | VF_HIRES | VF_80STORE | VF_MIXED | VF_PAGE2 | VF_TEXT);
    if (!isIIe()) flags &= ~(VF_80COL | VF_DHIRES | VF_80STORE);

    // NTSC_SetVideoTextMode follows the 80COL switch itself, at once
    m_textScreen = (flags & VF_80COL) ? Screen::Text80 : Screen::Text40;

    if (isIIe() && m_aux == nullptr) {   // aux slot empty (GH#1341)
        flags &= ~VF_DHIRES;
        if (!(flags & VF_TEXT)) flags &= ~VF_80COL;
    }

    // Only TEXT and MIXED changes are delayed by one cycle (GH#656)
    const bool delay = ((m_newModeFlags ^ flags) & (VF_TEXT | VF_MIXED)) != 0;
    m_newModeFlags = flags;
    if (delay) {
        m_delayMode = true;
        return;
    }
    applyMode(flags);
}

void Apple2Video::setAltCharSet(bool on) {
    m_altCharSet = on && isIIe();
    m_charSet = m_altCharSet ? 1 : 0;
}

// AppleWin NTSC_SetVideoMode, VT_COLOR_TV, NTSC
void Apple2Video::applyMode(uint32_t flags) {
    m_mixed = (flags & VF_MIXED) != 0;
    m_charSet = m_altCharSet ? 1 : 0;

    m_textPage = 1;
    m_hiresPage = 1;
    if ((flags & VF_PAGE2) && !(flags & VF_80STORE)) {   // 80STORE must be off to display page 2
        m_textPage = 2;
        m_hiresPage = 2;
    }

    if (flags & VF_TEXT) {
        m_graphicsScreen = (flags & VF_80COL) ? Screen::Text80 : Screen::Text40;
    } else if (flags & VF_HIRES) {
        if (flags & VF_DHIRES)
            m_graphicsScreen = (flags & VF_80COL) ? Screen::DoubleHires80 : Screen::DoubleHires40;
        else
            m_graphicsScreen = Screen::Hires40;
    } else {
        if (flags & VF_DHIRES)
            m_graphicsScreen = (flags & VF_80COL) ? Screen::DoubleLores80 : Screen::DoubleLores40;
        else
            m_graphicsScreen = Screen::Lores40;
    }
}

void Apple2Video::reset() {
    m_vert = 0;
    m_horz = 0;
    m_altCharSet = false;
    m_charSet = 0;
    m_delayMode = false;
    m_newModeFlags = VF_TEXT;
    m_textScreen = Screen::Text40;
    applyMode(VF_TEXT);
    m_colorBurstPixels = 0;
    m_flashCounter = 0;
    m_flashMask = 0;
    m_frameCount = 0;
    std::fill(m_back.begin(), m_back.end(), ALPHA32_MASK);
    std::fill(m_front.begin(), m_front.end(), ALPHA32_MASK);
    updateScannerAddress();
}

// ===========================================================================
// Scanner addresses
// ===========================================================================

uint16_t Apple2Video::addrTXT(int v, int h) const {
    const Tables& t = tables();
    const auto& horzTable = isIIe() ? t.horzOffsetIIE : t.horzOffsetIIP;
    return (uint16_t)(t.vertOffsetsTXT[v / 8] + horzTable[v / 64][h] + m_textPage * 0x400);
}

uint16_t Apple2Video::addrHGR(int v, int h) const {
    // Both the II and the IIe use the IIe horizontal table here: only text adds the HBL $1000
    const Tables& t = tables();
    return (uint16_t)(t.vertOffsetsHGR[v] + t.horzOffsetIIE[v / 64][h] + m_hiresPage * 0x2000);
}

uint16_t Apple2Video::addrTXTorHGR(int v, int h) const {
    const bool isTextAddr = (m_mixed && v >= Y_MIXED) || (m_newModeFlags & VF_TEXT) || !(m_newModeFlags & VF_HIRES);
    return isTextAddr ? addrTXT(v, h) : addrHGR(v, h);
}

// AppleWin NTSC_VideoGetScannerAddress: one cycle behind the beam (for ANSI STORY)
uint16_t Apple2Video::scannerAddress() const {
    int h = (int)m_horz - 1;
    int v = m_vert;
    if (h < 0) {
        h += MAX_HORZ;
        v = (v == 0) ? MAX_VERT - 1 : v - 1;
    }
    return addrTXTorHGR(v, h);
}

uint8_t Apple2Video::floatingBus() const { return mainByte(scannerAddress()); }

uint16_t Apple2Video::loResBits(uint8_t b) const {
    return tables().pixelMaskGR[(b >> (m_vert & 4)) & 0xF];
}

// ===========================================================================
// Pixels (VT_COLOR_TV, VS_HALF_SCANLINES: updatePixelHue/BnWColorTVSingleScanline)
// ===========================================================================

// updateFramebufferTVSingleScanline: the in-between line above (GH#650) is
// 50% of the blend of this line and the one above it
void Apple2Video::writeTVPixel(uint16_t signal, const uint32_t* table) {
    m_signalBits = ((m_signalBits << 1) | signal) & 0xFFF;
    const uint32_t color0 = table[m_signalBits];
    const uint32_t color2 = m_back[m_videoAddress - 2 * BUF_WIDTH];
    uint32_t color1 = ((color0 & 0x00fefefe) >> 1) + ((color2 & 0x00fefefe) >> 1);
    color1 = (color1 & 0x00fefefe) >> 1;
    m_back[m_videoAddress - BUF_WIDTH] = color1 | ALPHA32_MASK;
    m_back[m_videoAddress] = color0;
    if (m_vert == Y_DISPLAY - 1)   // last in-between line: 25% of the current line
        m_back[m_videoAddress + BUF_WIDTH] = ((color0 & 0x00fcfcfc) >> 2) | ALPHA32_MASK;
    m_videoAddress++;
}

void Apple2Video::pixelHue(uint16_t signal) {
    writeTVPixel(signal, tables().hueColorTV[m_colorPhase]);
    m_colorPhase = (m_colorPhase + 1) & 3;
}

void Apple2Video::pixelBnW(uint16_t signal) {
    writeTVPixel(signal, tables().bnwColorTV);
    m_colorPhase = (m_colorPhase + 1) & 3;   // keep the phase for mid-line mode switches
}

// 14 dots (one CPU cycle), bit 0 first. Leaves bit 13 as the last column pixel.
void Apple2Video::updatePixels(uint16_t bits) {
    if (!colorBurst()) {
        for (int i = 0; i < 13; i++, bits >>= 1) pixelBnW(bits & 1);
        pixelBnW(bits & 1);
    } else {
        for (int i = 0; i < 13; i++, bits >>= 1) pixelHue(bits & 1);
        pixelHue(bits & 1);
    }
    m_lastColumnPixel = bits & 1;
}

// ===========================================================================
// Beam
// ===========================================================================

// AppleWin updateVideoScannerAddress (VT_COLOR_TV, no VidHD)
void Apple2Video::updateScannerAddress() {
    const int row = m_vert < Y_DISPLAY ? 2 * m_vert : 0;
    m_videoAddress = (BUF_TOP + row) * BUF_WIDTH + BUF_LEFT;
    m_videoAddress -= 2;   // TV: 2 14M pixels of pre-render

    // GH#555: the 14M modes start one 14M pixel early
    if (m_graphicsScreen == Screen::DoubleHires80 || m_graphicsScreen == Screen::DoubleLores80 ||
        m_graphicsScreen == Screen::Text80 ||
        (m_mixed && m_vert >= Y_MIXED && m_textScreen == Screen::Text80))
        m_videoAddress -= 1;

    m_colorPhase = 0;
    m_lastColumnPixel = 0;
    m_signalBits = 0;
}

// AppleWin updateVideoScannerHorzEOL (+ frame publication at line 192)
void Apple2Video::updateHorzEOL() {
    if (++m_horz != MAX_HORZ) return;

    if (m_vert < Y_DISPLAY) {
        if (!colorBurst()) {
            pixelBnW((uint16_t)m_lastColumnPixel);   // last pixel in 14M modes
            pixelBnW(0);                             // 14M ringing pixel
            m_back[m_videoAddress++] = ALPHA32_MASK;
            m_back[m_videoAddress++] = ALPHA32_MASK;
        } else {
            pixelHue((uint16_t)m_lastColumnPixel);
            pixelHue(0);
            for (int i = 0; i < 2; i++) {             // clear junk on the right (GH#1157)
                m_back[m_videoAddress] = ALPHA32_MASK;
                m_back[m_videoAddress + BUF_WIDTH] = ALPHA32_MASK;
                m_videoAddress++;
            }
        }
    }

    m_horz = 0;
    if (++m_vert == MAX_VERT) {
        m_vert = 0;
        // updateFlashRate: NTSC 60/16 ~= 4 Hz toggles
        if ((++m_flashCounter & 0xF) == 0) m_flashMask ^= 0xFFFF;
    }
    if (m_vert == Y_DISPLAY) publishFrame();
    if (m_vert < Y_DISPLAY) updateScannerAddress();
}

void Apple2Video::publishFrame() {
    for (int y = 0; y < FRAME_HEIGHT; y++)
        std::memcpy(&m_front[(size_t)y * FRAME_WIDTH], &m_back[(size_t)(BUF_TOP + y) * BUF_WIDTH + BUF_LEFT],
                    FRAME_WIDTH * sizeof(uint32_t));
    m_frameCount++;
}

void Apple2Video::colorBurstOn() { m_colorBurstPixels = 1024; }

// ===========================================================================
// Screen updates (AppleWin updateScreen*, VT_COLOR_TV variants)
// ===========================================================================

void Apple2Video::updateScreen(Screen s, long cycles) {
    if (s == Screen::Text40) updateText40(cycles);
    else if (s == Screen::Text80) updateText80(cycles);
    else updateGraphics(s, cycles);
}

void Apple2Video::updateGraphics(Screen s, long cycles) {
    if (graphicsShowsText()) {
        updateScreen(m_textScreen, cycles);
        return;
    }
    const Tables& t = tables();
    for (; cycles > 0; --cycles) {
        if (m_vert < Y_DISPLAY) {
            if (m_horz < HORZ_COLORBURST_END && m_horz >= HORZ_COLORBURST_BEG) {
                colorBurstOn();
            } else if (m_horz >= HORZ_START) {
                const int odd = m_horz & 1;
                switch (s) {
                case Screen::Hires40: {             // updateScreenSingleHires40
                    const uint8_t m = mainByte(addrHGR(m_vert, m_horz));
                    uint16_t bits = t.pixelDoubleMaskHGR[m & 0x7F];
                    if (m & 0x80) bits = (uint16_t)((bits << 1) | m_lastColumnPixel);   // half-dot delay
                    updatePixels(bits);
                    // GH#555: don't carry a pixel past the last column
                    if (m_horz == MAX_HORZ - 1) m_lastColumnPixel = 0;
                    break;
                }
                case Screen::DoubleHires40: {       // updateScreenDoubleHires40: no half-dot shift
                    const uint8_t m = mainByte(addrHGR(m_vert, m_horz));
                    updatePixels(t.pixelDoubleMaskHGR[m & 0x7F]);
                    break;
                }
                case Screen::DoubleHires80: {
                    const uint16_t addr = addrHGR(m_vert, m_horz);
                    const uint8_t m = mainByte(addr), a = auxByte(addr);
                    uint16_t bits = (uint16_t)(((m & 0x7f) << 7) | (a & 0x7f));
                    bits = (uint16_t)((bits << 1) | m_lastColumnPixel);
                    updatePixels(bits);
                    m_lastColumnPixel = (bits >> 14) & 1;
                    break;
                }
                case Screen::Lores40: {             // updateScreenSingleLores40
                    const uint16_t lo = loResBits(mainByte(addrTXT(m_vert, m_horz)));
                    updatePixels((uint16_t)(lo >> ((1 - odd) * 2)));
                    break;
                }
                case Screen::DoubleLores40: {
                    const uint16_t lo = loResBits(mainByte(addrTXT(m_vert, m_horz)));
                    updatePixels(t.pixelDoubleMaskHGR[(0xFF & (lo >> ((1 - odd) * 2))) & 0x7F]);
                    break;
                }
                case Screen::DoubleLores80: {
                    const uint16_t addr = addrTXT(m_vert, m_horz);
                    const uint16_t lo = loResBits(mainByte(addr));
                    const uint16_t hi = loResBits(auxByte(addr));
                    const uint16_t mainBits = (uint16_t)(lo >> (((1 - odd) * 2) + 3));
                    const uint16_t auxBits = (uint16_t)(hi >> (((1 - odd) * 2) + 3));
                    const uint16_t bits = (uint16_t)((mainBits << 7) | (auxBits & 0x7f));
                    updatePixels(bits);
                    m_lastColumnPixel = (bits >> 14) & 1;
                    break;
                }
                default:
                    break;
                }
            }
        }
        updateHorzEOL();
    }
}

void Apple2Video::updateText40(long cycles) {
    const Tables& t = tables();
    for (; cycles > 0; --cycles) {
        if (m_horz < HORZ_COLORBURST_END && m_horz >= HORZ_COLORBURST_BEG) {
            if (m_colorBurstPixels > 0) m_colorBurstPixels -= 1;   // the TV's colour killer is slow
        } else if (m_vert < Y_DISPLAY && m_horz >= HORZ_START) {
            const uint8_t m = mainByte(addrTXT(m_vert, m_horz));
            const uint8_t c = charSetBits(m);
            uint16_t bits = t.pixelDoubleMaskHGR[c & 0x7F];
            if (m_charSet == 0 && (m & 0xC0) == 0x40)   // flash only if MouseText is not active
                bits ^= m_flashMask;
            updatePixels(bits);
        }
        updateHorzEOL();
    }
}

void Apple2Video::updateText80(long cycles) {
    for (; cycles > 0; --cycles) {
        if (m_horz < HORZ_COLORBURST_END && m_horz >= HORZ_COLORBURST_BEG) {
            if (m_colorBurstPixels > 0) m_colorBurstPixels -= 1;
        } else if (m_vert < Y_DISPLAY && m_horz >= HORZ_START) {
            const uint16_t addr = addrTXT(m_vert, m_horz);
            const uint8_t m = mainByte(addr);
            // No aux memory: the video reads the floating bus (MemReadFloatingBusFromNTSC)
            const uint8_t a = m_aux ? m_aux[addr] : floatingBus();
            uint16_t mainBits = charSetBits(m);
            uint16_t auxBits = charSetBits(a);
            if (m_charSet == 0 && (m & 0xC0) == 0x40) mainBits ^= m_flashMask;
            if (m_charSet == 0 && (a & 0xC0) == 0x40) auxBits ^= m_flashMask;
            uint16_t bits = (uint16_t)((mainBits << 7) | (auxBits & 0x7f));
            bits = (uint16_t)((bits << 1) | m_lastColumnPixel);   // GH#555: align TEXT80 with DHGR
            updatePixels(bits);
            m_lastColumnPixel = (bits >> 14) & 1;
        }
        updateHorzEOL();
    }
}

// AppleWin VideoUpdateCycles: split at line 160 so mixed mode switches screens there
void Apple2Video::updateCycles(int cyclesLeft) {
    const int cyclesToEndOfLine = MAX_HORZ - m_horz;
    const int frameCycles = (int)CYCLES_PER_FRAME;
    if (m_vert < Y_MIXED) {
        const int cyclesToLine160 = MAX_HORZ * (Y_MIXED - m_vert - 1) + cyclesToEndOfLine;
        int cycles = std::min(cyclesLeft, cyclesToLine160);
        updateScreen(m_graphicsScreen, cycles);   // lines [v..159]
        cyclesLeft -= cycles;
        const int line160To261 = frameCycles - MAX_HORZ * Y_MIXED;
        cycles = std::min(cyclesLeft, line160To261);
        updateScreen(m_graphicsScreen, cycles);   // lines [160..261]
        cyclesLeft -= cycles;
    } else {
        const int cyclesToLine262 = MAX_HORZ * (MAX_VERT - m_vert - 1) + cyclesToEndOfLine;
        int cycles = std::min(cyclesLeft, cyclesToLine262);
        updateScreen(m_graphicsScreen, cycles);   // lines [v..261]
        cyclesLeft -= cycles;
        const int line0To159 = MAX_HORZ * Y_MIXED;
        cycles = std::min(cyclesLeft, line0To159);
        updateScreen(m_graphicsScreen, cycles);   // lines [0..159]
        cyclesLeft -= cycles;
    }
    if (cyclesLeft > 0) updateScreen(m_graphicsScreen, cyclesLeft);
}

// AppleWin NTSC_VideoUpdateCycles, in chunks of under a frame
void Apple2Video::clock(uint32_t cpuCycles) {
    if (cpuCycles == 0) return;
    if (m_delayMode) {
        updateCycles(1);   // the mode change lands one cycle late
        m_delayMode = false;
        applyMode(m_newModeFlags);
        if (--cpuCycles == 0) return;
    }
    while (cpuCycles > 0) {
        const uint32_t n = std::min(cpuCycles, CYCLES_PER_FRAME - 1);
        updateCycles((int)n);
        cpuCycles -= n;
    }
}

// ===========================================================================
// IVideoOutput
// ===========================================================================

IVideoOutput::VideoDimensions Apple2Video::getDimensions() const {
    return {FRAME_WIDTH, FRAME_HEIGHT, FRAME_WIDTH, FRAME_HEIGHT};
}

void Apple2Video::renderFrame(uint32_t* buffer) {
    std::memcpy(buffer, m_front.data(), m_front.size() * sizeof(uint32_t));
}
