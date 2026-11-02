#include "audio_scope.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/iaudio_inspect.h"
#include "libdevices/main/io_registry.h"
#include <cmath>
#include <vector>

namespace scope {

std::vector<IAudioInspect*> findAudioInspect(MachineDescriptor* desc) {
    std::vector<IAudioInspect*> out;
    if (!desc || !desc->ioRegistry) return out;
    std::vector<IOHandler*> handlers;
    desc->ioRegistry->enumerate(handlers);
    for (auto* h : handlers)
        if (auto* a = dynamic_cast<IAudioInspect*>(h)) out.push_back(a);
    return out;
}

std::string noteName(double hz, int* centsOut) {
    static const char* const NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (centsOut) *centsOut = 0;
    if (!(hz >= 8.0)) return "";
    double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
    int n = (int)std::lround(midi);
    if (centsOut) *centsOut = (int)std::lround((midi - n) * 100.0);
    int octave = n / 12 - 1;   // MIDI 60 = C4
    return std::string(NAMES[((n % 12) + 12) % 12]) + std::to_string(octave);
}

std::string controlText(uint8_t cr) {
    std::string wave;
    auto add = [&](uint8_t bit, const char* name) {
        if (!(cr & bit)) return;
        if (!wave.empty()) wave += '+';
        wave += name;
    };
    add(0x10, "Tri");
    add(0x20, "Saw");
    add(0x40, "Pulse");
    add(0x80, "Noise");
    std::string s = wave.empty() ? "-" : wave;
    if (cr & 0x02) s += " Sync";
    if (cr & 0x04) s += " Ring";
    if (cr & 0x08) s += " Test";
    if (cr & 0x01) s += " Gate";
    return s;
}

int findTrigger(const float* s, int count, int window, float hyst) {
    if (!s || window <= 0 || count < window + 1) return -1;
    // Scan back from the latest start that leaves a full window. A crossing
    // counts once the signal has been below -hyst since the previous crossing.
    int last = count - window;
    for (int i = last; i >= 1; --i) {
        if (!(s[i - 1] < 0.0f && s[i] >= 0.0f)) continue;
        for (int j = i - 1; j >= 0 && s[j] < 0.0f; --j)
            if (s[j] < -hyst) return i;
    }
    return -1;
}

int attackMs(int nibble) {
    static const int MS[16] = {2, 8, 16, 24, 38, 56, 68, 80, 100, 250, 500, 800, 1000, 3000, 5000, 8000};
    return MS[nibble & 15];
}

int decayReleaseMs(int nibble) { return attackMs(nibble) * 3; }

} // namespace scope

// ---------------------------------------------------------------------------
// Shared scope logic and the image renderer
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace scope {

const char* phaseName(int p) {
    switch ((IAudioInspect::EnvPhase)p) {
        case IAudioInspect::EnvPhase::Attack:  return "Attack";
        case IAudioInspect::EnvPhase::Decay:   return "Decay";
        case IAudioInspect::EnvPhase::Sustain: return "Sustain";
        case IAudioInspect::EnvPhase::Release: return "Release";
        default:                               return "Idle";
    }
}

uint32_t traceColour(int channel) {
    static const uint32_t C[4] = {0xFF50DC64, 0xFF50BEFF, 0xFFFFAA46, 0xFFE1E1E1};
    return C[std::max(0, std::min(channel, 3))];
}

TraceWindow traceWindow(const float* s, int n, int rate, int ms, bool triggered) {
    TraceWindow w;
    if (!s || n < 2) return w;
    w.length = std::min(n, std::max(2, rate * ms / 1000));
    w.start = n - w.length;
    if (triggered) {
        int t = findTrigger(s, n, w.length);
        if (t >= 0) w.start = t;
        else w.untriggered = std::any_of(s + w.start, s + n, [](float v) { return std::fabs(v) >= 0.01f; });
    }
    return w;
}

std::string voiceSummary(IAudioInspect* insp, int chip) {
    std::string out;
    if (!insp || chip < 0 || chip >= insp->chipCount()) return out;
    char line[200];
    for (int v = 0; v < insp->voiceCount(chip); ++v) {
        IAudioInspect::VoiceInfo vi;
        if (!insp->voiceInfo(chip, v, vi)) continue;
        int cents = 0;
        std::string note = noteName(vi.freqHz, &cents);
        std::snprintf(line, sizeof line, "Voice %d: %s, %.1f Hz", v + 1, controlText(vi.control).c_str(), vi.freqHz);
        out += line;
        if (!note.empty()) { std::snprintf(line, sizeof line, " (%s %+dc)", note.c_str(), cents); out += line; }
        std::snprintf(line, sizeof line, ", PW %.1f%%, ADSR %X/%X/%X/%X, %s %d", vi.pulseWidth * 100.0 / 4096.0,
                      vi.attack, vi.decay, vi.sustain, vi.release, phaseName((int)vi.envPhase), vi.envLevel);
        out += line;
        if (vi.filtered) out += ", filtered";
        if (vi.muted) out += ", off";
        out += "\n";
    }
    return out;
}

namespace {

// 5x7 font for the characters scope labels use (drawn upper-case)
struct Glyph { char c; const char* rows[7]; };
const Glyph FONT[] = {
    {'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {'3', {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."}},
    {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6', {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}},
    {'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
    {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9', {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."}},
    {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
    {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
    {'D', {"###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###.."}},
    {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
    {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
    {'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"}},
    {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
    {'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
    {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
    {'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
    {'N', {"#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"}},
    {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
    {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
    {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
    {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."}},
    {'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
    {'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
    {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
    {'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
    {',', {".....", ".....", ".....", ".....", ".##..", "..#..", ".#..."}},
    {':', {".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}},
    {'%', {"##...", "##..#", "...#.", "..#..", ".#...", "#..##", "...##"}},
    {'+', {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}},
    {'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
    {'#', {".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#."}},
    {'$', {"..#..", ".####", "#.#..", ".###.", "..#.#", "####.", "..#.."}},
    {'/', {".....", "....#", "...#.", "..#..", ".#...", "#....", "....."}},
    {'(', {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."}},
    {')', {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."}},
};

const Glyph* glyphFor(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (const Glyph& g : FONT) if (g.c == c) return &g;
    return nullptr;   // space and anything unknown draw nothing
}

constexpr int CHAR_W = 6, CHAR_H = 7, LINE_H = 10;

class Canvas {
public:
    Canvas(Image& img) : m_img(img) {}
    void pixel(int x, int y, uint32_t c) {
        if (x >= 0 && y >= 0 && x < m_img.width && y < m_img.height) m_img.argb[(size_t)y * m_img.width + x] = c;
    }
    void line(int x0, int y0, int x1, int y1, uint32_t c) {   // Bresenham
        int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            pixel(x0, y0, c);
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
    void rect(int x, int y, int w, int h, uint32_t c) {
        line(x, y, x + w - 1, y, c); line(x, y + h - 1, x + w - 1, y + h - 1, c);
        line(x, y, x, y + h - 1, c); line(x + w - 1, y, x + w - 1, y + h - 1, c);
    }
    void fill(int x, int y, int w, int h, uint32_t c) {
        for (int j = 0; j < h; ++j) for (int i = 0; i < w; ++i) pixel(x + i, y + j, c);
    }
    int text(int x, int y, const std::string& s, uint32_t c) {
        for (char ch : s) {
            if (const Glyph* g = glyphFor(ch))
                for (int r = 0; r < CHAR_H; ++r)
                    for (int k = 0; k < 5; ++k)
                        if (g->rows[r][k] == '#') pixel(x + k, y + r, c);
            x += CHAR_W;
        }
        return x;
    }
private:
    Image& m_img;
};

void drawEnvelope(Canvas& cv, int x, int y, int w, int h, const IAudioInspect::VoiceInfo& vi, uint32_t colour) {
    const int barW = 6;
    int sw = w - barW - 4, top = y + 2, bottom = y + h - 3;
    if (sw < 20 || bottom - top < 8) return;
    double wa = std::log2(1.0 + attackMs(vi.attack));
    double wd = std::log2(1.0 + decayReleaseMs(vi.decay));
    double wr = std::log2(1.0 + decayReleaseMs(vi.release));
    double ws = (wa + wd + wr) / 3.0, total = wa + wd + ws + wr;
    auto xAt = [&](double t) { return x + (int)std::lround(t / total * (sw - 1)); };
    int susY = bottom - (int)std::lround(vi.sustain / 15.0 * (bottom - top));
    int px[5] = {x, xAt(wa), xAt(wa + wd), xAt(wa + wd + ws), x + sw - 1};
    int py[5] = {bottom, top, susY, susY, bottom};
    int active = -1;
    switch (vi.envPhase) {
        case IAudioInspect::EnvPhase::Attack:  active = 0; break;
        case IAudioInspect::EnvPhase::Decay:   active = 1; break;
        case IAudioInspect::EnvPhase::Sustain: active = 2; break;
        case IAudioInspect::EnvPhase::Release: active = 3; break;
        default: break;
    }
    for (int i = 0; i < 4; ++i) {
        uint32_t c = i == active ? colour : COLOUR_DIM;
        cv.line(px[i], py[i], px[i + 1], py[i + 1], c);
        if (i == active) cv.line(px[i], py[i] - 1, px[i + 1], py[i + 1] - 1, c);   // 2 px wide
    }
    int bx = x + w - barW, by = top, bh = bottom - top + 1;
    cv.rect(bx, by, barW, bh, COLOUR_GRID);
    int fillH = (int)std::lround(vi.envLevel / 255.0 * (bh - 2));
    if (fillH > 0) cv.fill(bx + 1, by + bh - 1 - fillH, barW - 2, fillH, colour);
}

} // namespace

Image renderScope(IAudioInspect* insp, const RenderOptions& opt) {
    Image img;
    if (!insp || opt.chip < 0 || opt.chip >= insp->chipCount()) return img;
    const int voices = insp->voiceCount(opt.chip), rows = voices + 1;
    img.width = std::max(160, opt.width);
    int rowH = std::max(40, opt.rowHeight);
    img.height = rows * rowH;
    img.argb.assign((size_t)img.width * img.height, COLOUR_BG);
    Canvas cv(img);

    std::vector<float> buf(insp->scopeCapacity());
    const int rate = insp->scopeSampleRate();
    char line[200];
    for (int ch = 0; ch < rows; ++ch) {
        const bool output = ch == voices;
        const int ry = ch * rowH;
        const uint32_t colour = traceColour(output ? 3 : ch);
        if (ry > 0) cv.line(0, ry, img.width - 1, ry, COLOUR_GRID);

        IAudioInspect::VoiceInfo vi;
        int headerH = LINE_H + 4;
        if (output) {
            cv.text(6, ry + 4, "Output", colour);
        } else {
            insp->voiceInfo(opt.chip, ch, vi);
            headerH = 2 * LINE_H + 4;
            std::snprintf(line, sizeof line, "Voice %d", ch + 1);
            int x = cv.text(6, ry + 4, line, colour) + CHAR_W * 2;
            std::string l1 = controlText(vi.control);
            if (vi.freq) {
                int cents = 0;
                std::string note = noteName(vi.freqHz, &cents);
                std::snprintf(line, sizeof line, "  %.1f Hz", vi.freqHz);
                l1 += line;
                if (!note.empty()) { std::snprintf(line, sizeof line, "  %s %+dc", note.c_str(), cents); l1 += line; }
            }
            cv.text(x, ry + 4, l1, COLOUR_TEXT);
            std::snprintf(line, sizeof line, "PW %.1f%%  ADSR %X %X %X %X  %s %d%s%s", vi.pulseWidth * 100.0 / 4096.0,
                          vi.attack, vi.decay, vi.sustain, vi.release, phaseName((int)vi.envPhase), vi.envLevel,
                          vi.filtered ? "  Filter" : "", vi.muted ? "  Off" : "");
            cv.text(x, ry + 4 + LINE_H, line, COLOUR_TEXT);
        }

        const int envW = output ? 0 : std::min(90, img.width / 4);
        int tx = 6, ty = ry + headerH;
        int tw = img.width - 12 - (envW ? envW + 8 : 0), th = rowH - headerH - 4;
        if (tw < 20 || th < 10) continue;
        if (envW) drawEnvelope(cv, tx + tw + 8, ty, envW, th, vi, colour);

        const int midY = ty + th / 2;
        const double halfH = th / 2.0 - 1;
        cv.rect(tx, ty, tw, th, COLOUR_GRID);
        cv.line(tx, midY, tx + tw - 1, midY, COLOUR_GRID);
        for (int i = 1; i < 10; ++i) {
            int gx = tx + tw * i / 10;
            cv.line(gx, midY - 3, gx, midY + 3, COLOUR_GRID);
        }

        int n = insp->scopeSamples(opt.chip, ch, buf.data(), (int)buf.size());
        TraceWindow win = traceWindow(buf.data(), n, rate, opt.timebaseMs, opt.triggered);
        if (win.untriggered) {
            std::string msg = "no trigger";
            cv.text(tx + tw - 3 - (int)msg.size() * CHAR_W, ty + 3, msg, COLOUR_DIM);
        }
        if (win.length < 2) continue;
        const float* s = buf.data() + win.start;
        auto yOf = [&](float v) {
            v = std::max(-1.0f, std::min(1.0f, v));
            return midY - (int)std::lround(v * halfH);
        };
        if (win.length <= tw) {
            int px = tx, py = yOf(s[0]);
            for (int i = 1; i < win.length; ++i) {
                int x = tx + (int)((int64_t)i * (tw - 1) / (win.length - 1)), y = yOf(s[i]);
                cv.line(px, py, x, y, colour);
                px = x; py = y;
            }
        } else {
            float prev = s[0];
            for (int x = 0; x < tw; ++x) {
                int a = (int)((int64_t)x * win.length / tw);
                int b = std::max(a + 1, (int)((int64_t)(x + 1) * win.length / tw));
                float lo = prev, hi = prev;
                for (int i = a; i < b; ++i) { lo = std::min(lo, s[i]); hi = std::max(hi, s[i]); }
                cv.line(tx + x, yOf(hi), tx + x, yOf(lo), colour);
                prev = s[b - 1];
            }
        }
    }
    return img;
}

} // namespace scope
