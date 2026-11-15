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
constexpr int SHIFTED = 0x100;
constexpr int K_SHIFT = 8 << 3 | 0;
constexpr int K_CAPS = 8 << 3 | 5;
constexpr double KEY_HOLD = 0.06, KEY_GAP = 0.04;      // typed keys: 60 ms down, 40 ms up

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
    m_caps = false;
    m_queue.clear();
    m_typing = -1;
    m_wait = 0;
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
            if (m_shifts > 0) v |= 0x01;
            if (m_caps) v |= 0x20;
        }
    }
    return (uint8_t)~v;
}

void Keyboard::press(int code, bool down) {
    if (code & SHIFTED) m_shifts = std::max(0, m_shifts + (down ? 1 : -1));
    if ((code & 0xFF) == K_CAPS) {             // CAPS LOCK locks: each press toggles it
        if (down) m_caps = !m_caps;
        return;
    }
    set((code >> 3) & 15, code & 7, down);
}

bool Keyboard::key(std::string n, bool down) {
    for (auto& c : n) c = (char)std::tolower((unsigned char)c);
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
        {"right_alt", 8 << 3 | 3}, {"altgr", 8 << 3 | 3}, {"code", 8 << 3 | 3}, {"home", 8 << 3 | 4},
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
    press(code, down);
    return true;
}

void Keyboard::enqueueText(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\\' && i + 1 < text.size() && text[i + 1] == 'n') { c = '\n'; ++i; }
        const int code = charCode(c);
        if (code >= 0) m_queue.push_back(code);
    }
}

void Keyboard::clock(uint64_t ticks, double hz) {
    if (m_typing < 0 && m_queue.empty()) return;
    double t = (double)ticks / hz;
    while (t > 0) {
        if (m_wait > t) { m_wait -= t; return; }
        t -= m_wait;
        m_wait = 0;
        if (m_typing >= 0) {
            press(m_typing, false);
            m_typing = -1;
            m_wait = KEY_GAP;
            continue;
        }
        if (m_queue.empty()) return;
        m_typing = m_queue.front();
        m_queue.pop_front();
        press(m_typing, true);
        m_wait = KEY_HOLD;
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
    if (spec.rfind("printer:", 0) != 0) return false;
    const std::string path = spec.substr(8);
    FILE* f = nullptr;
    if (path != "none" && !path.empty()) {
        f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
    }
    if (m_file) std::fclose(m_file);
    m_file = f;
    return true;
}

void Printer::write(uint8_t b) {
    printed.push_back(b);
    if (m_file) { std::fputc(b, m_file); std::fflush(m_file); }
}

bool SerialPort::attachSerial(const std::string& spec) {
    if (spec.rfind("printer:", 0) == 0) return false;
    const bool ok = m_sys->isT200() ? m_sys->usart.attachSerial(spec) : m_sys->uart.attachSerial(spec);
    if (ok) m_spec = spec;
    return ok;
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
IVideoOutput::VideoDimensions Display::getDimensions() const {
    const int w = m_sys->lcdWidth() * SCALE + 2 * BORDER, h = m_sys->lcdHeight() * SCALE + 2 * BORDER;
    return {w, h, w, h};
}

void Display::renderFrame(uint32_t* buf) {
    const auto d = getDimensions();
    std::fill(buf, buf + (size_t)d.width * d.height, BG);
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
    m_ram.assign(isT200() ? 72 * 1024 : 32 * 1024, 0);
}

System::~System() = default;

void System::setCpu(I8085* cpu) {
    m_cpu = cpu;
    if (!cpu) return;
    cpu->setSodCallback([this](bool v) { tape.output(v ? 63 : 0); });
    cpu->setSidCallback([this]() { return tape.input(); });
}

void System::setRamKb(int kb) {
    if (isT200()) kb = kb <= 24 ? 24 : kb <= 48 ? 48 : 72;
    else kb = std::max(8, std::min(32, (kb + 7) / 8 * 8));
    m_ram.assign((size_t)kb * 1024, 0);
}

void System::setOptionRom(std::vector<uint8_t> rom, const std::string& label) {
    m_opt = std::move(rom);
    if (!m_opt.empty()) m_opt.resize(0x8000, 0xFF);
    m_optLabel = m_opt.empty() ? "" : label;
}

int System::romBank() const { return isT200() ? (m_bankReg & 3) : (m_ctrl & 1); }

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
    uint8_t v = 0x02;                       // the printer: not busy (VirtualT)
    if (isT200()) {
        v |= 0x01;                          // /LPS: the batteries are fine
        SerialEndpoint* ep = usart.endpoint();
        if (ep && ep->dcd()) v |= 0x10;
        return v;
    }
    if (rtc.dataOut()) v |= 0x01;
    SerialEndpoint* ep = uart.endpoint();
    if (!(ep && ep->cts())) v |= 0x10;      // /CTS, /DSR: low when asserted
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
    // 6 +5 V (pulled up, MAME), 7 /LPS (power fine, VirtualT)
    uint8_t v = 0xE0;
    if (uart.oe()) v |= 0x02;
    if (uart.fe()) v |= 0x04;
    if (uart.pe()) v |= 0x08;
    if (uart.tbre()) v |= 0x10;
    return v;
}

// Time ----------------------------------------------------------------------
void System::advance(uint64_t t) {
    m_ticks += t;
    const uint64_t edges = pio.tick(t);
    if (edges) {
        if (isT200()) usart.tick(edges);
        else uart.tick16x(edges);
    }
    if (isT200()) {
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
        rtc.tick(t);
    }
    tape.clock(t);
    keyboard.clock(t, CPU_HZ);
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
uint8_t System::peek8(uint32_t addr) { return access((uint16_t)addr, false, 0); }

uint8_t System::access(uint16_t a, bool write, uint8_t v) {
    if (!isT200()) {
        if (a < 0x8000) {
            if (write) return 0;
            if (m_ctrl & 1) return m_opt.empty() ? 0xFF : m_opt[a];
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
        case 2: return m_opt.empty() ? 0xFF : m_opt[a];
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
    const int blk = port & 0xF0;
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
    const int blk = port & 0xF0;
    if (blk == 0xB0) return pio.readIo(port & 7);
    if (isT200()) {
        if (blk == 0xC0) return usart.read(port & 1);
        if (blk == 0xF0) return lcdc.read(port & 1);
        return peekPort(port);
    }
    if (blk == 0xC0) return uart.readData();
    if (blk == 0xF0) return lcd.read(port & 1);
    return peekPort(port);
}

void System::out(uint8_t port, uint8_t v) {
    const int blk = port & 0xF0;
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
    case 0xA0: if (m_model == Model::M100) m_modem = v; break;
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
    if (!isT200()) lcd.setSelectFromPorts(m_pa, m_pb);
    if (m_cpu) {
        m_cpu->setRst75(m_rst75);
        m_cpu->setRst65(m_rst65);
    }
}

System* systemOf(MachineDescriptor* m) {
    if (!m || m->buses.empty()) return nullptr;
    return dynamic_cast<System*>(m->buses[0].bus);
}

}  // namespace m100
