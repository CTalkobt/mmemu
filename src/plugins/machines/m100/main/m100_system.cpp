// The Tandy Model 100, 102 and 200: memory maps, ports and the wiring
// between the chips, after the Model 100 Technical Reference Manual,
// VirtualT (Stephen Hurd, Ken Pettit; BSD licence: io.c, memory.c) and
// MAME (kyocera.cpp, Curt Coder; BSD-3-Clause). mmsim's own code.

#include "m100_system.h"
#include "plugins/8085/main/cpu8085.h"
#include "plugins/devices/serial_endpoint/main/serial_endpoint.h"
#include "libcore/main/machine_desc.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

namespace m100 {

namespace {

void putBlock(std::vector<uint8_t>& v, const std::vector<uint8_t>& b) {
    const uint32_t n = (uint32_t)b.size();
    for (int i = 0; i < 4; ++i) v.push_back((uint8_t)(n >> (8 * i)));
    v.insert(v.end(), b.begin(), b.end());
}
std::vector<uint8_t> getBlock(const uint8_t*& p) {
    uint32_t n = 0;
    for (int i = 0; i < 4; ++i) n |= (uint32_t)*p++ << (8 * i);
    std::vector<uint8_t> b(p, p + n);
    p += n;
    return b;
}
template <class T> void put(std::vector<uint8_t>& v, const T& x) {
    const auto* p = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), p, p + sizeof(T));
}
template <class T> void get(const uint8_t*& p, T& x) {
    std::memcpy(&x, p, sizeof(T));
    p += sizeof(T);
}

std::string extOf(const std::string& path) {
    const auto dot = path.rfind('.');
    std::string e = dot == std::string::npos ? "" : path.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return e;
}

// Key codes: column << 3 | bit
constexpr int SHIFTED = KEY_SHIFTED;
constexpr int K_SHIFT = 8 << 3 | 0;
constexpr int K_CAPS = 8 << 3 | 5;
// Typed keys when nothing scans the keyboard: SHIFT 20 ms ahead, 60 ms down, 40 ms up
constexpr double KEY_SHIFT = 0.02, KEY_HOLD = 0.06, KEY_GAP = 0.04;

int charCode(char c) {
    static const char* const COLS[6] = {"zxcvbnml", "asdfghjk", "qwertyui", "op[;',./", "12345678", "90-="};
    if (c >= 'A' && c <= 'Z') return charCode((char)(c - 'A' + 'a')) | SHIFTED;
    for (int col = 0; col < 6; ++col)
        for (int bit = 0; COLS[col][bit]; ++bit)
            if (COLS[col][bit] == c) return col << 3 | bit;
    switch (c) {
    case ' ': return 6 << 3 | 0;
    case '\b': return 6 << 3 | 1;
    case '\t': return 6 << 3 | 2;
    case 0x1B: return 6 << 3 | 3;
    case '\n': case '\r': return 6 << 3 | 7;
    case '!': return charCode('1') | SHIFTED; case '@': return charCode('2') | SHIFTED;
    case '#': return charCode('3') | SHIFTED; case '$': return charCode('4') | SHIFTED;
    case '%': return charCode('5') | SHIFTED; case '^': return charCode('6') | SHIFTED;
    case '&': return charCode('7') | SHIFTED; case '*': return charCode('8') | SHIFTED;
    case '(': return charCode('9') | SHIFTED; case ')': return charCode('0') | SHIFTED;
    case '_': return charCode('-') | SHIFTED; case '+': return charCode('=') | SHIFTED;
    case ']': return charCode('[') | SHIFTED; case ':': return charCode(';') | SHIFTED;
    case '"': return charCode('\'') | SHIFTED; case '<': return charCode(',') | SHIFTED;
    case '>': return charCode('.') | SHIFTED; case '?': return charCode('/') | SHIFTED;
    default: return -1;
    }
}

/** The 8085's I/O space as a bus. */
class IoBus : public IBus {
public:
    explicit IoBus(System* s) : m_sys(s) { m_cfg = {8, 8, BusRole::IO_PORT, true, 0xFF}; }
    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "io"; }
    uint8_t read8(uint32_t addr) override { return m_sys->in((uint8_t)addr); }
    void write8(uint32_t addr, uint8_t v) override { m_sys->out((uint8_t)addr, v); }
    uint8_t peek8(uint32_t addr) override { return m_sys->peekPort((uint8_t)addr); }
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}
private:
    System* m_sys;
    BusConfig m_cfg;
};

}  // namespace

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------
void Keyboard::reset() {
    std::memset(m_cols, 0, sizeof m_cols);
    m_shifts = 0;
    m_typedShift = false;
    m_caps = false;
    m_held.clear();
    m_queue.clear();
    m_typing = -1;
    m_phase = Phase::Idle;
    m_elapsed = 0;
    m_phaseScans = 0;
}

void Keyboard::noteRead(uint16_t sel) {
    // Each of the ROMs' scans reads columns 0-7 together ("any key down?":
    // 100/102 $706F, 200 $88FE); nothing else in them does
    if ((sel & 0xFF) == 0x00) {
        ++m_scans;
        ++m_phaseScans;
    }
}

void Keyboard::set(int col, int bit, bool down) {
    if (col < 0 || col > 8 || bit < 0 || bit > 7) return;
    if (down) m_cols[col] |= (uint8_t)(1 << bit);
    else m_cols[col] &= (uint8_t)~(1 << bit);
}

uint8_t Keyboard::read(uint16_t sel) const {
    uint8_t v = 0;
    for (int c = 0; c < 9; ++c) {
        if (sel & (1 << c)) continue;
        v |= m_cols[c];
        if (c == 8) {
            const int sh = m_layout ? m_layout->shift : K_SHIFT, cl = m_layout ? m_layout->caps : K_CAPS;
            if (m_shifts > 0) v |= (uint8_t)(1 << (sh & 7));
            if (m_caps) v |= (uint8_t)(1 << (cl & 7));
        }
    }
    return (uint8_t)~v;
}

void Keyboard::press(int code, bool down) {
    if (code & SHIFTED) m_shifts = std::max(0, m_shifts + (down ? 1 : -1));
    if ((code & 0xFF) == (m_layout ? m_layout->caps : K_CAPS)) {   // CAPS LOCK locks: each press toggles it
        if (down) m_caps = !m_caps;
        return;
    }
    set((code >> 3) & 15, code & 7, down);
}

bool Keyboard::key(std::string n, bool down) {
    for (auto& c : n) c = (char)std::tolower((unsigned char)c);
    if (m_layout) {
        const int code = m_layout->keyCode(n);
        if (code < 0) return false;
        press(code, down);
        return true;
    }
    static const std::map<std::string, int> keys = {
        {"bracket_left", 3 << 3 | 2}, {"lbracket", 3 << 3 | 2}, {"bracket_right", (3 << 3 | 2) | SHIFTED},
        {"rbracket", (3 << 3 | 2) | SHIFTED}, {"semicolon", 3 << 3 | 3}, {"squote", 3 << 3 | 4},
        {"quote", 3 << 3 | 4}, {"apostrophe", 3 << 3 | 4}, {"comma", 3 << 3 | 5}, {"period", 3 << 3 | 6},
        {"slash", 3 << 3 | 7}, {"minus", 5 << 3 | 2}, {"kpeq", 5 << 3 | 3}, {"equal", 5 << 3 | 3},
        {"equals", 5 << 3 | 3},
        {"left", 5 << 3 | 4}, {"right", 5 << 3 | 5}, {"up", 5 << 3 | 6}, {"down", 5 << 3 | 7},
        {"space", 6 << 3 | 0}, {"delete", 6 << 3 | 1}, {"backspace", 6 << 3 | 1}, {"bksp", 6 << 3 | 1},
        {"tab", 6 << 3 | 2}, {"run_stop", 6 << 3 | 3}, {"escape", 6 << 3 | 3}, {"esc", 6 << 3 | 3},
        {"f9", 6 << 3 | 4}, {"paste", 6 << 3 | 4}, {"f10", 6 << 3 | 5}, {"label", 6 << 3 | 5},
        {"f11", 6 << 3 | 6}, {"print", 6 << 3 | 6}, {"return", 6 << 3 | 7}, {"enter", 6 << 3 | 7},
        {"kp_enter", 6 << 3 | 7},
        {"shift", K_SHIFT}, {"left_shift", K_SHIFT}, {"right_shift", K_SHIFT}, {"lshift", K_SHIFT},
        {"rshift", K_SHIFT}, {"control", 8 << 3 | 1}, {"ctrl", 8 << 3 | 1}, {"left_ctrl", 8 << 3 | 1},
        {"right_ctrl", 8 << 3 | 1}, {"alt", 8 << 3 | 2}, {"left_alt", 8 << 3 | 2}, {"graph", 8 << 3 | 2},
        {"right_alt", 8 << 3 | 3}, {"altgr", 8 << 3 | 3}, {"code", 8 << 3 | 3},
        {"restore", 8 << 3 | 3},                // the GUI's Page Up (wx cannot tell right Alt apart)
        {"home", 8 << 3 | 4},
        {"num", 8 << 3 | 4}, {"caps_lock", K_CAPS}, {"caps", K_CAPS}, {"f12", 8 << 3 | 7},
        {"pause", 8 << 3 | 7}, {"break", 8 << 3 | 7},
        // The PC's shifted symbols, as the GUI names them in its unshifted mode
        {"exclaim", (4 << 3 | 0) | SHIFTED}, {"at", (4 << 3 | 1) | SHIFTED}, {"hash", (4 << 3 | 2) | SHIFTED},
        {"dollar", (4 << 3 | 3) | SHIFTED}, {"percent", (4 << 3 | 4) | SHIFTED},
        {"uparrow", (4 << 3 | 5) | SHIFTED}, {"caret", (4 << 3 | 5) | SHIFTED},
        {"ampersand", (4 << 3 | 6) | SHIFTED}, {"asterisk", (4 << 3 | 7) | SHIFTED},
        {"lparen", (5 << 3 | 0) | SHIFTED}, {"rparen", (5 << 3 | 1) | SHIFTED},
        {"leftarrow", (5 << 3 | 2) | SHIFTED}, {"underscore", (5 << 3 | 2) | SHIFTED},
        {"plus", (5 << 3 | 3) | SHIFTED}, {"dquote", (3 << 3 | 4) | SHIFTED}, {"colon", (3 << 3 | 3) | SHIFTED},
        {"less", (3 << 3 | 5) | SHIFTED}, {"greater", (3 << 3 | 6) | SHIFTED},
        {"question", (3 << 3 | 7) | SHIFTED},
    };
    int code = -1;
    if (n.size() == 1 && (std::isalnum((unsigned char)n[0]))) code = charCode(n[0]);
    else if (n.size() == 2 && n[0] == 'f' && n[1] >= '1' && n[1] <= '8') code = 7 << 3 | (n[1] - '1');
    else {
        auto it = keys.find(n);
        if (it == keys.end()) return false;
        code = it->second;
    }
    if (code < 0) return false;
    // The host repeats a held key as more downs: the key just stays down
    // (the ROM repeats it itself) and a shifted symbol's SHIFT counts once
    if (down ? !m_held.insert(code).second : !m_held.erase(code)) return true;
    press(code, down);
    return true;
}

void Keyboard::enqueueText(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\\' && i + 1 < text.size() && text[i + 1] == 'n') { c = '\n'; ++i; }
        const int code = m_layout ? m_layout->charCode(c) : charCode(c);
        if (code >= 0) m_queue.push_back(code);
    }
}

void Keyboard::enter(Phase p) {
    m_phase = p;
    m_elapsed = 0;
    m_phaseScans = 0;
}

void Keyboard::clock(uint64_t ticks, double hz) {
    if (m_phase == Phase::Idle && m_queue.empty()) return;
    m_elapsed += (double)ticks / hz;
    if (m_phase == Phase::Idle) {
        // The ROM's type-ahead buffer is filling (BASIC is busy): wait, up to 2 s
        if (m_aheadCount && m_elapsed < 2.0) {
            const uint8_t n = m_aheadCount();
            if (n >= TYPE_AHEAD_LIMIT && n <= 64) return;
        }
        m_typing = m_queue.front();
        m_queue.pop_front();
        const bool shifted = m_typing & SHIFTED;
        if (shifted != m_typedShift) {
            m_typedShift = shifted;
            m_shifts = std::max(0, m_shifts + (shifted ? 1 : -1));
            enter(shifted ? Phase::ShiftDown : Phase::ShiftUp);
        } else {
            press(m_typing & 0xFF, true);
            enter(Phase::KeyDown);
        }
        return;
    }
    // Each phase: the scans it waits for, its shortest time, and its time
    // when nothing scans the keyboard (the fixed pace)
    uint64_t scans = GAP_SCANS;
    double minT = 0.010, fallback = KEY_GAP;
    if (m_phase == Phase::ShiftDown || m_phase == Phase::ShiftUp) {
        scans = SHIFT_SCANS;
        minT = 0.005;
        fallback = KEY_SHIFT;
    } else if (m_phase == Phase::KeyDown) {
        scans = HOLD_SCANS;
        fallback = KEY_HOLD;
    }
    const bool done = (m_phaseScans >= scans && m_elapsed >= minT) || (m_phaseScans == 0 && m_elapsed >= fallback) ||
                      m_elapsed >= 0.5;
    if (!done) return;
    switch (m_phase) {
    case Phase::ShiftDown:
    case Phase::ShiftUp:
        if (m_typing >= 0) {
            press(m_typing & 0xFF, true);
            enter(Phase::KeyDown);
        } else {
            enter(Phase::Idle);
        }
        break;
    case Phase::KeyDown:
        press(m_typing & 0xFF, false);
        m_typing = -1;
        enter(Phase::KeyUp);
        break;
    case Phase::KeyUp:
        // SHIFT stays down between shifted keys; let go of it at the end
        if (m_queue.empty() && m_typedShift) {
            m_typedShift = false;
            m_shifts = std::max(0, m_shifts - 1);
            enter(Phase::ShiftUp);
        } else {
            enter(Phase::Idle);
        }
        break;
    case Phase::Idle: break;
    }
}

// ---------------------------------------------------------------------------
// Sound: averaged to samples, then a DC blocker (the piezo has no DC)
// ---------------------------------------------------------------------------
void Sound::run(uint64_t ticks, float level) {
    while (ticks) {
        const uint64_t n = std::min<uint64_t>(ticks, (uint64_t)TICKS_PER_SAMPLE - m_acc);
        m_sum += level * (double)n;
        m_acc += n;
        ticks -= n;
        if (m_acc >= (uint64_t)TICKS_PER_SAMPLE) {
            const double x = m_sum / TICKS_PER_SAMPLE;
            m_dc = x - m_lastIn + 0.995 * m_dc;
            m_lastIn = x;
            if (m_samples.size() < 44100) m_samples.push_back((float)m_dc);
            m_sum = 0;
            m_acc = 0;
        }
    }
}

int Sound::pullSamples(float* buffer, int maxSamples) {
    int n = 0;
    while (n < maxSamples && !m_samples.empty()) {
        buffer[n++] = m_samples.front();
        m_samples.pop_front();
    }
    return n;
}

// ---------------------------------------------------------------------------
// Tape, printer, serial port
// ---------------------------------------------------------------------------
bool Tape::mountTape(const std::string& path) {
    return extOf(path) == "wav" && coco::Tape::mountTape(path);
}

bool Tape::saveTapeRecording(const std::string& path) {
    return extOf(path) == "wav" && coco::Tape::saveTapeRecording(path);
}

Printer::~Printer() {
    if (m_file) std::fclose(m_file);
}

bool Printer::attachSerial(const std::string& spec) {
    bool autoLf = true;
    std::string path;
    if (spec.rfind("printer:", 0) == 0) path = spec.substr(8);
    else if (spec.rfind("printer-raw:", 0) == 0) { path = spec.substr(12); autoLf = false; }
    else return false;
    FILE* f = nullptr;
    if (path != "none" && !path.empty()) {
        f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
    }
    if (m_file) std::fclose(m_file);
    m_file = f;
    m_autoLf = autoLf;
    m_lastCr = false;
    return true;
}

void Printer::write(uint8_t b) {
    printed.push_back(b);
    const bool lfAfterCr = m_lastCr && b == 0x0A;
    m_lastCr = b == 0x0D;
    if (!m_file) return;
    if (!m_autoLf) std::fputc(b, m_file);
    else if (!lfAfterCr) {
        std::fputc(b, m_file);
        if (b == 0x0D) std::fputc(0x0A, m_file);     // the printer's auto line feed
    }
    std::fflush(m_file);
}

bool SerialPort::attachSerial(const std::string& spec) {
    if (spec.rfind("printer:", 0) == 0 || spec.rfind("printer-raw:", 0) == 0 || spec.rfind("modem:", 0) == 0 || spec.rfind("barcode:", 0) == 0) return false;
    // The RS-232 connector's far end, behind the RS-232 / modem switch
    auto ep = makeSerialEndpoint(spec);
    if (!ep) return false;
    m_sys->serialMux->rs232 = std::shared_ptr<SerialEndpoint>(std::move(ep));
    if (m_sys->isT200()) m_sys->usart.setEndpoint(m_sys->serialMux);
    else m_sys->uart.setEndpoint(m_sys->serialMux);
    m_spec = spec;
    return true;
}

// The modem and the bar code wand, as the frontends attach them ------------
bool ModemPort::attachSerial(const std::string& spec) {
    if (spec.rfind("modem:", 0) != 0 || !m_sys->hasModem()) return false;
    const std::string arg = spec.substr(6);
    if (arg == "originate" || arg == "answer") {        // the Model 100's ORIG/ANS switch
        if (m_sys->isT200()) return false;
        m_sys->m100Originate = arg == "originate";
        m_sys->modem.setOriginate(m_sys->m100Originate);
        return true;
    }
    return m_sys->modem.attachLine(arg);
}

void ModemPort::getDeviceInfo(DeviceInfo& out) const {
    const Mc14412& m = m_sys->modem;
    out.name = "Modem (MC14412, 300 baud)";
    out.baseAddr = m_sys->isT200() ? 0xB2 : 0xA0;
    out.addrMask = 0;
    out.state.push_back({"Line", m.line() ? m.line()->describe() : "none"});
    out.state.push_back({"Selected", m_sys->modemSelected() ? "modem" : "RS-232"});
    out.state.push_back({"Enabled", m.enabled() ? "yes" : "no"});
    out.state.push_back({"Hook", m.offHook() ? "off hook" : "on hook"});
    out.state.push_back({"Mode", m.originate() ? "originate" : "answer"});
    out.state.push_back({"Carrier", m.carrier() ? "yes" : "no"});
    out.state.push_back({"Dialled", m.dialled().empty() ? "-" : m.dialled()});
}

bool BarcodePort::attachSerial(const std::string& spec) {
    if (spec.rfind("barcode:", 0) != 0) return false;
    const std::string code = spec.substr(8);
    if (code == "none") { m_sys->wandPlugged = false; return true; }
    const auto modules = BarcodeWand::parse(code);
    if (modules.empty()) return false;
    m_sys->wandPlugged = true;
    m_sys->wand.swipe(modules);
    return true;
}

void BarcodePort::getDeviceInfo(DeviceInfo& out) const {
    out.name = "Bar code reader (RST 5.5, port C bit 3)";
    out.baseAddr = 0xB3;
    out.addrMask = 0;
    out.state.push_back({"Wand", m_sys->wandPlugged ? (m_sys->wand.scanning() ? "swiping" : "plugged in")
                                                    : "not plugged in"});
    out.state.push_back({"Swipes", std::to_string(m_sys->wand.swipes())});
}

void SerialPort::getDeviceInfo(DeviceInfo& out) const {
    out.name = m_sys->isT200() ? "RS-232 (8251)" : "RS-232 (IM6402)";
    out.baseAddr = 0xC0;
    out.addrMask = 0x0F;
    SerialEndpoint* ep = m_sys->isT200() ? m_sys->usart.endpoint() : m_sys->uart.endpoint();
    out.state.push_back({"Link", ep ? ep->describe() : "none"});
    char buf[64];
    if (m_sys->isT200()) {
        std::snprintf(buf, sizeof buf, "$%02X (mode $%02X, command $%02X)", m_sys->usart.status(),
                      m_sys->usart.mode(), m_sys->usart.command());
    } else {
        std::snprintf(buf, sizeof buf, "$%02X (control $%02X, %d bits)", m_sys->uart.status(),
                      m_sys->uart.control(), m_sys->uart.wordLength());
    }
    out.state.push_back({"Status", buf});
    const uint16_t n = m_sys->pio.loadedCount();
    if (n) {
        std::snprintf(buf, sizeof buf, "%.0f", CPU_HZ / 16.0 / n);
        out.state.push_back({"Baud (81C55 timer)", buf});
    }
}

// ---------------------------------------------------------------------------
// The LCD: each dot SCALE x SCALE, a BORDER round the panel
// ---------------------------------------------------------------------------
// With a Disk/Video Interface fitted, its monitor goes under the LCD
IVideoOutput::VideoDimensions Display::getDimensions() const {
    int w = m_sys->lcdWidth() * SCALE + 2 * BORDER, h = m_sys->lcdHeight() * SCALE + 2 * BORDER;
    if (m_sys->dvi()) {
        w = std::max(w, Dvi::OUT_W + 2 * BORDER);
        h += Dvi::OUT_H + BORDER;
    }
    return {w, h, w, h};
}

void Display::renderFrame(uint32_t* buf) {
    const auto d = getDimensions();
    std::fill(buf, buf + (size_t)d.width * d.height, BG);
    if (Dvi* dvi = m_sys->dvi()) {
        const int top = m_sys->lcdHeight() * SCALE + 2 * BORDER;
        std::fill(buf + (size_t)top * d.width, buf + (size_t)d.width * d.height, 0xFF202020);
        m_frame.resize((size_t)Dvi::OUT_W * Dvi::OUT_H);
        dvi->render(m_frame.data());
        const int left = (d.width - Dvi::OUT_W) / 2;
        for (int y = 0; y < Dvi::OUT_H; ++y)
            std::copy(&m_frame[(size_t)y * Dvi::OUT_W], &m_frame[(size_t)(y + 1) * Dvi::OUT_W],
                      buf + (size_t)(top + y) * d.width + left);
    }
    const int lw = m_sys->lcdWidth(), lh = m_sys->lcdHeight();
    std::vector<uint8_t> dots;
    if (m_sys->isT200()) dots = m_sys->lcdc.render(lw, lh);
    else dots = m_sys->lcd.render();
    for (int y = 0; y < lh; ++y)
        for (int x = 0; x < lw; ++x) {
            if (!dots[(size_t)y * lw + x]) continue;
            for (int sy = 0; sy < SCALE; ++sy) {
                uint32_t* row = buf + (size_t)(BORDER + y * SCALE + sy) * d.width + BORDER + x * SCALE;
                for (int sx = 0; sx < SCALE; ++sx) row[sx] = FG;
            }
        }
}

void Display::getDeviceInfo(DeviceInfo& out) const {
    char buf[64];
    if (m_sys->isT200()) {
        out.name = "LCD (HD61830, 240 x 128)";
        out.baseAddr = 0xF0;
        out.addrMask = 0x0F;
        std::snprintf(buf, sizeof buf, "$%02X", m_sys->lcdc.mode());
        out.state.push_back({"Mode", buf});
        std::snprintf(buf, sizeof buf, "$%04X", m_sys->lcdc.displayStart());
        out.state.push_back({"Display start", buf});
        std::snprintf(buf, sizeof buf, "$%04X", m_sys->lcdc.cursorAddress());
        out.state.push_back({"Cursor", buf});
    } else {
        out.name = "LCD (10 x HD44102, 240 x 64)";
        out.baseAddr = 0xF0;
        out.addrMask = 0x0F;
        std::snprintf(buf, sizeof buf, "$%03X", m_sys->lcd.chipSelect());
        out.state.push_back({"Chip selects", buf});
    }
}

// ---------------------------------------------------------------------------
// The machine
// ---------------------------------------------------------------------------
System::System(Model model, std::vector<uint8_t> rom) : m_model(model), m_rom(std::move(rom)) {
    m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF};
    m_io = new IoBus(this);
    m_rom.resize(isT200() ? 0x12000 : 0x8000, 0xFF);
    pio.outPa = [this](uint8_t v) { portAOut(v); };
    pio.outPb = [this](uint8_t v) { portBOut(v); };
    pio.inPc = [this]() { return portCIn(); };
    uart.onDr = [this](bool v) { if (!isT200()) setRst65(v); };
    usart.onRxRdy = [this](bool v) { if (isT200()) setRst65(v); };
    rtc.setInputHz(CPU_HZ);
    rtc.onTp = [this](bool v) { if (!isT200()) setRst75(v); };
    rtc.syncToHost();
    rtc200.setInputHz(CPU_HZ);
    rtc200.syncToHost();
    tape.setMasterHz(CPU_HZ);
    keyboard.setLayout(keyLayout(model));
    m_ram.assign(isT200() ? 72 * 1024 : isPc8201() ? 96 * 1024 : 32 * 1024, 0);
    // Recordings at 44.1 kHz: at the CoCo's 22.05 kHz a sample (45 us) of
    // jitter on the 2400 Hz half-cycles (208 us) is enough for CLOAD to misread
    // bits of a CSAVE (checked with the real ROM); 44.1 kHz loads cleanly
    tape.setWavRate(44100);
    // The HD61830's busy flag: MAME's per-instruction clock counts at its
    // 1.2288 MHz (4.9152 MHz / 4, MAME kyocera_v.cpp), two CPU clocks each.
    // The ROM polls it before each access (t200 $8EAB: IN $FF / RAL / JC).
    lcdc.setClockScale(2);
    // The HD44102s' busy time is left at 0: no reference gives it (MAME
    // lists the flag as unsupported, VirtualT never sets it). The 100/102
    // ROM polls it before every access ($750B: IN $FE / RAL / JC), and
    // still draws correctly with a 200-clock busy time.
    // Typing waits while the ROM's type-ahead buffer fills: the ROMs' key
    // repeat starts "LDA count / CPI 2 / RNC" (100/102 $70B0: $FFAA; 200
    // $893F: $FD1E). Found in the ROM, so stand-ins and other ROMs are left alone.
    const size_t mainRom = isT200() ? 0xA000 : 0x8000;
    for (size_t i = 0; i + 6 <= mainRom; ++i) {
        if (m_rom[i] == 0x3A && m_rom[i + 3] == 0xFE && m_rom[i + 4] == 0x02 && m_rom[i + 5] == 0xD0 &&
            m_rom[i + 2] >= 0xA0) {
            const uint16_t count = (uint16_t)(m_rom[i + 1] | m_rom[i + 2] << 8);
            keyboard.setTypeAheadCounter([this, count]() { return peek8(count); });
            break;
        }
    }
    serialMux->modem = &modem;
    uart.setEndpoint(serialMux);
    usart.setEndpoint(serialMux);
}

System::~System() = default;

// The add-ons ----------------------------------------------------------------
bool System::fitRampac(const std::string& path, std::string& err) {
    auto r = std::make_unique<Rampac>();
    if (!r->open(path)) { err = "can't open or create '" + path + "'"; return false; }
    m_rampac = std::move(r);
    return true;
}

void System::removeRampac() { m_rampac.reset(); }      // (its destructor writes the file)

bool System::fitDvi(std::vector<uint8_t> rom, std::vector<uint8_t> cg, std::string& err) {
    if (isPc8201() || m_model == Model::M10 || m_model == Model::KC85) {
        err = "the Disk/Video Interface plugs into the Model 100, 102 and 200";
        return false;
    }
    m_rampac.reset();                                   // one system bus
    m_dvi = std::make_unique<Dvi>(std::move(rom), std::move(cg));
    m_dvi->setHostClock(CPU_HZ);
    return true;
}

void System::removeDvi() { m_dvi.reset(); }

bool System::fitRex(Rex::Kind kind, const std::string& path, std::string& err) {
    auto r = std::make_unique<Rex>(kind);
    if (!r->open(path)) { err = "can't open or create '" + path + "'"; return false; }
    m_rex = std::move(r);
    return true;
}

void System::removeRex() { m_rex.reset(); }

void System::flushAddons() {
    if (m_rampac) m_rampac->flush();
    if (m_dvi) m_dvi->flush();
    if (m_rex) m_rex->flush();
}

void System::modemLines() {
    serialMux->modemSelected = modemSelected();
    switch (m_model) {
    case Model::M100:
        // The ROM (TELCOM's dialler, $5359 and $540A in the Model 100 ROM):
        // $A0 bit 0 connects the modem to the line (MAME / VirtualT's relay)
        // and, with the modem selected, port B bit 7 holds the line (0) or
        // opens it (1): the dial pulses. $A0 bit 1 (MAME's EN) is only pulsed
        // for a few microseconds as the line is seized, then set to send
        // the carrier once the far end's tone is heard ($52F9-$530B): it is
        // the transmitter's enable. The ORIG/ANS switch picks the mode.
        modem.setOriginate(m100Originate);
        modem.setEnable(m_modem & 0x02);
        modem.setOffHook((m_modem & 0x01) && (m_pb & 0x08) && !(m_pb & 0x80));
        break;
    case Model::M200:                       // port B: 1 ORIG/ANS, 6 MEN, 7 CALL (MAME i8155_pb_w)
        modem.setOriginate(m_pb & 0x02);
        modem.setEnable(m_pb & 0x40);
        modem.setOffHook(m_pb & 0x80);
        break;
    default:
        modem.setEnable(false);
        modem.setOffHook(false);
        break;
    }
}

uint8_t System::optionRead(uint16_t a, bool peek) {
    if (m_rex) return peek ? m_rex->peek(a) : m_rex->read(a, m_ticks);
    return m_opt.empty() ? 0xFF : m_opt[a];
}

void System::setCpu(I8085* cpu) {
    m_cpu = cpu;
    if (!cpu) return;
    if (isPc8201()) {
        // The PC-8201A's modulator makes the tones from SOD; its comparator isn't inverted
        cpu->setSodCallback([this](bool v) { m_sod = v; });
        cpu->setSidCallback([this]() { return !tape.input(); });
        return;
    }
    cpu->setSodCallback([this](bool v) { tape.output(v ? 63 : 0); });
    cpu->setSidCallback([this]() { return tape.input(); });
}

void System::setRamKb(int kb) {
    if (isT200()) kb = kb <= 24 ? 24 : kb <= 48 ? 48 : 72;
    else if (isPc8201()) kb = kb <= 16 ? 16 : kb <= 32 ? 32 : kb <= 64 ? 64 : 96;
    else kb = std::max(8, std::min(32, (kb + 7) / 8 * 8));
    m_ram.assign((size_t)kb * 1024, 0);
}

void System::setOptionRom(std::vector<uint8_t> rom, const std::string& label) {
    m_opt = std::move(rom);
    if (!m_opt.empty()) m_opt.resize(0x8000, 0xFF);
    m_optLabel = m_opt.empty() ? "" : label;
}

int System::romBank() const { return isT200() || isPc8201() ? (m_bankReg & 3) : (m_ctrl & 1); }

bool System::pixel(int x, int y) const {
    if (!isT200()) return lcd.pixel(x, y);
    if (x < 0 || x >= 240 || y < 0 || y >= 128) return false;
    return lcdc.render(240, 128)[(size_t)y * 240 + x] != 0;
}

bool System::speaker() const {
    // Port B bit 2 (/MC, the 200's /BUZZER) high: the speaker follows bit 5
    // (BELL); low: TIMER OUT, while bit 5 is set (MAME i8155_pb_w / to_w)
    if (m_pb & 0x04) return m_pb & 0x20;
    return (m_pb & 0x20) && pio.timerOut();
}

void System::setRst75(bool level) {
    m_rst75 = level;
    if (m_cpu) m_cpu->setRst75(level);
}

void System::setRst65(bool level) {
    m_rst65 = level;
    if (m_cpu) m_cpu->setRst65(level);
}

// The 81C55's ports ---------------------------------------------------------
void System::portAOut(uint8_t v) {
    m_pa = v;
    m_keyLatch = (uint16_t)((m_keyLatch & 0x100) | v);
    if (isT200()) return;
    lcd.setSelectFromPorts(m_pa, m_pb);
    // The uPD1990AC: C0-C2, DATA IN, then CLK (its rising edge shifts)
    rtc.setPins(v & 7, v & 0x10, v & 0x08);
}

void System::portBOut(uint8_t v) {
    const uint8_t old = m_pb;
    m_pb = v;
    m_keyLatch = (uint16_t)((m_keyLatch & 0xFF) | ((v & 1) << 8));
    if (!isT200()) lcd.setSelectFromPorts(m_pa, m_pb);
    modemLines();
    // Bit 4: the auto power-off (100/102) or power cut (200)
    if ((v & 0x10) && !(old & 0x10)) powerDown();
    if (!isT200()) {
        SerialEndpoint* ep = uart.endpoint();
        if (ep && !(v & 0x08)) {            // RS-232 selected: /DTR, /RTS
            ep->setDtr(!(v & 0x40));
            ep->setRts(!(v & 0x80));
        }
    }
}

uint8_t System::portCIn() {
    // Bit 1 /BUSY high, bit 2 BUSY low: a ready printer, the state the ROMs
    // wait for before a strobe (100/102 $6D47: IN $BB / ANI 6 / XRI 2 / JNZ;
    // MAME kc85 i8155_pc_r and VirtualT agree on the bits)
    uint8_t v = 0x02;
    if (wandData()) v |= 0x08;              // the bar code reader
    if (isT200()) {
        v |= 0x01;                          // /LPS: the batteries are fine
        SerialEndpoint* ep = usart.endpoint();
        // CD: with the modem selected, the line's signal (the ROM times its
        // edges to detect the carrier, $876B in the Tandy 200 ROM, as the 100
        // does on $D0 bit 0); otherwise the RS-232 DCD
        if (modemSelected() ? modem.lineSignal() : (ep && ep->dcd())) v |= 0x10;
        return v;
    }
    if (rtc.dataOut()) v |= 0x01;
    SerialEndpoint* ep = uart.endpoint();
    // /CTS, /DSR: low when asserted, high with nothing connected (MAME's
    // RS-232 lines are active low, idle 1; VirtualT sets these bits when the
    // host's CTS / DSR are off). The ROM's own RS-232 output paces itself by
    // TBRE and XON/XOFF, not these; bit 4 is read by the modem routines
    // ($6EF6, $52E4)
    if (modemSelected()) {
        // With the modem selected the ROM reads bit 4 as ORIG/ANS (0
        // originate: listen for 2225 Hz, then send; 1 answer: send, then
        // listen for 1270 Hz; TELCOM $52E4): the side switch (unverified wiring)
        if (!m100Originate) v |= 0x10;
    } else if (!(ep && ep->cts())) {
        v |= 0x10;                          // /CTS, /DSR: low when asserted
    }
    if (!(ep && ep->dsr())) v |= 0x20;
    return v;
}

void System::controlOut(uint8_t v) {
    const uint8_t old = m_ctrl;
    m_ctrl = v;
    if (isT200()) {
        if ((old & 0x01) && !(v & 0x01)) printer.write(m_pa);
        tape.setMotor(v & 0x02);
        return;
    }
    if ((old & 0x02) && !(v & 0x02)) printer.write(m_pa);
    rtc.setStb(v & 0x04);
    tape.setMotor(v & 0x08);
}

uint8_t System::uartStatus() const {
    // 0 CD (the modem's: no carrier), 1 OE, 2 FE, 3 PE, 4 TBRE, 5 RP and
    // 6 +5 V (MAME's annotations: RP pulled up, bit 6 tied to +5 V; VirtualT
    // reads them 0), 7 /LPS (power fine; both agree). The ROMs read only
    // bits 0-4 here ($6DBE, $6E40); bit 0 only by counting its toggles to
    // find a modem carrier ($6ED6, $6F0A), so its idle level does not matter
    uint8_t v = 0xE0;
    if (modem.lineSignal()) v |= 0x01;      // the line's signal (the ROM detects the carrier tone itself)
    if (modem.ring()) v &= (uint8_t)~0x20;  // RP: the ring line, pulled up (low while ringing: unverified)
    if (uart.oe()) v |= 0x02;
    if (uart.fe()) v |= 0x04;
    if (uart.pe()) v |= 0x08;
    if (uart.tbre()) v |= 0x10;
    return v;
}

// Time ----------------------------------------------------------------------
void System::advance(uint64_t t) {
    m_ticks += t;
    // TIMER OUT clocks the UART at 16x the bit rate. On the 200 this is the
    // 8251's TxC/RxC: its ROM sets the 8251 to x16 ($4A, $8440) and loads the
    // 81C55 timer from a table of 153600 / baud counts ($86CC: 2048 for 75
    // baud ... 8 for 19200), as the 100's does for the IM6402. The 8251's
    // /CTS is left asserted: how the 200 wires it is not in the references.
    const uint64_t edges = pio.tick(t);
    if (edges) {
        if (isT200()) usart.tick(edges);
        else uart.tick16x(edges);
    }
    if (isT200()) {
        lcdc.tick(t);
        rtc200.tick(t);
        // RST 7.5: 4.9152 MHz / 2 / 8192, a 300 Hz toggle (MAME tandy200_tp_tick)
        m_tpAcc += t;
        while (m_tpAcc >= 8192) {
            m_tpAcc -= 8192;
            setRst75(!m_rst75);
        }
        m_frameAcc += t;
        const uint64_t frame = (uint64_t)(CPU_HZ / 80);    // the panel's 80 Hz refresh
        while (m_frameAcc >= frame) {
            m_frameAcc -= frame;
            lcdc.frame();
        }
    } else {
        lcd.tick(t);
        rtc.tick(t);
        if (isPc8201()) cmtClock(t);
    }
    tape.clock(t);
    if (m_dvi) m_dvi->advance(t);
    keyboard.clock(t, CPU_HZ);
    if (hasModem()) modem.tick(t);
    if (wandPlugged) wand.tick(t);
    if (wandRst55() != m_rst55) {
        m_rst55 = wandRst55();
        if (m_cpu) m_cpu->setRst55(m_rst55);
    }
    m_flushAcc += t;
    if (m_flushAcc >= (uint64_t)CPU_HZ) {   // the battery-backed images: to their files once a second
        m_flushAcc = 0;
        flushAddons();
    }
    sound.run(t, m_powered && speaker() ? 0.3f : 0.0f);
    if (m_holdup) {
        if (m_holdup > t) m_holdup -= t;
        else { m_holdup = 0; powerDown(); }
    }
}

int System::step() {
    int t = 64;
    if (m_powered && m_cpu) t = m_cpu->step();
    if (t <= 0) t = 1;
    advance((uint64_t)t);
    return t;
}

void System::hardwareReset() {
    pio.reset();
    m_pa = 0xFF;
    m_pb = 0x00;                            // (inputs: nothing driven; no power-off edge pending)
    m_keyLatch = 0x1FF;
    m_ctrl = 0;
    m_bankReg = 0;
    m_modem = 0;
    m_scp = 0;
    m_casRomSel = false;
    modemLines();
    uart.masterReset();
    usart.reset();
    tape.setMotor(false);
    m_holdup = 0;
}

void System::powerOn() {
    std::fill(m_ram.begin(), m_ram.end(), 0);
    keyboard.reset();
    lcd.reset();
    lcdc.reset();
    hardwareReset();
    m_powered = true;
    m_switchOff = false;
    if (m_cpu) {
        m_cpu->setTrap(false);
        m_cpu->reset();
    }
}

void System::resetButton() {
    hardwareReset();
    m_powered = true;
    if (m_cpu) m_cpu->reset();
}

void System::powerSwitch() {
    if (m_powered) {
        if (m_switchOff) return;
        // Off: TRAP, and the supply holds up for 50 ms (MAME POWER_HOLDUP)
        m_switchOff = true;
        if (m_cpu) m_cpu->setTrap(true);
        m_holdup = (uint64_t)(CPU_HZ * 0.05);
        return;
    }
    // On: the 8085 restarts; RAM was kept by its battery
    m_switchOff = false;
    if (m_cpu) m_cpu->setTrap(false);
    hardwareReset();
    m_powered = true;
    if (m_cpu) m_cpu->reset();
}

void System::powerDown() {
    m_powered = false;
    m_holdup = 0;
    m_switchOff = false;
    tape.setMotor(false);
    if (m_cpu) m_cpu->setTrap(false);
}

// Memory --------------------------------------------------------------------
uint8_t System::read8(uint32_t addr) { return access((uint16_t)addr, false, 0); }
void System::write8(uint32_t addr, uint8_t v) { access((uint16_t)addr, true, v); }
uint8_t System::peek8(uint32_t addr) { return access((uint16_t)addr, false, 0, true); }

uint8_t System::access(uint16_t a, bool write, uint8_t v, bool peek) {
    if (isPc8201()) return accessPc8201(a, write, v);
    if (!isT200()) {
        if (a < 0x8000) {
            if (write) return 0;
            if (m_ctrl & 1) return optionRead(a, peek);
            return m_rom[a];
        }
        const size_t base = 0x10000 - m_ram.size();
        if (a < base) return 0xFF;
        uint8_t& r = m_ram[a - base];
        if (write) r = v;
        return r;
    }
    if (a < 0x8000) {
        if (write) return 0;
        switch (m_bankReg & 3) {
        case 0: return m_rom[a];
        case 1: return m_rom[0xA000 + a];
        case 2: return optionRead(a, peek);
        default: return 0xFF;
        }
    }
    if (a < 0xA000) return write ? 0 : m_rom[a];
    const int bank = ramBank();
    const size_t off = (size_t)bank * 0x6000 + (a - 0xA000u);
    if (bank == 3 || off >= m_ram.size()) return 0xFF;
    if (write) m_ram[off] = v;
    return m_ram[off];
}

// Ports ---------------------------------------------------------------------
uint8_t System::peekPort(uint8_t port) {
    if (m_dvi && (port & 0xF0) == 0x80) return m_dvi->hostPeek(port);
    if (m_rampac && Rampac::decodes(port)) return m_rampac->peek(port);
    const int blk = port & 0xF0;
    if (uint8_t r; isPc8201() && inPc8201(port, r)) return r;
    if (blk == 0xB0) return pio.peekIo(port & 7);
    if (blk == 0xE0) return keyboard.read(m_keyLatch);
    if (isT200()) {
        switch (blk) {
        case 0x90: return rtc200.read(port & 0x0F);
        case 0xC0: return usart.peek(port & 1);
        case 0xD0: return m_bankReg;
        case 0xF0: return lcdc.peek(port & 1);
        default: return 0xFF;
        }
    }
    switch (blk) {
    case 0xC0: return uart.readRbr();
    case 0xD0: return uartStatus();
    case 0xF0: return (port & 1) ? 0xFF : lcd.read(0);
    default: return 0xFF;
    }
}

uint8_t System::in(uint8_t port) {
    if (m_dvi && (port & 0xF0) == 0x80) return m_dvi->hostIn(port);
    if (m_rampac && Rampac::decodes(port)) return m_rampac->in(port);
    const int blk = port & 0xF0;
    if (blk == 0xB0) return pio.readIo(port & 7);
    if (isT200()) {
        if (blk == 0xC0) return usart.read(port & 1);
        if (blk == 0xF0) return lcdc.read(port & 1);
        if (blk == 0xE0) keyboard.noteRead(m_keyLatch);
        return peekPort(port);
    }
    if (blk == 0xC0) return uart.readData();
    if (blk == 0xF0) return lcd.read(port & 1);
    if (blk == 0xE0) keyboard.noteRead(m_keyLatch);
    return peekPort(port);
}

void System::out(uint8_t port, uint8_t v) {
    if (m_dvi && (port & 0xF0) == 0x80) { m_dvi->hostOut(port, v); return; }
    if (m_rampac && Rampac::decodes(port)) { m_rampac->out(port, v); return; }
    const int blk = port & 0xF0;
    if (isPc8201() && outPc8201(port, v)) return;
    if (blk == 0xB0) { pio.writeIo(port & 7, v); return; }
    if (blk == 0xE0) { controlOut(v); return; }
    if (isT200()) {
        switch (blk) {
        case 0x90: rtc200.write(port & 0x0F, v); break;
        case 0xC0: usart.write(port & 1, v); break;
        case 0xD0: m_bankReg = v & 0x0F; break;
        case 0xF0: lcdc.write(port & 1, v); break;
        default: break;
        }
        return;
    }
    switch (blk) {
    case 0xA0: if (m_model == Model::M100) { m_modem = v; modemLines(); } break;
    case 0xC0: uart.writeTbr(v); break;
    case 0xD0: uart.writeControl(v & 0x1F); break;
    case 0xF0: lcd.write(port & 1, v); break;
    default: break;
    }
}

// Snapshots -----------------------------------------------------------------
size_t System::stateSize() const { return state().size(); }

void System::saveState(uint8_t* buf) const {
    const auto v = state();
    std::memcpy(buf, v.data(), v.size());
}

std::vector<uint8_t> System::state() const {
    std::vector<uint8_t> v;
    putBlock(v, m_ram);
    put(v, m_pa); put(v, m_pb); put(v, m_keyLatch); put(v, m_ctrl); put(v, m_bankReg); put(v, m_modem);
    put(v, m_rst75); put(v, m_rst65); put(v, m_tpAcc); put(v, m_frameAcc);
    put(v, m_powered); put(v, m_switchOff); put(v, m_holdup); put(v, m_ticks);
    putBlock(v, pio.saveState());
    putBlock(v, lcd.saveState());
    putBlock(v, lcdc.saveState());
    putBlock(v, uart.saveState());
    putBlock(v, usart.saveState());
    putBlock(v, rtc.saveState());
    putBlock(v, rtc200.saveState());
    if (isPc8201()) putPc8201(v);
    putBlock(v, addonState());
    return v;
}

void System::loadState(const uint8_t* buf) {
    const uint8_t* p = buf;
    auto ram = getBlock(p);
    if (ram.size() == m_ram.size()) m_ram = std::move(ram);
    get(p, m_pa); get(p, m_pb); get(p, m_keyLatch); get(p, m_ctrl); get(p, m_bankReg); get(p, m_modem);
    get(p, m_rst75); get(p, m_rst65); get(p, m_tpAcc); get(p, m_frameAcc);
    get(p, m_powered); get(p, m_switchOff); get(p, m_holdup); get(p, m_ticks);
    pio.loadState(getBlock(p));
    lcd.loadState(getBlock(p));
    lcdc.loadState(getBlock(p));
    uart.loadState(getBlock(p));
    usart.loadState(getBlock(p));
    rtc.loadState(getBlock(p));
    rtc200.loadState(getBlock(p));
    if (isPc8201()) getPc8201(p);
    loadAddonState(p);
    if (!isT200()) lcd.setSelectFromPorts(m_pa, m_pb);
    modemLines();
    if (m_cpu) {
        m_cpu->setRst75(m_rst75);
        m_cpu->setRst65(m_rst65);
    }
}

// The add-ons' state: the modem, the wand, and the RAMPAC's and REX's
// memories (each block empty when the device is not fitted)
std::vector<uint8_t> System::addonState() const {
    std::vector<uint8_t> v;
    putBlock(v, modem.saveState());
    put(v, wandPlugged);
    putBlock(v, m_rampac ? m_rampac->saveState() : std::vector<uint8_t>{});
    putBlock(v, m_rex ? m_rex->saveState() : std::vector<uint8_t>{});
    putBlock(v, m_dvi ? m_dvi->saveState() : std::vector<uint8_t>{});
    return v;
}

void System::loadAddonState(const uint8_t*& p) {
    const auto blk = getBlock(p);
    if (blk.size() < 4) return;
    const uint8_t* q = blk.data();
    modem.loadState(getBlock(q));
    get(q, wandPlugged);
    const auto r = getBlock(q);
    if (m_rampac && !r.empty()) m_rampac->loadState(r);
    const auto x = getBlock(q);
    if (m_rex && !x.empty()) m_rex->loadState(x);
    if (q < blk.data() + blk.size()) {                  // (older snapshots end here)
        const auto dv = getBlock(q);
        if (m_dvi && !dv.empty()) m_dvi->loadState(dv);
    }
}

System* systemOf(MachineDescriptor* m) {
    if (!m || m->buses.empty()) return nullptr;
    return dynamic_cast<System*>(m->buses[0].bus);
}

}  // namespace m100
