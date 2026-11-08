// Acorn Electron, after Elkulator (GPL-2.0-or-later): ula.c (video,
// interrupts, sound, cassette, the RAM contention), mem.c (the memory map,
// paged ROM, the keyboard read through ROM slots 8 and 9).

#include "machine_electron.h"
#include "bbc_tape.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/io_handler.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdebug/main/execution_observer.h"
#include "libmem/main/ibus.h"
#include "plugins/6502/main/cpu6502.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <vector>

namespace electron {

namespace {

constexpr uint32_t CPU_HZ = 2000000;
constexpr int SAMPLE_RATE = 44100;

bool loadFile(const std::string& path, size_t size, std::vector<uint8_t>& out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (f) out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (!f || out.size() != size) {
        if (err) *err = "missing or wrong-size ROM " + path;
        return false;
    }
    return true;
}

// The ULA's interrupt status bits (&FE00)
constexpr uint8_t INT_MASTER = 0x01, INT_POWER = 0x02, INT_VBL = 0x04, INT_RTC = 0x08;
constexpr uint8_t INT_RECEIVE = 0x10, INT_SEND = 0x20, INT_HIGHTONE = 0x40;
constexpr int BYTE_CYCLES = 16667;          // a cassette byte: 10 bits at 1200 baud

// ---------------------------------------------------------------------------
// The keyboard: 14 columns, each on an address line (A0-A13, low to select),
// 4 rows on data bits 0-3. A key is column | row << 4.
// ---------------------------------------------------------------------------
constexpr uint16_t SHIFTED = 0x100;
constexpr int K_SHIFT = 13 | 3 << 4, K_CTRL = 13 | 2 << 4, K_FUNC = 13 | 1 << 4, K_ESCAPE = 13;
constexpr int K_RETURN = 1 | 2 << 4, K_DELETE = 1 | 3 << 4, K_SPACE = 0 | 3 << 4;

// Letters and digits by column (Elkulator mem.c keys[][])
int letterKey(char c) {
    static const char* const cols[14] = {nullptr, nullptr, nullptr, "0P", "9OL", "8IK", "7UJM",
                                         "6YHN", "5TGB", "4RFV", "3EDC", "2WSX", "1QAZ", nullptr};
    c = (char)std::toupper((unsigned char)c);
    for (int col = 3; col <= 12; col++) {
        if (const char* p = std::strchr(cols[col], c)) return col | (int)(p - cols[col]) << 4;
    }
    return -1;
}

class ElkKeyboard : public IOHandler, public IKeyboardMatrix {
public:
    std::function<void()> onBreak;
    std::function<bool()> capsLockOn;

    const char* name() const override { return "Keyboard"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override { std::memset(m_down, 0, sizeof(m_down)); m_queue.clear(); m_typing = 0; m_wait = 0; }

    void set(int code, bool d) { m_down[code & 15][(code >> 4) & 3] = d; }

    /** A read of &8000-&BFFF with the keyboard paged in: the rows of every
     *  column whose address line is low */
    uint8_t read(uint16_t addr) const {
        uint8_t v = 0;
        for (int col = 0; col < 14; col++)
            if (!(addr & (1 << col)))
                for (int row = 0; row < 4; row++)
                    if (m_down[col][row]) v |= (uint8_t)(1 << row);
        return v;
    }

    // Typed text: a key (with Shift if needed) for 40 ms, then up for 30 ms
    void tick(uint64_t cycles) override {
        while (cycles) {
            if (m_wait > cycles) { m_wait -= cycles; return; }
            cycles -= m_wait;
            m_wait = 0;
            if (m_typing) {
                set(m_typing & 0xFF, false);
                if (m_typing & SHIFTED) set(K_SHIFT, false);
                m_typing = 0;
                m_wait = 60000;
                continue;
            }
            if (m_queue.empty()) return;
            const uint8_t ch = m_queue.front();
            uint16_t k;
            if (std::isalpha(ch) && std::islower(ch) && capsOn()) {
                k = K_FUNC | SHIFTED;                    // SHIFT+CAPS LK: Caps Lock off first
            } else {
                m_queue.pop_front();
                const int c = codeFor(ch);
                if (c < 0) continue;
                k = (uint16_t)c;
            }
            if (k & SHIFTED) set(K_SHIFT, true);
            set(k & 0xFF, true);
            m_typing = k;
            m_wait = 80000;
        }
    }

    bool key(std::string n, bool d) {
        for (auto& c : n) c = (char)std::tolower((unsigned char)c);
        if (n == "break" || n == "f12") { if (d && onBreak) onBreak(); return true; }
        // f1-f10: FUNC with the digit (f10: FUNC 0)
        if (n.size() >= 2 && n[0] == 'f' && std::isdigit((unsigned char)n[1])) {
            const int f = std::atoi(n.c_str() + 1);
            if (f >= 1 && f <= 10) {
                set(K_FUNC, d);
                set(letterKey((char)('0' + f % 10)), d);
                return true;
            }
        }
        static const std::map<std::string, int> keys = {
            {"right", 0}, {"end", 0 | 1 << 4}, {"home", 0 | 1 << 4}, {"copy", 0 | 1 << 4}, {"space", K_SPACE},
            {"left", 1}, {"down", 1 | 1 << 4}, {"return", K_RETURN}, {"enter", K_RETURN},
            {"delete", K_DELETE}, {"backspace", K_DELETE},
            {"minus", 2}, {"up", 2 | 1 << 4}, {"squote", 2 | 2 << 4}, {"colon", 2 | 2 << 4},
            {"semicolon", 3 | 2 << 4}, {"slash", 3 | 3 << 4}, {"period", 4 | 3 << 4}, {"comma", 5 | 3 << 4},
            {"run_stop", K_ESCAPE}, {"escape", K_ESCAPE}, {"esc", K_ESCAPE},
            {"alt", K_FUNC}, {"caps_lock", K_FUNC}, {"func", K_FUNC}, {"tab", K_FUNC},
            {"control", K_CTRL}, {"ctrl", K_CTRL},
            {"shift", K_SHIFT}, {"left_shift", K_SHIFT}, {"right_shift", K_SHIFT},
        };
        if (auto it = keys.find(n); it != keys.end()) { set(it->second, d); return true; }
        if (n.size() == 1 && std::isalnum((unsigned char)n[0])) { set(letterKey(n[0]), d); return true; }
        return false;
    }

    // IKeyboardMatrix
    void keyDown(int col, int row) override { set(col | row << 4, true); }
    void keyUp(int col, int row) override { set(col | row << 4, false); }
    void clearKeys() override { reset(); }
    bool pressKeyByName(const std::string& n, bool d) override { return key(n, d); }
    IPortDevice* getPort(int) override { return nullptr; }
    void enqueueText(const std::string& text) override {
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (c == '\\' && i + 1 < text.size() && text[i + 1] == 'n') { c = '\r'; ++i; }
            m_queue.push_back((uint8_t)c);
        }
    }

private:
    bool capsOn() const { return capsLockOn ? capsLockOn() : true; }
    int codeFor(uint8_t c) const {
        if (c == '\r' || c == '\n') return K_RETURN;
        if (c == 0x1B) return K_ESCAPE;
        if (c == ' ') return K_SPACE;
        if (std::isalpha(c)) {
            int k = letterKey((char)c);
            if (std::isupper(c) && !capsOn()) k |= SHIFTED;
            return k;
        }
        if (std::isdigit(c)) return letterKey((char)c);
        static const std::map<char, int> sym = {
            {'!', '1'}, {'"', '2'}, {'#', '3'}, {'$', '4'}, {'%', '5'}, {'&', '6'}, {'\'', '7'},
            {'(', '8'}, {')', '9'}, {'@', '0'}};
        if (auto it = sym.find((char)c); it != sym.end()) return letterKey((char)it->second) | SHIFTED;
        switch (c) {
            case '-': return 2;
            case '=': return 2 | SHIFTED;
            case ':': return 2 | 2 << 4;
            case '*': return 2 | 2 << 4 | SHIFTED;
            case ';': return 3 | 2 << 4;
            case '+': return 3 | 2 << 4 | SHIFTED;
            case '/': return 3 | 3 << 4;
            case '?': return 3 | 3 << 4 | SHIFTED;
            case '.': return 4 | 3 << 4;
            case '>': return 4 | 3 << 4 | SHIFTED;
            case ',': return 5 | 3 << 4;
            case '<': return 5 | 3 << 4 | SHIFTED;
        }
        return -1;
    }

    bool m_down[14][4] = {};
    std::deque<uint8_t> m_queue;
    uint16_t m_typing = 0;
    uint64_t m_wait = 0;
};

// ---------------------------------------------------------------------------
// The ULA (Elkulator ula.c)
// ---------------------------------------------------------------------------
class ElkUla : public IOHandler, public IVideoOutput, public IAudioOutput {
public:
    static constexpr int WIDTH = 640, HEIGHT = 512;

    ElkUla(const uint8_t* ram, bbc::UefTape* tape) : m_ram(ram), m_tape(tape) {
        m_canvas.assign((size_t)WIDTH * HEIGHT, 0xFF000000);
        m_frame = m_canvas;
        // ulalookup: bits 1, 3, 5, 7 of a byte to a 4-bit colour
        for (int c = 0; c < 256; c++)
            m_lookup[c] = (uint8_t)(((c & 2) ? 1 : 0) | ((c & 8) ? 2 : 0) | ((c & 0x20) ? 4 : 0) | ((c & 0x80) ? 8 : 0));
        tape->onCarrier = [this]() { if (!m_tapeOut) raise(INT_HIGHTONE); };
        tape->onByte = [this](uint8_t v) {
            if (m_tapeOut) return;
            m_tapeLatch = v;
            m_isr &= (uint8_t)~INT_HIGHTONE;
            raise(INT_RECEIVE);
        };
        reset();
    }

    const char* name() const override { return "ULA"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void tick(uint64_t) override {}
    void reset() override {
        // resetula: the power-on bit set, Mode 6
        m_x = m_y = m_sc = 0;
        m_dispon = false;
        m_oddEven = 0;
        m_isr = INT_POWER;
        m_ier = 0;
        m_mode = 6;
        m_motor = m_tapeOut = false;
        m_soundOn = false;
        m_sendLeft = 0;
        m_misc = 0;
        m_romBank = m_intRomBank = 0;
        m_extRom = false;
        update();
    }

    bool irq() const { return m_isr & INT_MASTER; }
    uint8_t isr() const { return m_isr; }
    bool capsLed() const { return m_misc & 0x80; }

    // Paged ROM (&FE05): 8-11 internal (8/9 the keyboard, 10/11 BASIC);
    // 12-15 external; 0-7 external only after 12-15 (the ULA's quirk)
    int romBank() const { return m_romBank; }
    bool externalRom() const { return m_extRom; }
    bool basicPaged() const { return !m_extRom && (m_intRomBank & 2); }

    /** &FE00-&FEFF (mirrored every 16) */
    uint8_t read(uint16_t addr) {
        switch (addr & 0xF) {
            case 0: {                                    // interrupt status
                const uint8_t v = (uint8_t)(m_isr | 0x80);
                m_isr &= (uint8_t)~INT_POWER;
                return v;
            }
            case 4:                                      // cassette data
                m_isr &= (uint8_t)~INT_RECEIVE;
                update();
                return m_tapeLatch;
        }
        return 0xFE;
    }

    void write(uint16_t addr, uint8_t v) {
        switch (addr & 0xF) {
            case 0: m_ier = v; update(); break;          // interrupt enable
            case 2: m_addrLo = v & 0xE0; break;          // screen start
            case 3: m_addrHi = v & 0x3F; break;
            case 4:                                      // cassette data out
                m_isr &= (uint8_t)~INT_SEND;
                update();
                m_sendLeft = BYTE_CYCLES;
                m_tape->transmitted(v);
                break;
            case 5:                                      // paging, interrupt clear
                if (v & 0x10) m_isr &= (uint8_t)~INT_VBL;
                if (v & 0x20) m_isr &= (uint8_t)~INT_RTC;
                if (v & 0x40) m_isr &= (uint8_t)~INT_HIGHTONE;
                update();
                m_romBank = v & 0xF;
                if (m_romBank >= 0xC) m_extRom = true;
                if ((m_romBank & 0xC) == 8) { m_extRom = false; m_intRomBank = m_romBank; }
                break;
            case 6:                                      // sound counter
                m_soundDiv = v;
                break;
            case 7:                                      // misc control
                m_misc = v;
                m_soundOn = (v & 6) == 2;
                m_mode = (v >> 3) & 7;
                m_motor = v & 0x40;
                m_tapeOut = (v & 6) == 4;
                m_tape->setMotor(m_motor, m_tapeOut);
                break;
            default:                                     // &FE08-&FE0F: the palette
                writePalette(addr & 0xF, v);
                break;
        }
    }

    /** Cycles a RAM access at cycle `at` waits (Elkulator waitforramsync):
     *  Modes 0-3 hold the CPU off the whole visible line; otherwise RAM runs
     *  at 1 MHz (the next even cycle) */
    int ramWait(uint64_t at) const {
        if (m_dispon && m_x < 640 && !(m_mode & 4)) return (640 - m_x) / 8;
        return (at & 1) ? 1 : 0;
    }

    /** Run `cycles` 2 MHz cycles: a character cell (8 pixels) a cycle, 128 a line */
    void clock(uint64_t cycles) {
        while (cycles--) {
            if (m_x < 640) {
                if (m_dispon) drawCell();
                else if (m_y < 256) cell(0);
            }
            m_x += 8;
            if (m_x == 1024) endLine();
            sound();
            if (m_sendLeft > 0 && --m_sendLeft == 0) raise(INT_SEND);
        }
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
    uint64_t frames() const { return m_frames; }

private:
    static constexpr uint32_t COLOURS[8] = {0xFF000000, 0xFFFF0000, 0xFF00FF00, 0xFFFFFF00,
                                            0xFF0000FF, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF};
    static constexpr int MODE_ROWS[8] = {8, 8, 8, 10, 8, 8, 10, 10};
    static constexpr uint16_t MODE_LEN[8] = {0x5000, 0x5000, 0x5000, 0x4000, 0x2800, 0x2800, 0x2000, 0x2000};
    static constexpr int MODE_END[8] = {256, 256, 256, 250, 256, 256, 250, 250};

    void raise(uint8_t bit) { m_isr |= bit; update(); }
    void update() {
        if (m_isr & m_ier & 0x7C) m_isr |= INT_MASTER;
        else m_isr &= (uint8_t)~INT_MASTER;
    }

    // The palette (&FE08-&FE0F): each register pair sets the red, green and
    // blue of some logical colours, a bit clear meaning on (Elkulator)
    void writePalette(int reg, uint8_t v) {
        static const int GROUPS[4][4] = {{0, 2, 8, 10}, {4, 6, 12, 14}, {5, 7, 13, 15}, {1, 3, 9, 11}};
        const int* g = GROUPS[(reg >> 1) & 3];
        if (!(reg & 1)) {                                // even: blue for all four, green for two
            m_pal[g[0]] &= ~4; m_pal[g[1]] &= ~4; m_pal[g[2]] &= ~6; m_pal[g[3]] &= ~6;
            if (!(v & 0x10)) m_pal[g[0]] |= 4;
            if (!(v & 0x20)) m_pal[g[1]] |= 4;
            if (!(v & 0x40)) m_pal[g[2]] |= 4;
            if (!(v & 0x80)) m_pal[g[3]] |= 4;
            if (!(v & 0x04)) m_pal[g[2]] |= 2;
            if (!(v & 0x08)) m_pal[g[3]] |= 2;
        } else {                                         // odd: red for all four, green for two
            m_pal[g[0]] &= ~3; m_pal[g[1]] &= ~3; m_pal[g[2]] &= ~1; m_pal[g[3]] &= ~1;
            if (!(v & 0x01)) m_pal[g[0]] |= 1;
            if (!(v & 0x02)) m_pal[g[1]] |= 1;
            if (!(v & 0x04)) m_pal[g[2]] |= 1;
            if (!(v & 0x08)) m_pal[g[3]] |= 1;
            if (!(v & 0x10)) m_pal[g[0]] |= 2;
            if (!(v & 0x20)) m_pal[g[1]] |= 2;
        }
    }

    void pixel(int x, uint32_t c) {
        if (x < 0 || x >= WIDTH || m_y >= 256) return;
        m_canvas[(size_t)(2 * m_y) * WIDTH + x] = c;
        m_canvas[(size_t)(2 * m_y + 1) * WIDTH + x] = c;
    }
    void cell(uint32_t c) { for (int i = 0; i < 8; i++) pixel(m_x + i, c | 0xFF000000); }

    void drawCell() {
        if (m_sc & 8) { cell(0); return; }               // Modes 3 and 6: the gap lines
        if ((m_x & 8) && (m_mode & 4)) return;           // Modes 4-6: drawn 16 at a time
        uint16_t a = (m_mode & 4) ? (uint16_t)(m_addr + m_sc + ((m_x >> 1) & ~7))
                                  : (uint16_t)(m_addr + m_sc + (m_x & ~7));
        if (a & 0x8000) a = (uint16_t)(a - MODE_LEN[m_mode]);
        uint8_t t = m_ram[a & 0x7FFF];
        auto col = [&](uint8_t bits) { return COLOURS[m_pal[m_lookup[bits]] & 7]; };
        switch (m_mode) {
            case 0: case 3:
                for (int i = 0; i < 8; i++, t <<= 1) pixel(m_x + i, col(t & 0x80));
                break;
            case 1:
                for (int i = 0; i < 8; i += 2, t <<= 1) { const uint32_t c = col(t & 0x88); pixel(m_x + i, c); pixel(m_x + i + 1, c); }
                break;
            case 2:
                for (int i = 0; i < 8; i += 4, t <<= 1) { const uint32_t c = col(t); for (int j = 0; j < 4; j++) pixel(m_x + i + j, c); }
                break;
            case 4: case 6: case 7:
                for (int i = 0; i < 16; i += 2, t <<= 1) { const uint32_t c = col(t & 0x80); pixel(m_x + i, c); pixel(m_x + i + 1, c); }
                break;
            case 5:
                for (int i = 0; i < 16; i += 4, t <<= 1) { const uint32_t c = col(t & 0x88); for (int j = 0; j < 4; j++) pixel(m_x + i + j, c); }
                break;
        }
    }

    void endLine() {
        m_x = 0;
        m_y++;
        if (m_dispon) {
            if (++m_sc == MODE_ROWS[m_mode]) {
                m_sc = 0;
                m_addr = (uint16_t)(m_addr + ((m_mode & 4) ? 320 : 640));
                if (m_addr & 0x8000) m_addr = (uint16_t)(m_addr - MODE_LEN[m_mode]);
                m_addrBack = m_addr;
            }
            m_addr = m_addrBack;
        }
        if (m_y == 99) raise(INT_RTC);
        if (m_y == MODE_END[m_mode]) { m_dispon = false; raise(INT_VBL); }
        if (m_y == 258) m_dispon = false;
        if (m_y == (m_oddEven ? 313 : 312) || m_y >= 313) {
            // A new frame: the screen start from &FE02/&FE03
            m_frame = m_canvas;
            ++m_frames;
            m_y = 0;
            m_oddEven ^= 1;
            m_dispon = true;
            m_sc = 0;
            m_addr = (uint16_t)((m_addrLo << 1) | (m_addrHi << 9));
            if (!m_addr) m_addr = (uint16_t)(0x8000 - MODE_LEN[m_mode]);
            m_addrBack = m_addr;
        }
    }

    // The square wave: a 1 MHz / 16 counter, the output flipping every
    // (S + 1) counts (&FE06); mixed down to 44.1 kHz
    void sound() {
        if (++m_soundTick == 32) {
            m_soundTick = 0;
            if (++m_soundCount > m_soundDiv) { m_soundCount = 0; m_soundLevel = !m_soundLevel; }
        }
        m_acc += (m_soundOn && !m_motor) ? (m_soundLevel ? 0.25 : -0.25) : 0.0;
        ++m_accN;
        m_phase += 1.0;
        if (m_phase >= (double)CPU_HZ / SAMPLE_RATE) {
            m_phase -= (double)CPU_HZ / SAMPLE_RATE;
            m_samples.push_back((float)(m_acc / m_accN));
            if (m_samples.size() > SAMPLE_RATE) m_samples.pop_front();
            m_acc = 0;
            m_accN = 0;
        }
    }

    const uint8_t* m_ram;
    bbc::UefTape* m_tape;
    std::vector<uint32_t> m_canvas, m_frame;
    uint8_t m_lookup[256] = {};
    uint8_t m_pal[16] = {};
    uint8_t m_isr = INT_POWER, m_ier = 0, m_addrLo = 0, m_addrHi = 0, m_tapeLatch = 0, m_misc = 0;
    int m_romBank = 0, m_intRomBank = 0;            // power-on: the keyboard is paged in
    bool m_extRom = false;
    int m_x = 0, m_y = 0, m_sc = 0, m_mode = 6, m_oddEven = 0;
    bool m_dispon = false;
    uint16_t m_addr = 0, m_addrBack = 0;
    bool m_motor = false, m_tapeOut = false, m_soundOn = false;
    int m_sendLeft = 0;
    int m_soundTick = 0, m_soundCount = 0, m_soundDiv = 0;
    bool m_soundLevel = false;
    double m_acc = 0, m_phase = 0;
    int m_accN = 0;
    std::deque<float> m_samples;
    uint64_t m_frames = 0;
};

constexpr uint32_t ElkUla::COLOURS[8];
constexpr int ElkUla::MODE_ROWS[8];
constexpr uint16_t ElkUla::MODE_LEN[8];
constexpr int ElkUla::MODE_END[8];

// ---------------------------------------------------------------------------
// The bus (Elkulator mem.c)
// ---------------------------------------------------------------------------
class ElkBus : public IBus {
public:
    std::function<void(bool)> onIrq;

    ElkBus(std::vector<uint8_t> os, std::vector<uint8_t> basic, ElkKeyboard* kbd, bbc::UefTape* tape)
        : m_os(std::move(os)), m_basic(std::move(basic)), m_kbd(kbd), m_tape(tape) {
        m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF};
        m_ram.assign(0x8000, 0);
        m_ula = new ElkUla(m_ram.data(), tape);
        kbd->capsLockOn = [this]() { return m_ula->capsLed(); };
    }
    ~ElkBus() override { delete m_ula; }

    ElkUla* ula() { return m_ula; }

    void powerOn() {
        std::fill(m_ram.begin(), m_ram.end(), 0);
        m_cycles = m_devCycle = 0;
        m_ula->reset();
    }

    void begin() { m_access = 0; m_stretch = 0; }
    int end(int c) {
        const int total = c + m_stretch;
        m_cycles += (uint64_t)total;
        sync(m_cycles);
        return total;
    }

    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "system"; }

    uint8_t read8(uint32_t addr) override {
        const uint16_t a = addr & 0xFFFF;
        uint8_t v;
        if (a < 0x8000) { ramWait(); v = m_ram[a]; }
        else if (a >= 0xFE00 && a < 0xFF00) { syncNow(); v = m_ula->read(a); irqUpdate(); }
        else v = peekMem(a);
        ++m_access;
        if (m_observer) m_observer->onMemoryRead(this, a, v);
        return v;
    }
    uint8_t peek8(uint32_t addr) override {
        const uint16_t a = addr & 0xFFFF;
        if (a >= 0xFE00 && a < 0xFF00) return 0xFE;
        return peekMem(a);
    }
    void write8(uint32_t addr, uint8_t v) override {
        const uint16_t a = addr & 0xFFFF;
        if (m_observer) m_observer->onMemoryWrite(this, a, peek8(a), v);
        if (a < 0x8000) { ramWait(); m_ram[a] = v; }
        else if (a >= 0xFE00 && a < 0xFF00) { syncNow(); m_ula->write(a, v); irqUpdate(); }
        ++m_access;
    }
    void reset() override {}
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

private:
    uint8_t peekMem(uint16_t a) const {
        if (a < 0x8000) return m_ram[a];
        if (a < 0xC000) {
            if (!m_ula->externalRom()) return m_ula->basicPaged() ? m_basic[a & 0x3FFF] : m_kbd->read(a);
            return (uint8_t)(a >> 8);                    // no cartridge or expansion ROM
        }
        if (a < 0xFC00 || a >= 0xFF00) return m_os[a & 0x3FFF];
        if (a < 0xFE00) return (uint8_t)(a >> 8);        // FRED, JIM: nothing fitted
        return 0xFE;
    }
    uint64_t now() const { return m_cycles + (uint64_t)m_access + (uint64_t)m_stretch; }
    void syncNow() { sync(now()); }
    // The CPU waits for the ULA to give it the RAM
    void ramWait() {
        syncNow();
        const int w = m_ula->ramWait(now());
        if (w > 0) { m_stretch += w; syncNow(); }
    }
    void sync(uint64_t to) {
        if (to <= m_devCycle) return;
        const uint64_t d = to - m_devCycle;
        m_devCycle = to;
        m_ula->clock(d);
        m_tape->clock(d);
        irqUpdate();
    }
    void irqUpdate() { if (onIrq) onIrq(m_ula->irq()); }

    BusConfig m_cfg;
    std::vector<uint8_t> m_os, m_basic, m_ram;
    ElkKeyboard* m_kbd;
    bbc::UefTape* m_tape;
    ElkUla* m_ula;
    uint64_t m_cycles = 0, m_devCycle = 0;
    int m_access = 0, m_stretch = 0;
};

// The ULA as the frontends find it (video and sound); the bus owns it
class ElkUlaHandle : public IOHandler, public IVideoOutput, public IAudioOutput {
public:
    explicit ElkUlaHandle(ElkUla* u) : m_u(u) {}
    const char* name() const override { return "Video"; }
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
    ElkUla* m_u;
};

}  // namespace

MachineDescriptor* create(std::string* err) {
    std::vector<uint8_t> os, basic;
    if (!loadFile("roms/electron/os100.rom", 0x4000, os, err)) return nullptr;
    if (!loadFile("roms/electron/basic2.rom", 0x4000, basic, err)) return nullptr;

    auto* desc = new MachineDescriptor();
    desc->machineId = "electron";
    desc->displayName = "Acorn Electron";
    desc->licenseClass = "proprietary";
    desc->uiPanesSet = true;
    desc->uiPanes = {"tape"};
    desc->clockHz = CPU_HZ;

    auto* kbd = new ElkKeyboard();
    auto* tape = new bbc::UefTape();
    auto* bus = new ElkBus(std::move(os), std::move(basic), kbd, tape);
    auto* video = new ElkUlaHandle(bus->ula());
    desc->buses.push_back({"system", bus});
    auto* cpu = new MOS6502();
    cpu->setDataBus(bus);
    cpu->setCodeBus(bus);
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});
    bus->onIrq = [cpu](bool on) { cpu->setIrqLine(on); };

    auto* io = new IORegistry();
    desc->ioRegistry = io;
    io->registerHandler(video);
    io->registerHandler(kbd);
    io->registerHandler(tape);
    desc->deleters.push_back([video, kbd, tape]() { delete video; delete kbd; delete tape; });

    // BREAK resets the 6502 only (Elkulator); the ULA's power-on bit tells the OS
    kbd->onBreak = [cpu]() { cpu->reset(); };
    desc->onKey = [kbd](const std::string& n, bool down) { return kbd->key(n, down); };
    desc->screenButtons = {{"BREAK", {"break"}, "BREAK: the 6502 restarts (a soft reset; the program in memory stays)."}};

    desc->onReset = [bus, io, cpu](MachineDescriptor&) {     // power on
        bus->powerOn();
        io->resetAll();
        cpu->reset();
    };
    desc->schedulerStep = [bus, cpu, io](MachineDescriptor&) -> int {
        bus->begin();
        const int c = bus->end(cpu->step());
        io->tickAll((uint64_t)c);
        return c;
    };
    if (desc->onReset) desc->onReset(*desc);
    return desc;
}

}  // namespace electron
