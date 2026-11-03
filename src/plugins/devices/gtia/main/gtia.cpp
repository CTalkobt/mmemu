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

// PAL palette: atari800's PAL model (src/colours_pal.c: the PAL GTIA's
// measured per-hue delays, even and odd lines averaged, GTIA delay 23.2
// degrees), with the same standard preset and conversion as the NTSC table.
const uint32_t GTIA::s_palettePal[256] = {
    0xFF040404, 0xFF0F0F0F, 0xFF1B1B1B, 0xFF272727, 0xFF333333, 0xFF414141, 0xFF4F4F4F, 0xFF5E5E5E,
    0xFF686868, 0xFF787878, 0xFF898989, 0xFF9A9A9A, 0xFFABABAB, 0xFFBFBFBF, 0xFFD3D3D3, 0xFFEAEAEA,
    0xFF2C0100, 0xFF380800, 0xFF431300, 0xFF4F2000, 0xFF5B2C00, 0xFF693A01, 0xFF77480B, 0xFF855619,
    0xFF906124, 0xFF9F7134, 0xFFB08246, 0xFFC29357, 0xFFD3A468, 0xFFE6B87C, 0xFFFACC90, 0xFFFFE3A7,
    0xFF380000, 0xFF430100, 0xFF4F0700, 0xFF5B1305, 0xFF671F10, 0xFF742D1E, 0xFF833C2D, 0xFF914A3B,
    0xFF9B5446, 0xFFAB6456, 0xFFBC7667, 0xFFCD8778, 0xFFDE988A, 0xFFF1AB9D, 0xFFFFC0B1, 0xFFFFD7C8,
    0xFF390000, 0xFF450002, 0xFF50040A, 0xFF5C0F17, 0xFF681B23, 0xFF762931, 0xFF84373F, 0xFF92464E,
    0xFF9D5058, 0xFFAC6068, 0xFFBD7179, 0xFFCE838A, 0xFFE0949C, 0xFFF3A7AF, 0xFFFFBBC3, 0xFFFFD2DA,
    0xFF350024, 0xFF400030, 0xFF4C003B, 0xFF580747, 0xFF641353, 0xFF712161, 0xFF80306F, 0xFF8E3E7E,
    0xFF984988, 0xFFA85998, 0xFFB96AA9, 0xFFCA7BBA, 0xFFDB8DCB, 0xFFEFA0DF, 0xFFFFB4F3, 0xFFFFCBFF,
    0xFF2A0044, 0xFF350050, 0xFF41005B, 0xFF4D0767, 0xFF591373, 0xFF662181, 0xFF752F8F, 0xFF833E9D,
    0xFF8D48A7, 0xFF9D58B7, 0xFFAE6AC8, 0xFFBF7BD9, 0xFFD08CEA, 0xFFE4A0FD, 0xFFF8B4FF, 0xFFFFCBFF,
    0xFF18005A, 0xFF240065, 0xFF2F0270, 0xFF3C0B7C, 0xFF481888, 0xFF552696, 0xFF6434A4, 0xFF7243B2,
    0xFF7C4DBC, 0xFF8C5DCC, 0xFF9D6EDD, 0xFFAE80EE, 0xFFC091FF, 0xFFD3A4FF, 0xFFE7B8FF, 0xFFFECFFF,
    0xFF00015A, 0xFF000965, 0xFF061470, 0xFF12217C, 0xFF1E2D88, 0xFF2C3B96, 0xFF3B49A4, 0xFF4957B2,
    0xFF5462BC, 0xFF6372CC, 0xFF7583DD, 0xFF8694EE, 0xFF97A5FF, 0xFFABB9FF, 0xFFBFCDFF, 0xFFD6E4FF,
    0xFF000A44, 0xFF001650, 0xFF00225B, 0xFF032E67, 0xFF0D3A73, 0xFF1B4881, 0xFF29568F, 0xFF38649D,
    0xFF426FA7, 0xFF527FB7, 0xFF6490C8, 0xFF75A1D9, 0xFF86B2EA, 0xFF9AC5FD, 0xFFAED9FF, 0xFFC5F0FF,
    0xFF001624, 0xFF002230, 0xFF002D3B, 0xFF003A47, 0xFF034653, 0xFF0F5361, 0xFF1E626F, 0xFF2D707E,
    0xFF377A88, 0xFF478A98, 0xFF599BA9, 0xFF6AADBA, 0xFF7BBECB, 0xFF8FD1DF, 0xFFA3E5F3, 0xFFBAFCFF,
    0xFF002200, 0xFF002E02, 0xFF00390A, 0xFF004517, 0xFF015123, 0xFF0B5F31, 0xFF1A6D3F, 0xFF287C4E,
    0xFF338658, 0xFF439668, 0xFF54A779, 0xFF66B88A, 0xFF77C99C, 0xFF8ADCAF, 0xFF9FF0C3, 0xFFB6FFDA,
    0xFF002600, 0xFF003100, 0xFF003C00, 0xFF024900, 0xFF0A5500, 0xFF196201, 0xFF27710B, 0xFF367F19,
    0xFF408924, 0xFF509934, 0xFF61AA46, 0xFF73BB57, 0xFF84CC68, 0xFF97E07C, 0xFFACF490, 0xFFC3FFA7,
    0xFF002100, 0xFF002C00, 0xFF053800, 0xFF114400, 0xFF1D5000, 0xFF2B5D00, 0xFF396C00, 0xFF487A04,
    0xFF52840D, 0xFF62941D, 0xFF74A52F, 0xFF85B640, 0xFF96C852, 0xFFA9DB65, 0xFFBEEF7A, 0xFFD5FF91,
    0xFF041700, 0xFF0F2200, 0xFF1B2E00, 0xFF273A00, 0xFF334600, 0xFF415400, 0xFF4F6200, 0xFF5E7100,
    0xFF687B06, 0xFF788B15, 0xFF899C27, 0xFF9AAD38, 0xFFABBE4A, 0xFFBFD25E, 0xFFD3E672, 0xFFEAFC89,
    0xFF1A0A00, 0xFF251600, 0xFF312100, 0xFF3D2D00, 0xFF493A00, 0xFF574700, 0xFF655600, 0xFF736404,
    0xFF7E6E0D, 0xFF8D7E1D, 0xFF9F8F2F, 0xFFB0A140, 0xFFC1B252, 0xFFD4C565, 0xFFE8D97A, 0xFFFFF091,
    0xFF2C0100, 0xFF380800, 0xFF431300, 0xFF4F2000, 0xFF5B2C00, 0xFF693A01, 0xFF77480B, 0xFF855619,
    0xFF906124, 0xFF9F7134, 0xFFB08246, 0xFFC29357, 0xFFD3A468, 0xFFE6B87C, 0xFFFACC90, 0xFFFFE3A7
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

    const uint32_t* palette = m_isPal ? s_palettePal : s_palette;
    const uint8_t prior = m_regs[PRIOR];
    const bool fifthPlayer = prior & 0x10;       // missiles take PF3's colour
    const bool multicolour = prior & 0x20;       // overlapping P0+P1, P2+P3 OR their colours
    const int gtiaMode = prior >> 6;             // 1: GRAPHICS 9, 2: 10, 3: 11
    // Priority: the lowest set bit of PRIOR 0-3 (0 behaves as 1)
    int pmode = (prior & 1) ? 1 : (prior & 2) ? 2 : (prior & 4) ? 4 : (prior & 8) ? 8 : 1;

    // Players' and missiles' coverage per colour clock
    constexpr int CLOCKS = LINE_PIXELS / 2;
    uint8_t pl[CLOCKS] = {}, ms[CLOCKS] = {};
    for (int p = 0; p < 4; ++p) {
        uint8_t data = m_regs[GRAFP0 + p];
        if (!data) continue;
        int w = bitWidth(m_regs[SIZEP0 + p] & 3);
        int x0 = m_regs[HPOSP0 + p] - FIRST_COLOUR_CLOCK;
        for (int b = 0; b < 8; ++b) {
            if (!(data & (0x80 >> b))) continue;
            for (int k = 0; k < w; ++k) {
                int x = x0 + b * w + k;
                if (x >= 0 && x < CLOCKS) pl[x] |= (uint8_t)(1 << p);
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
                if (x >= 0 && x < CLOCKS) ms[x] |= (uint8_t)(1 << m);
            }
        }
    }

    // GTIA modes: ANTIC's two bits a colour clock, paired into a 4-bit pixel
    // two colour clocks wide; mode 10's pairs start a colour clock later
    // (after atari800's draw_an_gtia9/10/11)
    uint8_t nibble[CLOCKS] = {};
    if (gtiaMode) {
        uint8_t bits[CLOCKS];
        for (int cc = 0; cc < CLOCKS; ++cc) {
            uint8_t a = pf[cc * 2], b = pf[cc * 2 + 1];
            if (a & PF_HIRES_MODE) {
                bits[cc] = (uint8_t)(((a & PF_HIRES) ? 2 : 0) | ((b & PF_HIRES) ? 1 : 0));
            } else {
                uint8_t c = a & 0x0F;
                bits[cc] = c == PF_BK ? 0 : c == PF_0 ? 1 : c == PF_1 ? 2 : 3;
            }
        }
        for (int cc = 0; cc < CLOCKS; ++cc) {
            int first = gtiaMode == 2 ? ((cc & 1) ? cc - 1 : cc - 2) : (cc & ~1);
            nibble[cc] = (first >= 0 && first + 1 < CLOCKS)
                       ? (uint8_t)((bits[first] << 2) | bits[first + 1]) : 0;
        }
    }

    auto colour = [&](uint8_t reg) { return palette[reg & 0xFE]; };
    const uint8_t pf1Lum = m_regs[COLPF1] & 0x0E;
    const uint8_t colbk = m_regs[COLBK] & 0xFE;
    static const uint8_t GTIA10_CLASS[16] = {
        PF_BK, PF_BK, PF_BK, PF_BK, PF_0, PF_1, PF_2, PF_3,
        PF_BK, PF_BK, PF_BK, PF_BK, PF_0, PF_1, PF_2, PF_3 };

    for (int px = 0; px < LINE_PIXELS; ++px) {
        int cc = px >> 1;
        uint8_t raw = pf[px];
        uint8_t cls = raw & 0x0F;
        bool hiresLit = raw & PF_HIRES;
        uint8_t players = pl[cc], missiles = ms[cc];
        uint8_t pseudo = 0;   // GTIA 10: pixel values 0-3 are drawn as players 0-3
        const bool inWindow = raw & PF_WINDOW;
        const uint8_t nib = nibble[cc];

        if (gtiaMode == 2) {
            cls = inWindow ? GTIA10_CLASS[nib] : (uint8_t)PF_BK;
            if (inWindow && nib < 4) pseudo = (uint8_t)(1 << nib);
            hiresLit = false;
        } else if (gtiaMode) {
            cls = PF_BK;      // modes 9 and 11 have no playfield colours (or collisions)
            hiresLit = false;
        }

        // Collisions, once per colour clock (hires lines: lit pixels count as PF2)
        if (!(px & 1) && (players | missiles)) {
            uint8_t pfBit = 0;
            if (gtiaMode == 2) {
                if (cls >= PF_0 && cls <= PF_3) pfBit = (uint8_t)(1 << (cls - 1));
            } else if (!gtiaMode) {
                for (int h = 0; h < 2; ++h) {
                    uint8_t r = pf[px + h], c = r & 0x0F;
                    if (r & PF_HIRES_MODE) { if (r & PF_HIRES) pfBit |= 4; }
                    else if (c >= PF_0 && c <= PF_3) pfBit |= (uint8_t)(1 << (c - 1));
                }
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
        bool fifth = fifthPlayer && missiles;
        if (fifth) { cls = PF_3; hiresLit = false; missiles = 0; }
        uint8_t objs = players | missiles | pseudo;

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
            if (multicolour && (topPlayer == 0 || topPlayer == 2) && (objs & (2 << topPlayer)))
                col |= m_regs[COLPM0 + topPlayer + 1];
            if (hiresLit) col = (uint8_t)((col & 0xF0) | pf1Lum);   // hires luminance shows through
            c = colour(col);
        } else if (gtiaMode == 1 || gtiaMode == 3) {
            // 9: 16 luminances of COLBK's hue; 11: 16 hues at COLBK's luminance
            // (0 is COLBK's hue at luminance 0). The fifth player tints with PF3.
            uint8_t base = fifth ? (uint8_t)(m_regs[COLPF3] & 0xFE) : colbk;
            uint8_t n = inWindow ? nib : 0;
            uint8_t idx = gtiaMode == 1 ? (uint8_t)(base | n)
                                        : n ? (uint8_t)(base | (n << 4)) : (uint8_t)(base & 0xF0);
            c = palette[idx];
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

uint32_t GTIA::getARGB(uint8_t index, bool pal) {
    return pal ? s_palettePal[index] : s_palette[index];
}
