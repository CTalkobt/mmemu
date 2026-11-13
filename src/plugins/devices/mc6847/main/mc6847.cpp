// MC6847 / MC6847T1 VDG. Timing and per-byte behaviour after the data sheet
// and XRoar (mc6847.c, GPL-3.0-or-later), which documents what the chip does
// mid-line; this is mmsim's own implementation.

#include "mc6847.h"
#include <algorithm>
#include <cstring>

namespace {

// Lines per row of the VDG's own address counter, by GM (graphics modes)
const unsigned GM_NLPR[8] = {3, 3, 3, 2, 2, 1, 1, 1};

const uint32_t PALETTE[Mc6847::NUM_COLOURS] = {
    0xFF0AFF0A,   // green
    0xFFFFFF43,   // yellow
    0xFF2216FF,   // blue
    0xFFB60521,   // red
    0xFFFFFFFF,   // buff
    0xFF0AD474,   // cyan
    0xFFFF1CFF,   // magenta
    0xFFFF4305,   // orange
    0xFF090909,   // black
    0xFF002C00,   // dark green (the background of inverse text)
    0xFF3C1000,   // dark orange
    0xFFFFB46A,   // bright orange (text with CSS)
    0xFF0080FF,   // artifact blue
    0xFFFF8000,   // artifact red (orange)
};

template <class T> void put(std::vector<uint8_t>& v, const T& x) {
    const auto* p = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), p, p + sizeof(T));
}
template <class T> void get(const uint8_t*& p, const uint8_t* end, T& x) {
    if (p + sizeof(T) > end) return;
    std::memcpy(&x, p, sizeof(T));
    p += sizeof(T);
}

}  // namespace

Mc6847::Mc6847(bool t1) : m_t1(t1) {
    m_back.assign(OUT_W * OUT_H, PALETTE[BLACK]);
    m_front = m_back;
    m_backIdx.assign(256 * 192, BLACK);
    m_frontIdx = m_backIdx;
    reset();
}

uint32_t Mc6847::rgb(uint8_t colour) { return colour < NUM_COLOURS ? PALETTE[colour] : PALETTE[BLACK]; }

uint8_t Mc6847::activeDot(int x, int y) const {
    if (x < 0 || x >= 256 || y < 0 || y >= 192) return BLACK;
    return m_frontIdx[(size_t)y * 256 + (size_t)x];
}

void Mc6847::reset() {
    m_line = 0;
    m_tick = 0;
    m_row = 0;
    m_addr = 0;
    m_hs = true;
    m_fs = true;
    std::memset(m_dots, BLACK, sizeof(m_dots));
    m_ag = true;                // so setMode(0) does the switch to alphanumerics
    setMode(0);
    m_fetched = m_index = m_bit = 0;
    m_bytesLeft = is32() ? 32 : 16;
    m_pos = LEFT_BORDER;
}

uint8_t Mc6847::borderColour() const {
    if (m_ag) return m_css ? BUFF : GREEN;
    // The T1's text border (GM2 without GM1): green or orange
    const bool textBorder = m_t1 && !(m_gm & 2) && (m_gm & 4);
    if (textBorder) return m_css ? (m_t1 ? ORANGE : BRIGHT_ORANGE) : GREEN;
    return BLACK;
}

void Mc6847::setMode(uint8_t mode) {
    if (m_line >= ACTIVE_FIRST && m_line < ACTIVE_END) render(m_tick);
    m_modePins = mode;
    m_gm = (mode >> 4) & 7;
    m_css = mode & 0x08;
    const bool ag = mode & 0x80;
    m_nlpr = ag ? GM_NLPR[m_gm] : 12;
    const uint8_t bright = m_t1 ? ORANGE : BRIGHT_ORANGE;
    if (!ag) {
        if (m_ag) {
            // Graphics to alphanumerics: the row counter is preset, and the
            // rest of this byte shows as bitmap
            m_row = 0;
            m_rmode = R_RG;
            if (m_as) {
                m_g = 0x3F;
                m_fg = GREEN;
                m_bg = DARK_GREEN;
            } else {
                m_fg = m_cssLatched ? bright : (uint8_t)GREEN;
                m_bg = m_cssLatched ? DARK_ORANGE : DARK_GREEN;
            }
        }
    } else {
        if (!m_ag) {
            m_fg = m_cssLatched ? BUFF : GREEN;
            m_bg = m_cssLatched ? BLACK : DARK_GREEN;
        }
        m_rmode = (m_gm & 1) ? R_RG : R_CG;
    }
    m_ag = ag;
    m_border = borderColour();
}

void Mc6847::advance(uint64_t ticks) {
    while (ticks) {
        const int next = m_tick < HS_RISE ? HS_RISE : LINE_TICKS;
        const uint64_t step = std::min<uint64_t>(ticks, (uint64_t)(next - m_tick));
        m_tick += (int)step;
        ticks -= step;
        if (m_tick == HS_RISE && !m_hs) {
            m_hs = true;
            if (onHs) onHs(true);
        }
        if (m_tick >= LINE_TICKS) endLine();
    }
}

void Mc6847::endLine() {
    const bool active = m_line >= ACTIVE_FIRST && m_line < ACTIVE_END;
    if (active) {
        render(LINE_TICKS);
        if (++m_row > 11) m_row = 0;
        if (m_row % m_nlpr == 0) m_addr = (uint16_t)(m_addr + (is32() ? 32 : 16));
    }
    commitLine();

    m_tick = 0;
    m_line = (m_line + 1) % FRAME_LINES;
    m_hs = false;
    if (onHs) onHs(false);

    m_fetched = m_index = m_bit = 0;
    m_bytesLeft = is32() ? 32 : 16;
    m_pos = LEFT_BORDER;
    m_artifactLine = (m_gm & 1) && m_css;
    if (m_line == ACTIVE_FIRST) m_row = 0;
    if (m_line == ACTIVE_END) {
        m_fs = false;
        if (onFs) onFs(false);
        m_addr = 0;
    }
    if (m_line == 0) {
        std::swap(m_front, m_back);
        std::swap(m_frontIdx, m_backIdx);
        ++m_frames;
        m_fs = true;
        if (onFs) onFs(true);
        if (onFrame) onFrame();
    }
}

void Mc6847::byteBoundary() {
    if (m_index >= m_fetched && m_index < 42) {
        if (fetch) fetch((uint16_t)(m_addr + m_fetched), m_index + 1 - m_fetched, m_vram + m_fetched);
        m_fetched = m_index + 1;
    }
    const uint16_t v = m_index < 42 ? m_vram[m_index] : 0;
    ++m_index;
    const uint8_t raw = v & 0xFF;
    m_g = raw;
    m_bit = 8;
    m_as = m_t1 ? (raw & 0x80) : (v & F_AS);
    m_ext = v & F_EXT;
    m_cssLatched = m_css;
    m_cgBase = m_cssLatched ? BUFF : GREEN;
    const uint8_t bright = m_t1 ? ORANGE : BRIGHT_ORANGE;

    if (!m_ag && !m_as) {
        // Alphanumerics: a row of the character, maybe inverted
        bool inv;
        if (m_t1) {
            inv = m_ext || (raw & 0x40);
            inv ^= (bool)(m_gm & 2);                       // the T1's inverse video (GM1)
            const uint8_t c = m_ext ? raw : (uint8_t)(raw | 0x40);
            m_g = mc6847_font::T1[(c & 0x7F) * 12 + m_row];
        } else {
            inv = v & F_INV;
            if (!m_ext) m_g = mc6847_font::STANDARD[(raw & 0x3F) * 12 + m_row];   // external: the byte itself
        }
        if (inv ^ m_invertText) m_g = (uint8_t)~m_g;
    }
    if (!m_ag && m_as) {
        // Semigraphics: 4 (2x2 blocks, colour in bits 4-6) or, with
        // INT/EXT, 6 (2x3 blocks, colour CSS and bits 6-7)
        uint8_t s = raw;
        if (m_t1 || !m_ext) {
            if (m_row < 6) s >>= 2;
            m_sfg = (raw >> 4) & 7;
        } else {
            if (m_row < 4) s >>= 4;
            else if (m_row < 8) s >>= 2;
            m_sfg = (uint8_t)(m_cgBase + ((raw >> 6) & 3));
        }
        m_sbg = BLACK;
        m_sg = (uint8_t)(((s & 2) ? 0xF0 : 0) | ((s & 1) ? 0x0F : 0));
    }
    if (!m_ag) {
        m_rmode = m_as ? R_SG : R_RG;
        m_fg = m_cssLatched ? bright : (uint8_t)GREEN;
        m_bg = m_cssLatched ? DARK_ORANGE : DARK_GREEN;
    } else {
        m_rmode = (m_gm & 1) ? R_RG : R_CG;
        m_fg = m_cssLatched ? BUFF : GREEN;
        m_bg = m_cssLatched ? BLACK : DARK_GREEN;
    }
}

// Draw the active line up to tick `to`, fetching the bytes the beam has reached
void Mc6847::render(int to) {
    to = std::min(to, LINE_TICKS);
    const bool wide = is32();
    if (fetch) {
        const int shift = wide ? 4 : 5, cap = wide ? 42 : 22;
        if (to >= HBLANK + (1 << shift)) {
            const int n = std::min(cap, (to - HBLANK) >> shift);
            if (n > m_fetched) {
                fetch((uint16_t)(m_addr + m_fetched), n - m_fetched, m_vram + m_fetched);
                m_fetched = n;
            }
        }
    }
    if (to <= m_pos) return;

    // Left border
    while (m_pos < ACTIVE_START) {
        m_dots[m_pos] = m_dots[m_pos + 1] = m_border;
        m_pos += 2;
        if (m_pos >= to) return;
    }
    // Active area: two bits of data at a time
    while (m_bytesLeft > 0) {
        if (m_bit == 0) byteBoundary();
        uint8_t c0, c1;
        switch (m_rmode) {
            case R_SG:
                c0 = (m_sg & 0x80) ? m_sfg : m_sbg;
                c1 = (m_sg & 0x40) ? m_sfg : m_sbg;
                break;
            case R_CG:
                c0 = c1 = (uint8_t)(m_cgBase + ((m_g & 0xC0) >> 6));
                break;
            default:
                c0 = (m_g & 0x80) ? m_fg : m_bg;
                c1 = (m_g & 0x40) ? m_fg : m_bg;
                break;
        }
        const int w = is32() ? 2 : 4;
        for (int i = 0; i < w && m_pos < LINE_TICKS + 8; ++i) m_dots[m_pos++] = c0;
        for (int i = 0; i < w && m_pos < LINE_TICKS + 8; ++i) m_dots[m_pos++] = c1;
        m_bit -= 2;
        if (m_bit == 0) --m_bytesLeft;
        m_g = (uint8_t)(m_g << 2);
        m_sg = (uint8_t)(m_sg << 2);
        if (m_pos >= to) return;
    }
    // Right border, then black to its end (a 32 to 16-byte switch mid-line
    // leaves the line short)
    const int rbEnd = std::min(RIGHT_BORDER_END, m_pos + (RIGHT_BORDER_END - RIGHT_BORDER));
    while (m_pos < rbEnd) {
        m_border = borderColour();
        m_dots[m_pos] = m_dots[m_pos + 1] = m_border;
        m_pos += 2;
        if (m_pos >= to) return;
    }
    while (m_pos < RIGHT_BORDER_END) {
        m_dots[m_pos] = m_dots[m_pos + 1] = BLACK;
        m_pos += 2;
    }
}

void Mc6847::commitLine() {
    if (m_line < OUT_TOP || m_line >= OUT_TOP + OUT_H) return;
    uint32_t* out = &m_back[(size_t)(m_line - OUT_TOP) * OUT_W];
    const bool active = m_line >= ACTIVE_FIRST && m_line < ACTIVE_END;
    if (!active) {
        const uint32_t c = rgb(m_border);
        std::fill(out, out + OUT_W, c);
        return;
    }
    uint8_t idx[OUT_W];
    for (int x = 0; x < OUT_W; ++x) idx[x] = m_dots[OUT_LEFT + 2 * x];
    if (m_artifact != Artifact::Off && m_artifactLine) {
        // NTSC cross-colour: each aligned pair of black/buff dots of the
        // active area becomes black, white or one of the artifact colours
        const uint8_t lit01 = m_artifact == Artifact::BlueRed ? ARTIFACT_BLUE : ARTIFACT_RED;
        const uint8_t lit10 = m_artifact == Artifact::BlueRed ? ARTIFACT_RED : ARTIFACT_BLUE;
        for (int x = 32; x < 32 + 256; x += 2) {
            const uint8_t a = idx[x], b = idx[x + 1];
            if ((a != BLACK && a != BUFF) || (b != BLACK && b != BUFF)) continue;
            const int k = (a == BUFF ? 2 : 0) | (b == BUFF ? 1 : 0);
            const uint8_t c = k == 0 ? (uint8_t)BLACK : k == 3 ? (uint8_t)BUFF : k == 1 ? lit01 : lit10;
            idx[x] = idx[x + 1] = c;
        }
    }
    for (int x = 0; x < OUT_W; ++x) out[x] = rgb(idx[x]);
    std::memcpy(&m_backIdx[(size_t)(m_line - ACTIVE_FIRST) * 256], idx + 32, 256);
}

std::vector<uint8_t> Mc6847::saveState() const {
    std::vector<uint8_t> v;
    put(v, m_modePins); put(v, m_gm); put(v, m_ag); put(v, m_css);
    put(v, m_line); put(v, m_tick); put(v, m_hs); put(v, m_fs); put(v, m_row); put(v, m_nlpr);
    put(v, m_addr); put(v, m_vram); put(v, m_fetched); put(v, m_index); put(v, m_bytesLeft);
    put(v, m_bit); put(v, m_pos); put(v, m_g); put(v, m_sg); put(v, m_as); put(v, m_ext);
    put(v, m_fg); put(v, m_bg); put(v, m_sfg); put(v, m_sbg); put(v, m_cgBase); put(v, m_rmode);
    put(v, m_cssLatched); put(v, m_border); put(v, m_artifactLine); put(v, m_dots);
    return v;
}

void Mc6847::loadState(const std::vector<uint8_t>& s) {
    const uint8_t* p = s.data();
    const uint8_t* e = p + s.size();
    get(p, e, m_modePins); get(p, e, m_gm); get(p, e, m_ag); get(p, e, m_css);
    get(p, e, m_line); get(p, e, m_tick); get(p, e, m_hs); get(p, e, m_fs); get(p, e, m_row); get(p, e, m_nlpr);
    get(p, e, m_addr); get(p, e, m_vram); get(p, e, m_fetched); get(p, e, m_index); get(p, e, m_bytesLeft);
    get(p, e, m_bit); get(p, e, m_pos); get(p, e, m_g); get(p, e, m_sg); get(p, e, m_as); get(p, e, m_ext);
    get(p, e, m_fg); get(p, e, m_bg); get(p, e, m_sfg); get(p, e, m_sbg); get(p, e, m_cgBase); get(p, e, m_rmode);
    get(p, e, m_cssLatched); get(p, e, m_border); get(p, e, m_artifactLine); get(p, e, m_dots);
}
