#include "gtia.h"
#include <cstring>

// NTSC Atari 8-bit palette, 0xAARRGGBB (the IVideoOutput frame format).
// Index = hue << 4 | luminance. Generated with atari800's NTSC palette model
// (src/colours_ntsc.c, "standard" preset: colour delay 26.8 degrees, CGIA.PDF
// luma steps, black/white levels 16/235, CRT gamma 2.35 converted to sRGB).
const uint32_t GTIA::s_palette[256] = {
    0xFF040404, 0xFF0F0F0F, 0xFF1B1B1B, 0xFF272727, 0xFF333333, 0xFF414141, 0xFF4F4F4F, 0xFF5E5E5E,
    0xFF686868, 0xFF787878, 0xFF898989, 0xFF9A9A9A, 0xFFABABAB, 0xFFBFBFBF, 0xFFD3D3D3, 0xFFEAEAEA,
    0xFF041600, 0xFF0F2200, 0xFF1B2D00, 0xFF273A00, 0xFF334600, 0xFF415300, 0xFF4F6200, 0xFF5E7001,
    0xFF687B08, 0xFF788A18, 0xFF899B29, 0xFF9AAD3B, 0xFFABBE4C, 0xFFBFD160, 0xFFD3E574, 0xFFEAFC8C,
    0xFF1C0800, 0xFF281300, 0xFF331F00, 0xFF3F2B00, 0xFF4B3700, 0xFF594500, 0xFF675300, 0xFF766207,
    0xFF806C12, 0xFF907C22, 0xFFA18D34, 0xFFB29E45, 0xFFC3B057, 0xFFD7C36A, 0xFFEBD77E, 0xFFFFEE96,
    0xFF2F0000, 0xFF3B0500, 0xFF460F00, 0xFF521C00, 0xFF5E2800, 0xFF6C3607, 0xFF7A4416, 0xFF895324,
    0xFF935D2F, 0xFFA36D3F, 0xFFB47E51, 0xFFC58F62, 0xFFD6A173, 0xFFE9B487, 0xFFFDC89B, 0xFFFFDFB2,
    0xFF390000, 0xFF450002, 0xFF50040A, 0xFF5C0F17, 0xFF681B23, 0xFF762931, 0xFF84373F, 0xFF92464E,
    0xFF9D5058, 0xFFAC6068, 0xFFBD7179, 0xFFCE838A, 0xFFE0949C, 0xFFF3A7AF, 0xFFFFBBC3, 0xFFFFD2DA,
    0xFF380020, 0xFF43002C, 0xFF4E0037, 0xFF5A0743, 0xFF66134F, 0xFF74215D, 0xFF822F6B, 0xFF913E7A,
    0xFF9B4884, 0xFFAB5894, 0xFFBC6AA5, 0xFFCD7BB6, 0xFFDE8CC7, 0xFFF1A0DB, 0xFFFFB4EF, 0xFFFFCBFF,
    0xFF2B0047, 0xFF360052, 0xFF42005E, 0xFF4E066A, 0xFF5A1276, 0xFF672083, 0xFF762E91, 0xFF843DA0,
    0xFF8E47AA, 0xFF9E57BA, 0xFFAF69CB, 0xFFC07ADC, 0xFFD18BED, 0xFFE59FFF, 0xFFF9B3FF, 0xFFFFCAFF,
    0xFF16005F, 0xFF21006A, 0xFF2D0275, 0xFF390C81, 0xFF45188D, 0xFF53269B, 0xFF6135A9, 0xFF6F43B7,
    0xFF7A4EC1, 0xFF895DD1, 0xFF9B6FE2, 0xFFAC80F3, 0xFFBD91FF, 0xFFD0A5FF, 0xFFE4B9FF, 0xFFFBD0FF,
    0xFF010063, 0xFF08026F, 0xFF130B7A, 0xFF201886, 0xFF2C2492, 0xFF3A329F, 0xFF4840AD, 0xFF564FBC,
    0xFF6159C6, 0xFF7169D5, 0xFF827AE7, 0xFF938CF8, 0xFFA49DFF, 0xFFB8B0FF, 0xFFCCC4FF, 0xFFE3DBFF,
    0xFF000453, 0xFF000F5F, 0xFF011B6A, 0xFF082776, 0xFF143382, 0xFF224190, 0xFF314F9E, 0xFF3F5EAC,
    0xFF4A68B6, 0xFF5A78C6, 0xFF6B89D7, 0xFF7C9AE8, 0xFF8DACF9, 0xFFA1BFFF, 0xFFB5D3FF, 0xFFCCEAFF,
    0xFF001332, 0xFF001E3E, 0xFF002A49, 0xFF003655, 0xFF044261, 0xFF11506F, 0xFF205E7D, 0xFF2E6D8B,
    0xFF397796, 0xFF4987A5, 0xFF5A98B7, 0xFF6CA9C8, 0xFF7DBAD9, 0xFF90CEEC, 0xFFA4E2FF, 0xFFBCF9FF,
    0xFF001F07, 0xFF002A12, 0xFF00361E, 0xFF00422A, 0xFF014E36, 0xFF0A5C44, 0xFF196A53, 0xFF287861,
    0xFF32836B, 0xFF42927B, 0xFF54A38C, 0xFF65B59E, 0xFF76C6AF, 0xFF8AD9C2, 0xFF9EEDD6, 0xFFB5FFED,
    0xFF002500, 0xFF003000, 0xFF003C00, 0xFF004801, 0xFF03540A, 0xFF0F6118, 0xFF1E7027, 0xFF2D7E35,
    0xFF378840, 0xFF479850, 0xFF59A961, 0xFF6ABA72, 0xFF7BCC84, 0xFF8FDF97, 0xFFA3F3AB, 0xFFBAFFC2,
    0xFF002400, 0xFF002F00, 0xFF003B00, 0xFF054700, 0xFF115300, 0xFF1F6000, 0xFF2E6F04, 0xFF3C7D12,
    0xFF47871D, 0xFF57972D, 0xFF68A83E, 0xFF79B950, 0xFF8ACA61, 0xFF9EDE75, 0xFFB2F289, 0xFFC9FFA0,
    0xFF001C00, 0xFF052700, 0xFF103300, 0xFF1C3F00, 0xFF284B00, 0xFF365900, 0xFF446700, 0xFF537502,
    0xFF5D8009, 0xFF6D8F1A, 0xFF7EA12B, 0xFF90B23D, 0xFFA1C34E, 0xFFB4D662, 0xFFC8EA76, 0xFFDFFF8D,
    0xFF120E00, 0xFF1D1A00, 0xFF292600, 0xFF353200, 0xFF413E00, 0xFF4F4C00, 0xFF5D5A00, 0xFF6C6802,
    0xFF76730B, 0xFF86821B, 0xFF97942D, 0xFFA8A53E, 0xFFB9B650, 0xFFCDC963, 0xFFE1DD78, 0xFFF8F48F
};

bool GTIA::ioRead(IBus* bus, uint32_t addr, uint8_t* val) {
    (void)bus;
    uint32_t offset = addr & 0x001F;
    if (offset < 0x10) {
        *val = m_collisions[offset] & 0x0F;
        return true;
    }
    switch (offset) {
        case TRIG0: case TRIG1: case TRIG2: case TRIG3: {
            int n = (int)offset - TRIG0;
            // 1 = released; with GRACTL bit 2 a press is latched until the bit is cleared
            *val = (m_trig[n] || m_trigLatched[n]) ? 0x00 : 0x01;
            return true;
        }
        case PAL:
            *val = m_isPal ? 0x01 : 0x0F;   // bits 1-3: 0 PAL, 1 NTSC
            return true;
        case CONSOL:
            *val = m_consoleSwitches & 0x07;
            return true;
        default:
            *val = 0x0F;                    // unused reads: high nibble floats low
            return true;
    }
}

bool GTIA::ioWrite(IBus* bus, uint32_t addr, uint8_t val) {
    (void)bus;
    uint32_t offset = addr & 0x001F;
    m_regs[offset] = val;

    if (offset == HITCLR) {
        std::memset(m_collisions, 0, sizeof(m_collisions));
    } else if (offset == GRACTL && !(val & 0x04)) {
        for (bool& t : m_trigLatched) t = false;
    }
    return true;
}

void GTIA::reset() {
    std::memset(m_regs, 0, sizeof(m_regs));
    std::memset(m_collisions, 0, sizeof(m_collisions));
    m_consoleSwitches = 0x07;
    for (int i = 0; i < 4; ++i) m_trig[i] = m_trigLatched[i] = false;
}

void GTIA::tick(uint64_t cycles) {
    (void)cycles;   // all work happens per scanline in renderLine()
}

namespace {
// Width in colour clocks of one bit of a player or missile, by size code
inline int bitWidth(int code) { return code == 1 ? 2 : code == 3 ? 4 : 1; }
}  // namespace

void GTIA::renderLine(const uint8_t* pf, uint32_t* out) {
    if (m_regs[GRACTL] & 0x04)
        for (int i = 0; i < 4; ++i) if (m_trig[i]) m_trigLatched[i] = true;

    const uint8_t prior = m_regs[PRIOR];
    const bool fifthPlayer = prior & 0x10;       // missiles take PF3's colour
    // Priority: the lowest set bit of PRIOR 0-3 (0 behaves as 1)
    int pmode = (prior & 1) ? 1 : (prior & 2) ? 2 : (prior & 4) ? 4 : (prior & 8) ? 8 : 1;

    // Players' and missiles' coverage per colour clock
    uint8_t pl[LINE_PIXELS / 2] = {}, ms[LINE_PIXELS / 2] = {};
    for (int p = 0; p < 4; ++p) {
        uint8_t data = m_regs[GRAFP0 + p];
        if (!data) continue;
        int w = bitWidth(m_regs[SIZEP0 + p] & 3);
        int x0 = m_regs[HPOSP0 + p] - FIRST_COLOUR_CLOCK;
        for (int b = 0; b < 8; ++b) {
            if (!(data & (0x80 >> b))) continue;
            for (int k = 0; k < w; ++k) {
                int x = x0 + b * w + k;
                if (x >= 0 && x < LINE_PIXELS / 2) pl[x] |= (uint8_t)(1 << p);
            }
        }
    }
    for (int m = 0; m < 4; ++m) {
        uint8_t bits = (m_regs[GRAFM] >> (m * 2)) & 3;
        if (!bits) continue;
        int w = bitWidth((m_regs[SIZEM] >> (m * 2)) & 3);
        int x0 = m_regs[HPOSM0 + m] - FIRST_COLOUR_CLOCK;
        for (int b = 0; b < 2; ++b) {
            if (!(bits & (2 >> b))) continue;
            for (int k = 0; k < w; ++k) {
                int x = x0 + b * w + k;
                if (x >= 0 && x < LINE_PIXELS / 2) ms[x] |= (uint8_t)(1 << m);
            }
        }
    }

    auto colour = [&](uint8_t reg) { return s_palette[reg & 0xFE]; };
    const uint8_t pf1Lum = m_regs[COLPF1] & 0x0E;

    for (int px = 0; px < LINE_PIXELS; ++px) {
        int cc = px >> 1;
        uint8_t raw = pf[px];
        uint8_t cls = raw & 0x0F;
        bool hiresLit = raw & PF_HIRES;
        uint8_t players = pl[cc], missiles = ms[cc];

        // Collisions, once per colour clock (hires lines: lit pixels count as PF2)
        if (!(px & 1) && (players | missiles)) {
            uint8_t pfBit = 0;
            for (int h = 0; h < 2; ++h) {
                uint8_t r = pf[px + h], c = r & 0x0F;
                if (r & PF_HIRES_MODE) { if (r & PF_HIRES) pfBit |= 4; }
                else if (c >= PF_0 && c <= PF_3) pfBit |= (uint8_t)(1 << (c - 1));
            }
            for (int m = 0; m < 4; ++m) if (missiles & (1 << m)) {
                m_collisions[M0PF + m] |= pfBit;
                m_collisions[M0PL + m] |= players;
            }
            for (int p = 0; p < 4; ++p) if (players & (1 << p)) {
                m_collisions[P0PF + p] |= pfBit;
                m_collisions[P0PL + p] |= players & (uint8_t)~(1 << p);
            }
        }

        // Missiles as a fifth player draw as PF3; otherwise in their player's colour
        if (fifthPlayer && missiles) { cls = PF_3; hiresLit = false; missiles = 0; }
        uint8_t objs = players | missiles;

        // Resolve the topmost object
        uint32_t c;
        int topPlayer = -1;
        for (int p = 0; p < 4; ++p) if (objs & (1 << p)) { topPlayer = p; break; }
        bool pfFront;   // the playfield class beats the players
        switch (pmode) {
            case 2:  pfFront = topPlayer >= 2 && cls != PF_BK; break;   // P0-1 > PF > P2-3
            case 4:  pfFront = cls != PF_BK; break;                     // PF > P
            case 8:  pfFront = cls == PF_0 || cls == PF_1; break;       // PF0-1 > P > PF2-3
            default: pfFront = false; break;                            // P > PF
        }
        if (topPlayer >= 0 && !pfFront) {
            uint8_t col = m_regs[COLPM0 + topPlayer];
            if (hiresLit) col = (uint8_t)((col & 0xF0) | pf1Lum);   // hires luminance shows through
            c = colour(col);
        } else {
            switch (cls) {
                case PF_0: c = colour(m_regs[COLPF0]); break;
                case PF_1: c = colour(m_regs[COLPF1]); break;
                case PF_2: c = hiresLit ? colour((uint8_t)((m_regs[COLPF2] & 0xF0) | pf1Lum))
                                        : colour(m_regs[COLPF2]); break;
                case PF_3: c = colour(m_regs[COLPF3]); break;
                default:   c = colour(m_regs[COLBK]); break;
            }
        }
        out[px] = c;
    }
}

uint32_t GTIA::getARGB(uint8_t index) {
    return s_palette[index];
}
