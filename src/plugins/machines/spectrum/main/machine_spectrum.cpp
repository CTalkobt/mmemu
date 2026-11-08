// Sinclair ZX Spectrum 48K / 128K / +2, after Fuse (GPL-2.0-or-later):
// machines/spec48.c, spec128.c, machine.c and spectrum.c (the contention
// pattern and the floating bus), peripherals/ula.c (port $FE and the port
// contention), display*.c (the beam position: the border and screen as the
// beam passes), keyboard.c, peripherals/joystick.c (Kempston). Frame
// timings from libspectrum timings.c.

#include "machine_spectrum.h"
#include "spectrum_tape.h"
#include "libcore/main/image_loader.h"
#include "libdevices/main/disk_file.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/io_handler.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdebug/main/execution_observer.h"
#include "libmem/main/ibus.h"
#include "plugins/z80/main/cpu_z80.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <vector>

namespace spectrum {

namespace {

constexpr int SAMPLE_RATE = 44100;

bool loadRom(const std::string& path, std::vector<uint8_t>& out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (f) out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (!f || out.size() != 0x4000) {
        if (err) *err = "missing or wrong-size ROM " + path;
        return false;
    }
    return true;
}

// libspectrum timings.c: Ferranti 5C/6C (48K), 7C (128K, +2)
struct Timings {
    uint32_t cpuHz;
    int tstatesPerLine, lines, interruptLength, topLeftPixel;
    int frame() const { return tstatesPerLine * lines; }
};
constexpr Timings T48 = {3500000, 224, 312, 32, 14336};
constexpr Timings T128 = {3546900, 228, 311, 36, 14362};

// Fuse's display: 32 pixels (4 columns of 8) of left and right border, 24
// lines of top and bottom border around the 256 x 192 screen
constexpr int BORDER_COLS = 4, BORDER_LINES = 24, LEFT_BORDER_T = 24, SCREEN_T = 128;
constexpr int COLS = 40, ROWS = 240;

// ---------------------------------------------------------------------------
// The keyboard: 8 half-rows (A8-A15 low selects), 5 keys each on D0-D4.
// A key is row << 3 | bit.
// ---------------------------------------------------------------------------
constexpr int K_CAPS = 0 << 3 | 0, K_SYM = 7 << 3 | 1, K_ENTER = 6 << 3 | 0, K_SPACE = 7 << 3 | 0;

int keyFor(char c) {
    static const char* const rows[8] = {"\x01ZXCV", "ASDFG", "QWERT", "12345", "09876", "POIUY", "\x02LKJH", "\x03\x04MNB"};
    c = (char)std::toupper((unsigned char)c);
    for (int r = 0; r < 8; ++r)
        for (int b = 0; b < 5; ++b)
            if (rows[r][b] == c) return r << 3 | b;
    return -1;
}

// BASIC keywords and their token codes (48K: $A5-$FF), for typed text
struct Keyword { const char* text; uint8_t code; };
const Keyword KEYWORDS[] = {
    {"RND", 0xA5}, {"INKEY$", 0xA6}, {"PI", 0xA7}, {"FN", 0xA8}, {"POINT", 0xA9}, {"SCREEN$", 0xAA},
    {"ATTR", 0xAB}, {"AT", 0xAC}, {"TAB", 0xAD}, {"VAL$", 0xAE}, {"CODE", 0xAF}, {"VAL", 0xB0},
    {"LEN", 0xB1}, {"SIN", 0xB2}, {"COS", 0xB3}, {"TAN", 0xB4}, {"ASN", 0xB5}, {"ACS", 0xB6},
    {"ATN", 0xB7}, {"LN", 0xB8}, {"EXP", 0xB9}, {"INT", 0xBA}, {"SQR", 0xBB}, {"SGN", 0xBC},
    {"ABS", 0xBD}, {"PEEK", 0xBE}, {"IN", 0xBF}, {"USR", 0xC0}, {"STR$", 0xC1}, {"CHR$", 0xC2},
    {"NOT", 0xC3}, {"BIN", 0xC4}, {"OR", 0xC5}, {"AND", 0xC6}, {"<=", 0xC7}, {">=", 0xC8},
    {"<>", 0xC9}, {"LINE", 0xCA}, {"THEN", 0xCB}, {"TO", 0xCC}, {"STEP", 0xCD}, {"DEF FN", 0xCE},
    {"CAT", 0xCF}, {"FORMAT", 0xD0}, {"MOVE", 0xD1}, {"ERASE", 0xD2}, {"OPEN #", 0xD3},
    {"CLOSE #", 0xD4}, {"MERGE", 0xD5}, {"VERIFY", 0xD6}, {"BEEP", 0xD7}, {"CIRCLE", 0xD8},
    {"INK", 0xD9}, {"PAPER", 0xDA}, {"FLASH", 0xDB}, {"BRIGHT", 0xDC}, {"INVERSE", 0xDD},
    {"OVER", 0xDE}, {"OUT", 0xDF}, {"LPRINT", 0xE0}, {"LLIST", 0xE1}, {"STOP", 0xE2}, {"READ", 0xE3},
    {"DATA", 0xE4}, {"RESTORE", 0xE5}, {"NEW", 0xE6}, {"BORDER", 0xE7}, {"CONTINUE", 0xE8},
    {"DIM", 0xE9}, {"REM", 0xEA}, {"FOR", 0xEB}, {"GO TO", 0xEC}, {"GOTO", 0xEC}, {"GO SUB", 0xED},
    {"GOSUB", 0xED}, {"INPUT", 0xEE}, {"LOAD", 0xEF}, {"LIST", 0xF0}, {"LET", 0xF1}, {"PAUSE", 0xF2},
    {"NEXT", 0xF3}, {"POKE", 0xF4}, {"PRINT", 0xF5}, {"PLOT", 0xF6}, {"RUN", 0xF7}, {"SAVE", 0xF8},
    {"RANDOMIZE", 0xF9}, {"IF", 0xFA}, {"CLS", 0xFB}, {"DRAW", 0xFC}, {"CLEAR", 0xFD},
    {"RETURN", 0xFE}, {"COPY", 0xFF},
};

/**
 * Text for 48K BASIC as the ROM's key codes: keywords (outside strings and
 * REM, at word boundaries, longest first) become their tokens, as K mode
 * would enter them, and the spaces around them go (the ROM lists its own).
 */
std::vector<uint8_t> tokenise(const std::string& text) {
    std::vector<uint8_t> out;
    bool quote = false, rem = false;
    auto isWord = [](char c) { return std::isalnum((unsigned char)c) || c == '$'; };
    for (size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '\n' || c == '\r') { out.push_back(0x0D); quote = rem = false; ++i; continue; }
        if (!quote && !rem) {
            const Keyword* best = nullptr;
            size_t bestLen = 0;
            for (const Keyword& k : KEYWORDS) {
                const size_t n = std::strlen(k.text);
                if (n <= bestLen || i + n > text.size()) continue;
                bool same = true;
                for (size_t j = 0; j < n && same; ++j)
                    same = std::toupper((unsigned char)text[i + j]) == k.text[j];
                if (!same) continue;
                const bool alpha = std::isalpha((unsigned char)k.text[0]);
                if (alpha && i > 0 && isWord(text[i - 1])) continue;          // inside a word
                if (alpha && k.text[n - 1] != '$' && k.text[n - 1] != '#' && i + n < text.size() &&
                    isWord(text[i + n]))
                    continue;
                best = &k;
                bestLen = n;
            }
            if (best) {
                while (!out.empty() && out.back() == ' ') out.pop_back();
                out.push_back(best->code);
                i += bestLen;
                while (i < text.size() && text[i] == ' ' && best->code != 0xEA) ++i;
                if (best->code == 0xEA) rem = true;
                continue;
            }
        }
        if (c == '"') quote = !quote;
        out.push_back((uint8_t)c);
        ++i;
    }
    return out;
}

class SpecKeyboard : public IOHandler, public IKeyboardMatrix {
public:
    // Typed text goes to the ROM's key buffer when it reads a key: LAST_K
    // (23560) and FLAGS (23611) bit 5. Each ROM's key-read point is found
    // by its code (see keyReadPoint); 48 BASIC takes keywords as tokens.
    std::function<uint8_t(uint16_t)> peek;
    std::function<void(uint16_t, uint8_t)> poke;

    const char* name() const override { return "Keyboard"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void tick(uint64_t) override {}
    void reset() override { std::memset(m_rows, 0x1F, sizeof m_rows); m_queue.clear(); m_text.clear(); }

    void set(int key, bool down) {
        if (key < 0) return;
        if (down) m_rows[key >> 3] &= (uint8_t)~(1 << (key & 7));
        else m_rows[key >> 3] |= (uint8_t)(1 << (key & 7));
    }

    /** Port $FE's high byte: the keys of every half-row whose line is low (bits 0-4). */
    uint8_t read(uint8_t high) const {
        uint8_t v = 0x1F;
        for (int r = 0; r < 8; ++r)
            if (!(high & (1 << r))) v &= m_rows[r];
        return v;
    }

    bool typing() const { return !m_queue.empty() || !m_text.empty(); }

    /** The ROM is about to read a key: give it the next typed one. `tokens`:
     *  48 BASIC (keywords as tokens) rather than the 128 editor. */
    void atKeyRead(bool tokens) {
        if (!typing() || !peek || !poke) return;
        if (m_queue.empty()) {
            if (tokens) { for (uint8_t c : tokenise(m_text)) m_queue.push_back(c); }
            else for (char c : m_text) m_queue.push_back(c == '\n' ? 0x0D : (uint8_t)c);
            m_text.clear();
        }
        const uint8_t flags = peek(23611);
        if (flags & 0x20) return;               // the last one not taken yet
        poke(23560, m_queue.front());
        poke(23611, (uint8_t)(flags | 0x20));
        m_queue.pop_front();
    }

    bool key(std::string n, bool d) {
        for (auto& c : n) c = (char)std::tolower((unsigned char)c);
        // Host keys that are Caps Shift with a digit on the Spectrum
        static const std::map<std::string, char> caps = {
            {"backspace", '0'}, {"delete", '0'}, {"left", '5'}, {"down", '6'}, {"up", '7'}, {"right", '8'},
            {"caps_lock", '2'}, {"tab", '1'}};
        if (auto it = caps.find(n); it != caps.end()) { set(K_CAPS, d); set(keyFor(it->second), d); return true; }
        if (n == "escape" || n == "esc" || n == "break" || n == "run_stop") { set(K_CAPS, d); set(K_SPACE, d); return true; }
        // Host punctuation that is Symbol Shift with a key on the Spectrum
        static const std::map<std::string, char> sym = {
            {"comma", 'N'}, {"period", 'M'}, {"semicolon", 'O'}, {"minus", 'J'}, {"slash", 'V'},
            {"kpeq", 'L'}, {"equals", 'L'}, {"plus", 'K'}, {"asterisk", 'B'}, {"squote", '7'}};
        if (auto it = sym.find(n); it != sym.end()) { set(K_SYM, d); set(keyFor(it->second), d); return true; }
        static const std::map<std::string, int> keys = {
            {"return", K_ENTER}, {"enter", K_ENTER}, {"space", K_SPACE},
            {"shift", K_CAPS}, {"left_shift", K_CAPS}, {"caps_shift", K_CAPS},
            {"right_shift", K_SYM}, {"control", K_SYM}, {"ctrl", K_SYM}, {"alt", K_SYM}, {"symbol_shift", K_SYM},
        };
        if (auto it = keys.find(n); it != keys.end()) { set(it->second, d); return true; }
        if (n.size() == 1 && std::isalnum((unsigned char)n[0])) { set(keyFor(n[0]), d); return true; }
        return false;
    }

    // IKeyboardMatrix: row = half-row (0-7), col = bit (0-4)
    void keyDown(int row, int col) override { set(row << 3 | col, true); }
    void keyUp(int row, int col) override { set(row << 3 | col, false); }
    void clearKeys() override { std::memset(m_rows, 0x1F, sizeof m_rows); }
    bool pressKeyByName(const std::string& n, bool d) override { return key(n, d); }
    IPortDevice* getPort(int) override { return nullptr; }
    void enqueueText(const std::string& text) override {
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n') { m_text += '\n'; ++i; }
            else m_text += text[i];
        }
    }

private:
    uint8_t m_rows[8] = {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F};
    std::deque<uint8_t> m_queue;                // codes for the ROM
    std::string m_text;                         // typed, not yet converted
};

/**
 * Where a ROM reads a typed key: 48 BASIC's KEY-INPUT tests FLAGS bit 5
 * with BIT 5,(IY+1) / RET Z / LD A,(LAST_K) / RES 5,(IY+1); the 128
 * editor waits with LD HL,FLAGS / BIT 5,(HL) / JR Z,-4. The address of
 * the BIT, or -1.
 */
int keyReadPoint(const std::vector<uint8_t>& rom, bool& tokens) {
    static const uint8_t K48[] = {0xFD, 0xCB, 0x01, 0x6E, 0xC8, 0x3A, 0x08, 0x5C, 0xFD, 0xCB, 0x01, 0xAE};
    static const uint8_t K128[] = {0x21, 0x3B, 0x5C, 0xCB, 0x6E, 0x28, 0xFC, 0xCB, 0xAE, 0x3A, 0x08, 0x5C};
    auto at = std::search(rom.begin(), rom.end(), std::begin(K48), std::end(K48));
    if (at != rom.end()) { tokens = true; return (int)(at - rom.begin()); }
    at = std::search(rom.begin(), rom.end(), std::begin(K128), std::end(K128));
    if (at != rom.end()) { tokens = false; return (int)(at - rom.begin()) + 3; }
    return -1;
}

// ---------------------------------------------------------------------------
// Memory: 16 KB ROM pages and RAM banks (the 48K uses banks 5, 2 and 0)
// ---------------------------------------------------------------------------
struct Memory {
    std::vector<std::vector<uint8_t>> roms;
    std::vector<uint8_t> ram = std::vector<uint8_t>(8 * 0x4000, 0);
    bool is128 = false;
    uint8_t port7ffd = 0;
    int page[4] = {-1, 5, 2, 0};                // -1: ROM
    int rom = 0;

    void map() {
        rom = is128 ? ((port7ffd >> 4) & 1) : 0;
        page[3] = is128 ? (port7ffd & 7) : 0;
    }
    int screenBank() const { return (is128 && (port7ffd & 8)) ? 7 : 5; }
    bool locked() const { return is128 && (port7ffd & 0x20); }
    /** The bank a page holds, -1 for ROM. */
    int bankAt(uint16_t a) const { return page[a >> 14]; }
    /** Contended: bank 5 on the 48K; the odd banks on the 128K. */
    bool contended(uint16_t a) const {
        const int b = bankAt(a);
        if (b < 0) return false;
        return is128 ? (b & 1) : b == 5;
    }
    uint8_t read(uint16_t a) const {
        const int b = bankAt(a);
        return b < 0 ? roms[rom][a & 0x3FFF] : ram[(size_t)b * 0x4000 + (a & 0x3FFF)];
    }
    uint8_t* bank(int b) { return &ram[(size_t)b * 0x4000]; }
};

// ---------------------------------------------------------------------------
// The ULA: contention, the display, port $FE's outputs, the beeper
// ---------------------------------------------------------------------------
class SpecUla : public IZ80Timing, public IVideoOutput, public IAudioOutput {
public:
    static constexpr int WIDTH = COLS * 8, HEIGHT = ROWS;

    SpecUla(const Timings& t, Memory* mem) : m_t(t), m_mem(mem) {
        m_lineTime0 = t.topLeftPixel - BORDER_LINES * t.tstatesPerLine - 4 * BORDER_COLS;
        m_canvas.assign((size_t)WIDTH * HEIGHT, 0xFF000000);
        m_frame = m_canvas;
    }

    void reset(uint64_t frameStart) {
        m_frameStart = frameStart;
        m_cy = m_cx = 0;
        m_border = 7;
        m_out = 0;
        m_audT = frameStart;
        m_level = 0;
        m_acc = 0;
        m_accT = 0;
    }

    const Timings& timings() const { return m_t; }
    uint64_t frameStart() const { return m_frameStart; }
    uint64_t frames() const { return m_frames; }
    int border() const { return m_border; }
    uint8_t lastOut() const { return m_out; }

    /** The T-state in this frame. */
    int64_t ft(uint64_t t) const { return (int64_t)(t - m_frameStart); }

    // Fuse spectrum.c contend_delay_common, pattern 6,5,4,3,2,1,0,0
    int delay(int64_t time) const {
        static const int PATTERN[8] = {6, 5, 4, 3, 2, 1, 0, 0};
        if (time < m_lineTime0) return 0;
        const int64_t line = (time - m_lineTime0) / m_t.tstatesPerLine;
        if (line < BORDER_LINES || line >= BORDER_LINES + 192) return 0;
        const int64_t through = (time - m_lineTime0 + (LEFT_BORDER_T - BORDER_COLS * 4)) % m_t.tstatesPerLine;
        if (through < LEFT_BORDER_T - 1 || through >= LEFT_BORDER_T + SCREEN_T - 1) return 0;
        return PATTERN[through % 8];
    }

    // IZ80Timing
    int contend(uint16_t addr, uint64_t t, bool) override {
        return m_mem->contended(addr) ? delay(ft(t)) : 0;
    }
    // Fuse periph.c readport / writeport, ula.c ula_contend_port_early/late:
    // a read is made after both, a write between them; 1 more T-state after
    int ioBefore(uint16_t port, uint64_t t, bool write) override {
        int64_t tt = ft(t);
        const int64_t start = tt;
        if (m_mem->contended(port)) tt += delay(tt);
        tt += 1;
        if (!write) tt += late(port, tt);
        return (int)(tt - start);
    }
    int ioAfter(uint16_t port, uint64_t t, bool write) override {
        if (!write) return 1;
        const int64_t tt = ft(t);
        return (int)(late(port, tt) + 1);
    }

    /** The floating bus (Fuse spectrum_unattached_port): what the ULA is reading. */
    uint8_t floating(uint64_t t) const {
        const int64_t time = ft(t);
        const int64_t top = m_lineTime0 + (int64_t)BORDER_LINES * m_t.tstatesPerLine;
        if (time < top) return 0xFF;
        const int64_t line = (time - top) / m_t.tstatesPerLine;
        if (line >= 192) return 0xFF;
        const int64_t through = time - (top + line * m_t.tstatesPerLine) + (LEFT_BORDER_T - BORDER_COLS * 4);
        if (through < LEFT_BORDER_T || through >= LEFT_BORDER_T + SCREEN_T) return 0xFF;
        int column = (int)((through - LEFT_BORDER_T) / 8) * 2;
        const uint8_t* scr = m_mem->bank(m_mem->screenBank());
        switch (through % 8) {
            case 5: column++; [[fallthrough]];
            case 3: return scr[0x1800 + (line >> 3) * 32 + column];
            case 4: column++; [[fallthrough]];
            case 2: return scr[pixelAddr((int)line) + column];
            default: return 0xFF;
        }
    }

    /** A write to port $FE: the border from the beam's position, MIC and EAR. */
    void out(uint64_t t, uint8_t v) {
        drawTo(t);
        audioTo(t);
        m_out = v;
        m_border = v & 7;
        m_level = ((v & 0x10) ? 0.5f : 0.0f) + ((v & 0x08) ? 0.0f : 0.06f);   // EAR, MIC (Fuse sound_ula)
    }

    /** Draw the cells the beam has passed (Fuse display_get_beam_position): before
     *  a write to the screen or the border, and at the end of the frame. */
    void drawTo(uint64_t t) {
        const int64_t time = ft(t);
        if (time < m_lineTime0) return;
        int by = (int)((time - m_lineTime0) / m_t.tstatesPerLine), bx;
        if (by >= ROWS) { by = ROWS; bx = 0; }
        else bx = std::min(COLS, (int)((time - m_lineTime0 - (int64_t)by * m_t.tstatesPerLine) / 4));
        while (m_cy < by || (m_cy == by && m_cx < bx)) {
            drawCell(m_cx, m_cy);
            if (++m_cx == COLS) { m_cx = 0; ++m_cy; }
        }
    }

    /** The frame is over: the rest of it drawn, the next begun. */
    void endFrame() {
        const uint64_t end = m_frameStart + (uint64_t)m_t.frame();
        drawTo(end);
        audioTo(end);
        m_frame = m_canvas;
        m_frameStart = end;
        m_cy = m_cx = 0;
        if (++m_frames % 16 == 0) m_flash = !m_flash;
    }

    // IVideoOutput
    VideoDimensions getDimensions() const override { return {WIDTH, HEIGHT, WIDTH, HEIGHT}; }
    void renderFrame(uint32_t* buffer) override {
        std::memcpy(buffer, m_frame.data(), m_frame.size() * sizeof(uint32_t));
    }
    // IAudioOutput
    int nativeSampleRate() const override { return SAMPLE_RATE; }
    int pullSamples(float* buffer, int maxSamples) override {
        int n = 0;
        while (n < maxSamples && !m_samples.empty()) { buffer[n++] = m_samples.front(); m_samples.pop_front(); }
        return n;
    }
    /** The beeper's samples so far (tests). */
    const std::deque<float>& samples() const { return m_samples; }

private:
    static int pixelAddr(int y) { return ((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2); }

    int64_t late(uint16_t port, int64_t tt) const {
        const int64_t start = tt;
        if (!(port & 1)) {                       // the ULA's own port
            tt += delay(tt); tt += 2;
        } else if (m_mem->contended(port)) {
            tt += delay(tt); tt += 1;
            tt += delay(tt); tt += 1;
            tt += delay(tt);
        } else {
            tt += 2;
        }
        return tt - start;
    }

    void drawCell(int cx, int cy) {
        static const uint32_t PAL[16] = {
            0xFF000000, 0xFF0000C0, 0xFFC00000, 0xFFC000C0, 0xFF00C000, 0xFF00C0C0, 0xFFC0C000, 0xFFC0C0C0,
            0xFF000000, 0xFF0000FF, 0xFFFF0000, 0xFFFF00FF, 0xFF00FF00, 0xFF00FFFF, 0xFFFFFF00, 0xFFFFFFFF};
        uint32_t* px = &m_canvas[(size_t)cy * WIDTH + (size_t)cx * 8];
        const int y = cy - BORDER_LINES, x = cx - BORDER_COLS;
        if (y < 0 || y >= 192 || x < 0 || x >= 32) {
            for (int i = 0; i < 8; ++i) px[i] = PAL[m_border];
            return;
        }
        const uint8_t* scr = m_mem->bank(m_mem->screenBank());
        uint8_t bits = scr[pixelAddr(y) + x];
        const uint8_t attr = scr[0x1800 + (y >> 3) * 32 + x];
        const int bright = (attr & 0x40) ? 8 : 0;
        uint32_t ink = PAL[(attr & 7) | bright], paper = PAL[((attr >> 3) & 7) | bright];
        if ((attr & 0x80) && m_flash) std::swap(ink, paper);
        for (int i = 0; i < 8; ++i, bits <<= 1) px[i] = (bits & 0x80) ? ink : paper;
    }

    // The beeper: the level held between port writes, averaged per sample
    void audioTo(uint64_t t) {
        const double per = (double)m_t.cpuHz / SAMPLE_RATE;
        while (m_audT < t) {
            const double room = per - m_accT;
            const uint64_t left = t - m_audT;
            if ((double)left < room) {
                m_acc += m_level * (double)left;
                m_accT += (double)left;
                m_audT = t;
            } else {
                const uint64_t step = (uint64_t)room + ((room - (double)(uint64_t)room) > 0 ? 1 : 0);
                m_acc += m_level * room;
                m_audT += step;
                m_samples.push_back((float)(m_acc / per) - 0.28f);
                if (m_samples.size() > (size_t)SAMPLE_RATE) m_samples.pop_front();
                m_acc = 0;
                m_accT = (double)step - room;
            }
        }
    }

    Timings m_t;
    Memory* m_mem;
    int64_t m_lineTime0 = 0;
    uint64_t m_frameStart = 0, m_frames = 0;
    int m_cy = 0, m_cx = 0;
    int m_border = 7;
    uint8_t m_out = 0;
    bool m_flash = false;
    std::vector<uint32_t> m_canvas, m_frame;
    uint64_t m_audT = 0;
    float m_level = 0;
    double m_acc = 0, m_accT = 0;
    std::deque<float> m_samples;
};

// ---------------------------------------------------------------------------
// The memory bus: a write to the screen draws the beam's cells first
// ---------------------------------------------------------------------------
class SpecBus : public IBus {
public:
    SpecBus(Memory* mem, SpecUla* ula, Z80* cpu) : m_mem(mem), m_ula(ula), m_cpu(cpu) {
        m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF};
    }
    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "system"; }
    uint8_t read8(uint32_t addr) override {
        const uint16_t a = addr & 0xFFFF;
        const uint8_t v = m_mem->read(a);
        if (m_observer) m_observer->onMemoryRead(this, a, v);
        return v;
    }
    uint8_t peek8(uint32_t addr) override { return m_mem->read(addr & 0xFFFF); }
    void write8(uint32_t addr, uint8_t v) override {
        const uint16_t a = addr & 0xFFFF;
        if (m_observer) m_observer->onMemoryWrite(this, a, peek8(a), v);
        const int b = m_mem->bankAt(a);
        if (b < 0) return;                       // ROM
        if (b == m_mem->screenBank() && (a & 0x3FFF) < 0x1B00) m_ula->drawTo(m_cpu->cycles());
        m_mem->ram[(size_t)b * 0x4000 + (a & 0x3FFF)] = v;
    }
    void reset() override {}
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

private:
    BusConfig m_cfg;
    Memory* m_mem;
    SpecUla* m_ula;
    Z80* m_cpu;
};

// ---------------------------------------------------------------------------
// The I/O bus: the ULA (even ports), Kempston ($1F), $7FFD on the 128K, the
// floating bus where nothing answers (Fuse periph.c readport_internal)
// ---------------------------------------------------------------------------
class SpecIo : public IBus {
public:
    SpecIo(Memory* mem, SpecUla* ula, SpecKeyboard* kbd, Z80* cpu, SpectrumTape* tape)
        : m_mem(mem), m_ula(ula), m_kbd(kbd), m_cpu(cpu), m_tape(tape) {
        m_cfg = {16, 8, BusRole::IO_PORT, true, 0xFFFF};
    }
    uint8_t kempston = 0;                        // right 1, left 2, down 4, up 8, fire 16
    bool issue2 = false;

    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "io"; }
    uint8_t read8(uint32_t addr) override {
        const uint16_t port = addr & 0xFFFF;
        uint8_t v = 0xFF, attached = 0;
        if (!(port & 1)) {                       // the ULA: keys, EAR
            v &= ulaDefault() & (uint8_t)(m_kbd->read((uint8_t)(port >> 8)) | 0xE0);
            const uint64_t now = m_cpu->cycles();
            m_tape->earRead(now, (uint8_t)(m_cpu->regRead(Z80::R_BC) >> 8));
            if (m_tape->ear(now)) v ^= 0x40;       // Fuse ula_read: tape_microphone
            attached = 0xFF;
        }
        if (!(port & 0xE0)) { v &= kempston; attached = 0xFF; }
        if (attached != 0xFF) v &= (uint8_t)(m_ula->floating(m_cpu->cycles()) | attached);
        return v;
    }
    uint8_t peek8(uint32_t) override { return 0xFF; }
    void write8(uint32_t addr, uint8_t v) override {
        const uint16_t port = addr & 0xFFFF;
        if (!(port & 1)) m_ula->out(m_cpu->cycles(), v);
        if (m_mem->is128 && !(port & 0x8002) && !m_mem->locked()) {
            if (((m_mem->port7ffd ^ v) & 8)) m_ula->drawTo(m_cpu->cycles());   // the screen moves
            m_mem->port7ffd = v;
            m_mem->map();
        }
    }
    void reset() override {}
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

private:
    // Fuse ula_write: bit 6 (EAR in) follows the last EAR output (Issue 3;
    // Issue 2 also MIC); the 128K acts as an Issue 3
    uint8_t ulaDefault() const {
        const uint8_t o = m_ula->lastOut();
        if (issue2 && !m_mem->is128) return (o & 0x18) ? 0xFF : 0xBF;
        return (o & 0x10) ? 0xFF : 0xBF;
    }

    BusConfig m_cfg;
    Memory* m_mem;
    SpecUla* m_ula;
    SpecKeyboard* m_kbd;
    Z80* m_cpu;
    SpectrumTape* m_tape;
};

// The machine's parts, for the snapshot and tape loaders
class SpecParts : public IOHandler {
public:
    Memory* mem = nullptr;
    SpecUla* ula = nullptr;
    Z80* cpu = nullptr;
    SpectrumTape* tape = nullptr;
    SpecKeyboard* kbd = nullptr;
    MachineDescriptor* desc = nullptr;
    bool is128 = false;
    const char* name() const override { return "Spectrum"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
};

// The ULA as the frontends find it (video and sound)
class UlaHandle : public IOHandler, public IVideoOutput, public IAudioOutput {
public:
    explicit UlaHandle(SpecUla* u) : m_u(u) {}
    const char* name() const override { return "ULA"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    VideoDimensions getDimensions() const override { return m_u->getDimensions(); }
    void renderFrame(uint32_t* buffer) override { m_u->renderFrame(buffer); }
    int nativeSampleRate() const override { return m_u->nativeSampleRate(); }
    int pullSamples(float* buffer, int maxSamples) override { return m_u->pullSamples(buffer, maxSamples); }
private:
    SpecUla* m_u;
};

}  // namespace

MachineDescriptor* create(Model model, std::string* err) {
    const bool is128 = model != Model::S48;
    Memory* mem = new Memory();
    mem->is128 = is128;
    {
        std::vector<std::string> paths;
        if (model == Model::S48) paths = {"roms/spectrum/48.rom"};
        else if (model == Model::S128) paths = {"roms/spectrum/128-0.rom", "roms/spectrum/128-1.rom"};
        else paths = {"roms/spectrum/plus2-0.rom", "roms/spectrum/plus2-1.rom"};
        for (const auto& p : paths) {
            std::vector<uint8_t> r;
            if (!loadRom(p, r, err)) { delete mem; return nullptr; }
            mem->roms.push_back(std::move(r));
        }
    }
    const Timings& tim = is128 ? T128 : T48;

    auto* desc = new MachineDescriptor();
    desc->machineId = model == Model::S48 ? "spectrum48" : model == Model::S128 ? "spectrum128" : "spectrumplus2";
    desc->displayName = model == Model::S48 ? "ZX Spectrum 48K" : model == Model::S128 ? "ZX Spectrum 128K" : "ZX Spectrum +2";
    desc->licenseClass = "proprietary";
    desc->uiPanesSet = true;
    desc->clockHz = tim.cpuHz;

    auto* cpu = new Z80();
    auto* kbd = new SpecKeyboard();
    auto* ula = new SpecUla(tim, mem);
    auto* bus = new SpecBus(mem, ula, cpu);
    auto* tape = new SpectrumTape(tim.cpuHz);
    tape->set48k(!is128);
    auto* iobus = new SpecIo(mem, ula, kbd, cpu, tape);
    auto* parts = new SpecParts();
    parts->mem = mem; parts->ula = ula; parts->cpu = cpu; parts->tape = tape; parts->kbd = kbd;
    parts->desc = desc; parts->is128 = is128;
    auto* video = new UlaHandle(ula);
    kbd->peek = [mem](uint16_t a) { return mem->read(a); };
    kbd->poke = [mem](uint16_t a, uint8_t v) { if (mem->bankAt(a) >= 0) mem->ram[(size_t)mem->bankAt(a) * 0x4000 + (a & 0x3FFF)] = v; };

    cpu->setDataBus(bus);
    cpu->setIoBus(iobus);
    cpu->setTiming(ula);
    desc->buses.push_back({"system", bus});
    desc->buses.push_back({"io", iobus});
    desc->cpus.push_back({"main", cpu, bus, bus, iobus, true, 1});

    auto* io = new IORegistry();
    desc->ioRegistry = io;
    io->registerHandler(video);
    io->registerHandler(kbd);
    io->registerHandler(tape);
    io->registerHandler(parts);
    desc->deleters.push_back([video, kbd, ula, mem, tape, parts]() {
        delete video; delete kbd; delete ula; delete mem; delete tape; delete parts;
    });
    desc->uiPanes = {"tape"};

    desc->onKey = [kbd](const std::string& n, bool down) { return kbd->key(n, down); };
    desc->screenButtons = {{"BREAK", {"escape"}, "BREAK: Caps Shift + Space (stops a BASIC program)."}};

    // Kempston: the host joystick (CBM bits, active low) as right/left/down/up/fire
    desc->joystickPorts = 1;
    desc->primaryJoystickPort = 0;
    desc->onJoystick = [iobus](int, uint8_t bits) {
        uint8_t k = 0;
        if (!(bits & 0x08)) k |= 0x01;
        if (!(bits & 0x04)) k |= 0x02;
        if (!(bits & 0x02)) k |= 0x04;
        if (!(bits & 0x01)) k |= 0x08;
        if (!(bits & 0x10)) k |= 0x10;
        iobus->kempston = k;
    };

    desc->onReset = [mem, ula, io, cpu, kbd](MachineDescriptor&) {     // power on
        std::fill(mem->ram.begin(), mem->ram.end(), 0);
        mem->port7ffd = 0;
        mem->map();
        io->resetAll();
        kbd->reset();
        cpu->reset();
        ula->reset(cpu->cycles());
    };
    // Each ROM's key-read point, for typed text
    std::vector<std::pair<int, bool>> keyRead;
    for (const auto& r : mem->roms) { bool tok = false; int pc = keyReadPoint(r, tok); keyRead.push_back({pc, tok}); }
    // The tape traps, in the 48 BASIC ROM (Fuse trap_check_rom: LD-BYTES's
    // RET NZ at $056B, SA-BYTES's EX AF,AF' at $04D0)
    std::vector<bool> tapeRom;
    for (const auto& r : mem->roms) tapeRom.push_back(r[0x056B] == 0xC0 && r[0x04D0] == 0x08);
    desc->schedulerStep = [cpu, ula, kbd, tim, mem, keyRead, tape, tapeRom](MachineDescriptor&) -> int {
        const uint16_t pc = (uint16_t)cpu->pc();
        if (pc < 0x4000) {
            if (kbd->typing()) {
                const auto& k = keyRead[(size_t)mem->rom];
                if ((int)pc == k.first) kbd->atKeyRead(k.second);
            }
            if ((pc == 0x056B || pc == 0x04D0) && tapeRom[(size_t)mem->rom]) {
                auto rd = [mem](uint16_t a) { return mem->read(a); };
                auto wr = [mem](uint16_t a, uint8_t v) {
                    const int b = mem->bankAt(a);
                    if (b >= 0) mem->ram[(size_t)b * 0x4000 + (a & 0x3FFF)] = v;
                };
                if (pc == 0x056B && tape->mounted()) tape->trapLoad(*cpu, rd, wr);
                else if (pc == 0x04D0) tape->trapSave(*cpu, rd);
            }
        }
        // /INT is held for the first 32 (36) T-states of the frame
        cpu->setIrqLine(ula->ft(cpu->cycles()) < tim.interruptLength);
        const int c = cpu->step();
        if (tape->playing()) tape->advance(cpu->cycles());
        if (ula->ft(cpu->cycles()) >= tim.frame()) {
            ula->endFrame();
        }
        return c;
    };
    if (desc->onReset) desc->onReset(*desc);
    return desc;
}


// ---------------------------------------------------------------------------
// Loaders: tapes (mount, reset, LOAD ""), snapshots (.sna, .z80 v1-v3; the
// formats as libspectrum's snapshot readers take them)
// ---------------------------------------------------------------------------
namespace {

SpecParts* partsOf(MachineDescriptor* m) {
    if (!m || !m->ioRegistry) return nullptr;
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if (auto* p = dynamic_cast<SpecParts*>(h)) return p;
    return nullptr;
}

uint16_t w16(const std::vector<uint8_t>& b, size_t at) { return (uint16_t)(b[at] | b[at + 1] << 8); }

void putBank(Memory* mem, int bank, const uint8_t* src) { std::memcpy(mem->bank(bank), src, 0x4000); }

// The registers both formats hold, and the state around them
void startFrom(SpecParts* p, uint8_t border, uint8_t port7ffd) {
    p->mem->port7ffd = p->is128 ? port7ffd : 0;
    p->mem->map();
    p->ula->reset(p->cpu->cycles());
    p->ula->out(p->cpu->cycles(), border & 7);
    p->tape->stop();
}

bool loadSna(SpecParts* p, const std::vector<uint8_t>& b, std::string& err) {
    const bool snap128 = b.size() == 131103 || b.size() == 147487;
    if (b.size() != 49179 && !snap128) { err = "not a .sna snapshot (wrong size)"; return false; }
    if (snap128 && !p->is128) { err = "a 128K snapshot needs spectrum128"; return false; }
    Z80& c = *p->cpu;
    c.reset();
    c.regWrite(Z80::R_I, b[0]);
    c.regWrite(Z80::R_HL2, w16(b, 1)); c.regWrite(Z80::R_DE2, w16(b, 3));
    c.regWrite(Z80::R_BC2, w16(b, 5)); c.regWrite(Z80::R_AF2, w16(b, 7));
    c.regWrite(Z80::R_HL, w16(b, 9)); c.regWrite(Z80::R_DE, w16(b, 11)); c.regWrite(Z80::R_BC, w16(b, 13));
    c.regWrite(Z80::R_IY, w16(b, 15)); c.regWrite(Z80::R_IX, w16(b, 17));
    const bool iff = b[19] & 0x04;
    c.regWrite(Z80::R_IFF, iff ? 3 : 0);
    c.regWrite(Z80::R_R, b[20]);
    c.regWrite(Z80::R_AF, w16(b, 21));
    uint16_t sp = w16(b, 23);
    c.regWrite(Z80::R_IM, b[25] & 3);
    Memory* mem = p->mem;
    uint8_t port = 0;
    if (!snap128) {
        if (p->is128) port = 0x30;                       // 48K mode: ROM 1, locked
        putBank(mem, 5, &b[27]); putBank(mem, 2, &b[27 + 0x4000]); putBank(mem, 0, &b[27 + 0x8000]);
        startFrom(p, b[26], port);
        // The PC is on the stack (the snapshot was taken in an interrupt)
        const uint16_t pc = (uint16_t)(mem->read(sp) | mem->read((uint16_t)(sp + 1)) << 8);
        sp = (uint16_t)(sp + 2);
        c.regWrite(Z80::R_SP, sp);
        c.setPc(pc);
        return true;
    }
    port = b[49181];
    const int paged = port & 7;
    putBank(mem, 5, &b[27]); putBank(mem, 2, &b[27 + 0x4000]); putBank(mem, paged, &b[27 + 0x8000]);
    size_t at = 49183;
    for (int bank = 0; bank < 8; ++bank) {
        if (bank == 5 || bank == 2 || bank == paged) continue;
        if (at + 0x4000 > b.size()) break;
        putBank(mem, bank, &b[at]);
        at += 0x4000;
    }
    startFrom(p, b[26], port);
    c.regWrite(Z80::R_SP, sp);
    c.setPc(w16(b, 49179));
    return true;
}

// .z80 RLE: ED ED nn bb is nn copies of bb
std::vector<uint8_t> unrle(const uint8_t* src, size_t n, size_t want) {
    std::vector<uint8_t> out;
    out.reserve(want);
    for (size_t i = 0; i < n && out.size() < want;) {
        if (i + 3 < n && src[i] == 0xED && src[i + 1] == 0xED) {
            out.insert(out.end(), src[i + 2], src[i + 3]);
            i += 4;
        } else {
            out.push_back(src[i++]);
        }
    }
    out.resize(want, 0);
    return out;
}

bool loadZ80(SpecParts* p, const std::vector<uint8_t>& b, std::string& err) {
    if (b.size() < 30) { err = "not a .z80 snapshot"; return false; }
    Z80& c = *p->cpu;
    uint8_t flags = b[12] == 0xFF ? 1 : b[12];
    uint16_t pc = w16(b, 6);
    Memory* mem = p->mem;
    bool snap128 = false;
    uint8_t port = 0;
    size_t at = 30;
    std::vector<std::pair<int, std::vector<uint8_t>>> banks;    // RAM bank, contents
    if (pc) {                                                    // version 1: 48K
        std::vector<uint8_t> ram = (flags & 0x20) ? unrle(&b[30], b.size() - 30, 0xC000)
                                                  : std::vector<uint8_t>(b.begin() + 30, b.end());
        ram.resize(0xC000, 0);
        banks.push_back({5, std::vector<uint8_t>(ram.begin(), ram.begin() + 0x4000)});
        banks.push_back({2, std::vector<uint8_t>(ram.begin() + 0x4000, ram.begin() + 0x8000)});
        banks.push_back({0, std::vector<uint8_t>(ram.begin() + 0x8000, ram.end())});
    } else {                                                     // versions 2 and 3
        if (b.size() < 34) { err = "truncated .z80 snapshot"; return false; }
        const size_t extra = w16(b, 30);
        if (32 + extra > b.size()) { err = "truncated .z80 snapshot"; return false; }
        pc = w16(b, 32);
        const int hw = b[34];
        snap128 = extra == 23 ? (hw == 3 || hw == 4) : (hw >= 4 && hw <= 6) || hw == 12;
        if (extra == 23 ? hw > 4 : (hw > 6 && hw != 12)) { err = "unsupported .z80 hardware (+3, Timex or another)"; return false; }
        port = b[35];
        at = 32 + extra;
        while (at + 3 <= b.size()) {
            const size_t len = w16(b, at);
            const int page = b[at + 2];
            at += 3;
            std::vector<uint8_t> data;
            if (len == 0xFFFF) {
                if (at + 0x4000 > b.size()) break;
                data.assign(b.begin() + (long)at, b.begin() + (long)(at + 0x4000));
                at += 0x4000;
            } else {
                if (at + len > b.size()) break;
                data = unrle(&b[at], len, 0x4000);
                at += len;
            }
            int bank = -1;
            if (snap128) { if (page >= 3 && page <= 10) bank = page - 3; }
            else if (page == 8) bank = 5;
            else if (page == 4) bank = 2;
            else if (page == 5) bank = 0;
            if (bank >= 0) banks.push_back({bank, std::move(data)});
        }
    }
    if (snap128 && !p->is128) { err = "a 128K snapshot needs spectrum128"; return false; }
    c.reset();
    c.regWrite(Z80::R_AF, (uint32_t)(b[0] << 8 | b[1]));
    c.regWrite(Z80::R_BC, w16(b, 2)); c.regWrite(Z80::R_HL, w16(b, 4));
    c.regWrite(Z80::R_SP, w16(b, 8));
    c.regWrite(Z80::R_I, b[10]);
    c.regWrite(Z80::R_R, (uint32_t)((b[11] & 0x7F) | ((flags & 1) << 7)));
    c.regWrite(Z80::R_DE, w16(b, 13));
    c.regWrite(Z80::R_BC2, w16(b, 15)); c.regWrite(Z80::R_DE2, w16(b, 17)); c.regWrite(Z80::R_HL2, w16(b, 19));
    c.regWrite(Z80::R_AF2, (uint32_t)(b[21] << 8 | b[22]));
    c.regWrite(Z80::R_IY, w16(b, 23)); c.regWrite(Z80::R_IX, w16(b, 25));
    c.regWrite(Z80::R_IFF, (b[27] ? 1 : 0) | (b[28] ? 2 : 0));
    c.regWrite(Z80::R_IM, b[29] & 3);
    for (auto& [bank, data] : banks) putBank(mem, bank, data.data());
    startFrom(p, (uint8_t)((flags >> 1) & 7), snap128 ? port : (p->is128 ? 0x30 : 0));
    c.setPc(pc);
    return true;
}

class SnapshotLoader : public IImageLoader {
public:
    bool canLoad(const std::string& path) const override {
        const std::string e = diskfile::imageExt(path);
        return e == "sna" || e == "z80";
    }
    bool load(const std::string& path, IBus*, MachineDescriptor* m, uint32_t) override {
        SpecParts* p = partsOf(m);
        if (!p) return false;
        std::vector<uint8_t> b;
        std::string err;
        if (!diskfile::read(path, b, &err)) return false;
        const bool ok = diskfile::imageExt(path) == "sna" ? loadSna(p, b, err) : loadZ80(p, b, err);
        if (!ok) std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
        return ok;
    }
    const char* name() const override { return "ZX Spectrum Snapshot Loader"; }
};

// A tape: mounted, the machine reset, and loading started as a user would
// (48K: LOAD ""; 128K: the menu's Tape Loader)
class TapeLoader : public IImageLoader {
public:
    bool canLoad(const std::string& path) const override {
        const std::string e = diskfile::imageExt(path);
        return e == "tap" || e == "tzx";
    }
    bool load(const std::string& path, IBus*, MachineDescriptor* m, uint32_t) override {
        SpecParts* p = partsOf(m);
        if (!p || !p->tape->mountTape(path)) return false;
        if (m->onReset) m->onReset(*m);
        p->kbd->enqueueText(p->is128 ? "\n" : "LOAD \"\"\n");
        return true;
    }
    const char* name() const override { return "ZX Spectrum Tape Loader"; }
};

}  // namespace

IImageLoader* createSnapshotLoader() { return new SnapshotLoader(); }
IImageLoader* createTapeLoader() { return new TapeLoader(); }

bool loadSnapshot(MachineDescriptor* m, const std::vector<uint8_t>& bytes, bool z80, std::string* err) {
    SpecParts* p = partsOf(m);
    if (!p) { if (err) *err = "not a ZX Spectrum"; return false; }
    std::string e;
    const bool ok = z80 ? loadZ80(p, bytes, e) : loadSna(p, bytes, e);
    if (!ok && err) *err = e;
    return ok;
}

}  // namespace spectrum
