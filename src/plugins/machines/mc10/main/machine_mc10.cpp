// TRS-80 MC-10 Micro Color Computer and Matra Alice. The board's wiring
// after XRoar (mc10.c; GPL-3.0-or-later) and MAME (mame/trs/mc10.cpp), and
// the MC-10 Service Manual; mmsim's own code.

#include "mc10_system.h"
#include "plugins/6801/main/cpu6801.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdebug/main/execution_observer.h"
#include "util/path_util.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mc10 {

namespace {

constexpr uint64_t KEY_HOLD = 859091;      // typed keys: 60 ms down
constexpr uint64_t KEY_GAP = 572727;       // then 40 ms up

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

template <class T> void put(std::vector<uint8_t>& v, const T& x) {
    const auto* p = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), p, p + sizeof(T));
}
void putBlob(std::vector<uint8_t>& v, const std::vector<uint8_t>& b) {
    put(v, (uint32_t)b.size());
    v.insert(v.end(), b.begin(), b.end());
}
template <class T> void get(const uint8_t*& p, T& x) {
    std::memcpy(&x, p, sizeof(T));
    p += sizeof(T);
}
std::vector<uint8_t> getBlob(const uint8_t*& p) {
    uint32_t n = 0;
    get(p, n);
    std::vector<uint8_t> b(p, p + n);
    p += n;
    return b;
}

// Key codes: row << 3 | column, with the chord's SHIFT or CONTROL
constexpr int SHIFTED = 0x100, CTRLED = 0x200;
constexpr int K_ENTER = 3 << 3 | 6, K_SPACE = 3 << 3 | 7;
constexpr int K_CTRL = 6 << 3 | 0, K_BREAK = 6 << 3 | 2, K_SHIFT = 6 << 3 | 7;
// The arrows by position (the same keys on the Alice): CONTROL with these
constexpr int P_LEFT = 0 << 3 | 1, P_RIGHT = 2 << 3 | 3, P_UP = 2 << 3 | 7, P_DOWN = 3 << 3 | 2, P_LDEL = 2 << 3 | 1;

// Where the MC-10 has a key legend (no SHIFT): -1 if none
int mc10Pos(char c) {
    if (c >= 'A' && c <= 'Z') return 1 + (c - 'A');           // A row 0 col 1 ... Z row 3 col 2
    if (c >= '0' && c <= '9') return 32 + (c - '0');           // 0-7 row 4, 8 9 row 5
    switch (c) {
        case '@': return 0;
        case ':': return 42;
        case ';': return 43;
        case ',': return 44;
        case '-': return 45;
        case '.': return 46;
        case '/': return 47;
        default: return -1;
    }
}

// A key legend's position on the MC-10 or the Alice (AZERTY: A/Q, Z/W
// swapped; M, ; and / moved round)
int keyPos(char c, bool alice) {
    c = (char)std::toupper((unsigned char)c);
    if (alice) {
        static const std::map<char, char> MOVED = {
            {'A', 'Q'}, {'Q', 'A'}, {'Z', 'W'}, {'W', 'Z'}, {'M', ';'}, {';', '/'}, {'/', 'M'},
        };
        const auto it = MOVED.find(c);
        if (it != MOVED.end()) c = it->second;
    }
    return mc10Pos(c);
}

// A character as a key chord, or -1
int charCode(char c, bool alice) {
    if (c == '\n' || c == '\r') return K_ENTER;
    if (c == ' ') return K_SPACE;
    if (std::isalpha((unsigned char)c)) return keyPos(c, alice);
    static const char* SHIFT_DIGITS = "!\"#$%&'()";
    if (const char* p = c ? std::strchr(SHIFT_DIGITS, c) : nullptr) return keyPos((char)('1' + (p - SHIFT_DIGITS)), alice) | SHIFTED;
    switch (c) {
        case '*': return keyPos(':', alice) | SHIFTED;
        case '+': return keyPos(';', alice) | SHIFTED;
        case '<': return keyPos(',', alice) | SHIFTED;
        case '=': return keyPos('-', alice) | SHIFTED;
        case '>': return keyPos('.', alice) | SHIFTED;
        case '?': return keyPos('/', alice) | SHIFTED;
        case 0x08: return P_LEFT | CTRLED;
        case 0x03: return K_BREAK;
        default: return keyPos(c, alice);
    }
}

}  // namespace

std::string firmwareDir() {
    const char* v = std::getenv("MMEMU_MC10_FIRMWARE");
    return v ? v : PathUtil::findResource("roms/mc10");
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------
void Keyboard::reset() {
    std::memset(m_matrix, 0, sizeof(m_matrix));
    m_shifts = m_ctrls = 0;
    m_queue.clear();
    m_typing = -1;
    m_wait = 0;
}

void Keyboard::set(int row, int col, bool down) {
    if (row < 0 || row > 6 || col < 0 || col > 7) return;
    if (down) m_matrix[col] |= (uint8_t)(1 << row);
    else m_matrix[col] &= (uint8_t)~(1 << row);
}

uint8_t Keyboard::column(int col) const {
    uint8_t v = m_matrix[col & 7];
    if ((col & 7) == (K_SHIFT & 7) && m_shifts > 0) v |= 0x40;
    if ((col & 7) == (K_CTRL & 7) && m_ctrls > 0) v |= 0x40;
    return v;
}

void Keyboard::press(int code, bool down) {
    if (code & SHIFTED) m_shifts = std::max(0, m_shifts + (down ? 1 : -1));
    if (code & CTRLED) m_ctrls = std::max(0, m_ctrls + (down ? 1 : -1));
    set((code >> 3) & 7, code & 7, down);
}

bool Keyboard::key(std::string n, bool down) {
    for (auto& c : n) c = (char)std::tolower((unsigned char)c);
    static const std::map<std::string, int> keys = {
        {"return", K_ENTER}, {"enter", K_ENTER}, {"space", K_SPACE},
        {"break", K_BREAK}, {"escape", K_BREAK}, {"esc", K_BREAK}, {"run_stop", K_BREAK},
        {"left_shift", K_SHIFT}, {"right_shift", K_SHIFT}, {"shift", K_SHIFT},
        {"control", K_CTRL}, {"ctrl", K_CTRL}, {"left_ctrl", K_CTRL}, {"right_ctrl", K_CTRL},
        {"left", P_LEFT | CTRLED}, {"backspace", P_LEFT | CTRLED}, {"delete", P_LEFT | CTRLED},
        {"right", P_RIGHT | CTRLED}, {"up", P_UP | CTRLED}, {"down", P_DOWN | CTRLED},
        {"home", P_LDEL | CTRLED},
    };
    // Symbols by the GUI's names: the MC-10 key with that legend
    static const std::map<std::string, char> symbols = {
        {"at", '@'}, {"colon", ':'}, {"semicolon", ';'}, {"comma", ','}, {"minus", '-'}, {"period", '.'},
        {"slash", '/'}, {"exclaim", '!'}, {"dquote", '"'}, {"hash", '#'}, {"dollar", '$'}, {"percent", '%'},
        {"ampersand", '&'}, {"squote", '\''}, {"lparen", '('}, {"rparen", ')'}, {"asterisk", '*'},
        {"plus", '+'}, {"less", '<'}, {"equal", '='}, {"equals", '='}, {"kpeq", '='}, {"greater", '>'},
        {"question", '?'},
    };
    int code = -1;
    if (n.size() == 1 && (std::isalnum((unsigned char)n[0]) || mc10Pos(n[0]) >= 0)) {
        code = charCode(n[0], m_alice);
    } else if (auto it = keys.find(n); it != keys.end()) {
        code = it->second;
    } else if (auto s = symbols.find(n); s != symbols.end()) {
        code = charCode(s->second, m_alice);
    }
    if (code < 0) return false;
    press(code, down);
    return true;
}

void Keyboard::enqueueText(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\\' && i + 1 < text.size() && text[i + 1] == 'n') { c = '\n'; ++i; }
        const int code = charCode(c, m_alice);
        if (code >= 0) m_queue.push_back(code);
    }
}

void Keyboard::clock(uint64_t ticks) {
    while (ticks) {
        if (m_wait > ticks) { m_wait -= ticks; return; }
        ticks -= m_wait;
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
// The serial port's output
// ---------------------------------------------------------------------------
SerialPort::~SerialPort() { closeLink(); }

void SerialPort::closeLink() {
    if (m_file) std::fclose(m_file);
    m_file = nullptr;
    if (m_clientFd >= 0) ::close(m_clientFd);
    if (m_listenFd >= 0) ::close(m_listenFd);
    m_clientFd = m_listenFd = -1;
    m_loopback = false;
}

bool SerialPort::attachSerial(const std::string& spec) {
    std::string path;
    if (spec.rfind("file:", 0) == 0) path = spec.substr(5);
    else if (spec.rfind("printer:", 0) == 0) path = spec.substr(8);
    if (!path.empty()) {
        FILE* f = std::fopen(path.c_str(), "ab");
        if (!f) return false;
        closeLink();
        m_file = f;
        return true;
    }
    if (spec == "none") { closeLink(); return true; }
    if (spec == "loopback") { closeLink(); m_loopback = true; return true; }
    if (spec.rfind("tcp:", 0) == 0) {
        const int port = std::atoi(spec.c_str() + 4);
        if (port <= 0 || port > 65535) return false;
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd, (sockaddr*)&a, sizeof a) < 0 || ::listen(fd, 1) < 0) { ::close(fd); return false; }
        ::fcntl(fd, F_SETFL, O_NONBLOCK);
        closeLink();
        m_listenFd = fd;
        return true;
    }
    return false;
}

void SerialPort::pollTcp() {
    if (m_clientFd < 0 && m_listenFd >= 0) {
        const int c = ::accept(m_listenFd, nullptr, nullptr);
        if (c >= 0) { ::fcntl(c, F_SETFL, O_NONBLOCK); m_clientFd = c; }
    }
    if (m_clientFd < 0 || m_rxQueue.size() > 256) return;
    uint8_t buf[64];
    const ssize_t n = ::recv(m_clientFd, buf, sizeof buf, 0);
    if (n > 0) m_rxQueue.insert(m_rxQueue.end(), buf, buf + n);
    else if (n == 0) { ::close(m_clientFd); m_clientFd = -1; }
}

void SerialPort::clock(uint64_t ticks) {
    if (m_listenFd >= 0) {
        if (m_pollLeft > ticks) m_pollLeft -= ticks;
        else { m_pollLeft = (uint64_t)(m_hz / 1000); pollTcp(); }
    }
    const uint64_t bit = (uint64_t)(m_hz / m_baud);
    while (ticks) {
        if (m_rxBit < 0) {
            if (m_rxQueue.empty()) return;
            m_rxByte = m_rxQueue.front();
            m_rxQueue.pop_front();
            m_rxBit = 0;                     // the start bit
            m_rxLeft = bit;
        }
        if (m_rxLeft > ticks) { m_rxLeft -= ticks; return; }
        ticks -= m_rxLeft;
        if (++m_rxBit > 9) m_rxBit = -1;     // after the stop bit: idle (or the next byte)
        m_rxLeft = bit;
    }
}

bool SerialPort::setOption(const std::string& key, const std::string& value, std::string& err) {
    if (key == "tp10") { m_tp10 = value == "1" || value == "on" || value == "true"; return true; }
    if (key != "baud") { err = "no such option (baud, tp10)"; return false; }
    const int b = std::atoi(value.c_str());
    if (b < 50 || b > 115200) { err = "baud: 50-115200"; return false; }
    m_baud = b;
    return true;
}

void SerialPort::getDeviceInfo(DeviceInfo& out) const {
    IOHandler::getDeviceInfo(out);
    out.state.push_back({"baud", std::to_string(m_baud)});
    out.state.push_back({"device", m_tp10 ? "TP-10 thermal printer" : "serial printer / RS-232"});
    out.state.push_back({"link", m_file ? "file" : m_loopback ? "loopback" : m_clientFd >= 0 ? "tcp (connected)" : m_listenFd >= 0 ? "tcp (listening)" : "none"});
    out.state.push_back({"bytes", std::to_string(sent.size())});
}

void SerialPort::txLevel(uint64_t now, bool level) {
    decode(now);
    if (level == m_level) return;
    m_level = level;
    if (!m_inFrame) {
        if (!level) {                        // a start bit
            m_inFrame = true;
            m_frameStart = now;
            m_edges.clear();
        }
        return;
    }
    m_edges.emplace_back(now, level);
}

// A frame is done 9.5 bit times after its start bit: the middle of each data bit
void SerialPort::decode(uint64_t now) {
    if (!m_inFrame) return;
    const double bit = m_hz / m_baud;
    if ((double)(now - m_frameStart) < bit * 9.5) return;
    auto levelAt = [&](uint64_t t) {
        bool l = false;
        for (const auto& e : m_edges) {
            if (e.first > t) break;
            l = e.second;
        }
        return l;
    };
    uint8_t v = 0;
    for (int i = 0; i < 8; ++i)
        if (levelAt(m_frameStart + (uint64_t)(bit * (1.5 + i)))) v |= (uint8_t)(1 << i);
    m_inFrame = false;
    deliver(v);
    // Another start bit already under way after this frame's stop bit
    const uint64_t stop = m_frameStart + (uint64_t)(bit * 9.0);
    bool l = levelAt(stop);
    for (const auto& e : m_edges) {
        if (e.first <= stop) continue;
        if (!e.second && l) {
            m_inFrame = true;
            m_frameStart = e.first;
            std::vector<std::pair<uint64_t, bool>> rest;
            for (const auto& r : m_edges) if (r.first > e.first) rest.push_back(r);
            m_edges = rest;
            decode(now);
            return;
        }
        l = e.second;
    }
}

void SerialPort::deliver(uint8_t v) {
    sent.push_back(v);
    std::string out;
    if (m_tp10) {
        // The TP-10's paper: 32 columns; CR feeds a line
        const uint8_t c = v & 0x7F;
        if (c == 0x0D) { out = "\n"; m_column = 0; }
        else if (c >= 0x20) {
            out += (char)c;
            if (++m_column >= 32) { out += "\n"; m_column = 0; }
        }
        paper += out;
    } else {
        out.assign(1, (char)v);
    }
    if (m_file && !out.empty()) {
        std::fwrite(out.data(), 1, out.size(), m_file);
        std::fflush(m_file);
    }
    if (m_loopback) m_rxQueue.push_back(v);
    if (m_clientFd >= 0 && ::send(m_clientFd, &v, 1, MSG_NOSIGNAL) < 0) { ::close(m_clientFd); m_clientFd = -1; }
}

// ---------------------------------------------------------------------------
// Sound, display, tape, expansion port
// ---------------------------------------------------------------------------
void Sound::run(uint64_t ticks, float level) {
    while (ticks) {
        const uint64_t s = std::min<uint64_t>(ticks, TICKS_PER_SAMPLE - m_acc);
        m_sum += (double)level * (double)s;
        m_acc += s;
        ticks -= s;
        if (m_acc == TICKS_PER_SAMPLE) {
            m_samples.push_back((float)(m_sum / TICKS_PER_SAMPLE));
            if (m_samples.size() > 44100) m_samples.pop_front();
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

void Display::renderFrame(uint32_t* buffer) {
    if (m_useEf) { m_ef->render(buffer); return; }
    std::memcpy(buffer, m_vdg->frame(), sizeof(uint32_t) * Mc6847::OUT_W * Mc6847::OUT_H);
}

bool TapeDeck::mountTape(const std::string& path) {
    if (!m_sys->tape.mountTape(path)) return false;
    m_sys->tape.controlTape("stop");          // in, paused: no motor control
    return true;
}
void TapeDeck::controlTape(const std::string& op) { m_sys->tape.controlTape(op); }
bool TapeDeck::startTapeRecord() { return m_sys->tape.startTapeRecord(); }
void TapeDeck::stopTapeRecord() { m_sys->tape.stopTapeRecord(); }
bool TapeDeck::saveTapeRecording(const std::string& path) { return m_sys->tape.saveTapeRecording(path); }
bool TapeDeck::isTapeRecording() const { return m_sys->tape.isTapeRecording(); }

void ExpansionPort::getDeviceInfo(DeviceInfo& out) const {
    IOHandler::getDeviceInfo(out);
    out.state.push_back({"device", m_sys->cart() ? m_sys->cart()->describe() : "none"});
}

// ---------------------------------------------------------------------------
// The system
// ---------------------------------------------------------------------------
System::System(Model model, std::vector<uint8_t> rom) : vdg(false), m_model(model) {
    m_ef = hasEf9345(model);
    m_masterHz = model == Model::MC10 ? MASTER_HZ : ALICE_HZ;
    sound.setMasterHz(m_masterHz);
    serial.setMasterHz(m_masterHz);
    tape.setMasterHz(m_masterHz);
    tape.setMotor(true);                     // no motor relay
    keyboard.setAlice(model != Model::MC10);
    m_cfg = {16, 8, BusRole::DATA, false, 0xFFFF};
    const size_t romSize = m_ef ? 0x4000 : 0x2000;
    m_rom.assign(romSize, 0xFF);
    for (size_t i = 0; i < romSize && !rom.empty(); ++i) m_rom[i] = rom[i % rom.size()];
    m_ram.assign(model == Model::Alice90 ? 0x8000 : model == Model::Alice32 ? 0x2000 : 0x1000, 0);
    m_ramStart = m_ef ? 0x3000 : 0x4000;
    ef.setClock(m_masterHz);
    display.useEf9345(m_ef);
    vdg.fetch = [this](uint16_t addr, int n, uint16_t* out) { fetchVideo(addr, n, out); };
    // The Alice's PAL board pads the VDG to 312 lines as a PAL CoCo's does (XRoar's assumption)
    if (model == Model::Alice) {
        Mc6847::PalPadding p;
        p.stop0 = 16; p.delay0 = 24;
        p.stop1 = 256; p.delay1 = 26;
        vdg.setPal(p);
    }
}

System::~System() = default;

void System::setCpu(M6801* cpu) {
    m_cpu = cpu;
    if (!cpu) return;
    cpu->setModePins(2);                     // P20 low, P21 high, P22 low: mode 2
    // The chip's own writes are on the bus too: the expansion port sees them
    cpu->onInternalWrite = [this](uint16_t a, uint8_t v) { if (m_cart) m_cart->internalWrite(a, v); };
    cpu->port(2).preRead = [this]() { port2In(); };
    cpu->port(2).postWrite = [this]() {
        // P20: the cassette's output and the serial line
        const bool p20 = m_cpu->port2Out() & 1;
        const uint64_t t = now();
        sync(t);
        tape.output(p20 ? 63 : 0);
        serial.txLevel(t, p20);
    };
}

uint64_t System::now() const {
    const int k = m_cpu ? m_cpu->cycleInStep() : 0;
    return m_ticks + (uint64_t)(k > 0 ? k - 1 : 0) * TICKS_PER_CYCLE;
}

void System::setCart(std::unique_ptr<Cart> cart) {
    m_cart = std::move(cart);
    if (!m_cart) return;
    m_cart->setNmi = [this](bool on) { if (m_cpu) m_cpu->setNmiLine(on); };
    m_cart->reset();
}

void System::powerOn() {
    std::fill(m_ram.begin(), m_ram.end(), 0);
    for (uint16_t a = 0; a < Ef9345::VRAM_SIZE; ++a) ef.setVram(a, 0);
    if (auto* r = dynamic_cast<RamPack*>(m_cart.get())) r->clear();
    resetButton();
}

void System::resetButton() {
    vdg.reset();
    ef.reset();
    keyboard.reset();
    m_latch = 0;
    vdg.setMode(0);
    if (m_cart) m_cart->reset();
    m_level = soundLevel();
    if (m_cpu) m_cpu->reset();
    port2In();
}

float System::soundLevel() const {
    float v = (m_latch & 0x80) ? 0.25f : 0.0f;            // the 1-bit sound
    if (tape.playing()) v += tape.input() ? -0.1f : 0.1f;  // the cassette, as heard
    return v;
}

// Rows 0-5 low for keys in strobed columns; row 6 (P21) separately
// (the Alice 90 reads only the highest column strobed)
uint8_t System::keyboardRows(uint8_t& row6) const {
    const uint8_t cols = m_cpu ? m_cpu->port(1).out() : 0xFF;
    uint8_t rows = 0;
    for (int c = 7; c >= 0; --c)
        if (!(cols & (1 << c))) {
            rows |= keyboard.column(c);
            if (m_model == Model::Alice90) break;
        }
    row6 = rows & 0x40;
    return (uint8_t)(~rows & 0x3F);
}

void System::port2In() {
    if (!m_cpu) return;
    sync(now());
    uint8_t row6;
    keyboardRows(row6);
    uint8_t in = 0xFF;
    if (row6) in &= (uint8_t)~0x02;                   // P21: row 6
    if (!serial.rxLevel()) in &= (uint8_t)~0x04;      // P22: RS-232 receive / the printer's ready (low)
    in &= (uint8_t)~0x08;                             // P23: CTS (low: clear to send)
    if (!tape.input()) in &= (uint8_t)~0x10;          // P24: the cassette
    m_cpu->setPort2Input(in);
}

void System::writeLatch(uint8_t v) {
    sync(now());
    m_latch = v;
    m_level = soundLevel();
    if (m_ef) return;                                 // the Alice 32/90: the sound only
    vdg.update();
    // D5 A/G, D2 GM2, D3 GM1, D4 GM0, D6 CSS
    const uint8_t mode = (uint8_t)(((v & 0x20) ? 0x80 : 0) | ((v & 0x04) ? 0x40 : 0) | ((v & 0x08) ? 0x20 : 0) |
                                   ((v & 0x10) ? 0x10 : 0) | ((v & 0x40) ? 0x08 : 0));
    vdg.setMode(mode);
    m_level = soundLevel();
}

// The VDG's own address into the internal RAM; GM2 is also INT/EXT; D7 A/S, D6 INV
void System::fetchVideo(uint16_t addr, int n, uint16_t* out) {
    const uint16_t ext = (m_latch & 0x04) ? Mc6847::F_EXT : 0;
    for (int i = 0; i < n; ++i) {
        const uint16_t a = (uint16_t)((addr + i) & 0x1FFF);
        const uint8_t d = a < 0x1000 ? m_ram[a % m_ram.size()] : 0;
        *out++ = (uint16_t)(d | ext | ((d & 0x80) ? Mc6847::F_AS : 0) | ((d & 0x40) ? Mc6847::F_INV : 0));
    }
}

void System::sync(uint64_t to) {
    if (to <= m_dev) return;
    const uint64_t d = to - m_dev;
    sound.run(d, m_level);
    if (m_ef) ef.advance(d);
    else vdg.advance(d);
    const bool tapeWas = tape.input();
    tape.clock(d);
    if (tape.input() != tapeWas) m_level = soundLevel();
    keyboard.clock(d);
    serial.clock(d);
    m_dev = to;
    if (m_cpu) serial.txLevel(m_dev, m_cpu->port2Out() & 1);
    if (m_playAfterTyping && !keyboard.typing()) {
        m_playAfterTyping = false;
        tape.controlTape("play");
    }
}

uint8_t System::efRead(uint16_t a, bool peek) {
    if (a >= m_ramStart && a < m_ramStart + m_ram.size()) return m_ram[a - m_ramStart];
    if (a >= 0xC000) return m_rom[a & 0x3FFF];
    if ((a & 0xFFF0) == 0xBF20) {
        if (peek) return ef.peek((uint8_t)(a & 0x0F));
        sync(now());
        return ef.read((uint8_t)(a & 0x0F));
    }
    if (a >= 0x8000) {                                // the keyboard's rows
        if (!peek) sync(now());
        uint8_t row6;
        return (uint8_t)((a & 0xC0) | keyboardRows(row6));
    }
    return (uint8_t)a;
}

uint8_t System::decodeRead(uint16_t a) {
    if (m_ef) return efRead(a, false);
    switch (a >> 14) {
        case 1:
            if (!(a & 0x1000)) return m_ram[a & 0x0FFF];
            break;
        case 2: {                                     // the keyboard's rows
            sync(now());
            uint8_t row6;
            return (uint8_t)((a & 0xC0) | keyboardRows(row6));
        }
        case 3: return m_rom[a & 0x1FFF];
        default: break;
    }
    return (uint8_t)a;                                // the floating bus: the address's low byte
}

void System::decodeWrite(uint16_t a, uint8_t v) {
    if (m_ef) {
        if (a >= m_ramStart && a < m_ramStart + m_ram.size()) m_ram[a - m_ramStart] = v;
        else if ((a & 0xFFF0) == 0xBF20) { sync(now()); ef.write((uint8_t)(a & 0x0F), v); }
        else if (a >= 0x8000 && a < 0xC000) writeLatch(v);
        return;
    }
    switch (a >> 14) {
        case 1:
            if (!(a & 0x1000)) {
                sync(now());
                vdg.update();                         // the VDG has what it fetched before this write
                m_ram[a & 0x0FFF] = v;
            }
            break;
        case 2: writeLatch(v); break;
        default: break;
    }
}

uint8_t System::read8(uint32_t addr) {
    const uint16_t a = (uint16_t)addr;
    uint8_t v;
    if (m_cpu && m_cpu->isInternal(a)) {              // the debugger (the CPU keeps these to itself)
        m_cpu->peekInternal(a, v);
        return v;
    }
    if (!(m_cart && m_cart->read(a, v))) v = decodeRead(a);
    if (m_observer) m_observer->onMemoryRead(this, a, v);
    return v;
}

void System::write8(uint32_t addr, uint8_t v) {
    const uint16_t a = (uint16_t)addr;
    if (m_cpu && m_cpu->isInternal(a)) { m_cpu->writeInternal(a, v); return; }
    if (m_observer) m_observer->onMemoryWrite(this, a, peek8(a), v);
    if (m_cart && m_cart->write(a, v)) return;
    decodeWrite(a, v);
}

uint8_t System::peek8(uint32_t addr) {
    const uint16_t a = (uint16_t)addr;
    uint8_t v;
    if (m_cpu && m_cpu->peekInternal(a, v)) return v;
    if (m_cart && m_cart->peek(a, v)) return v;
    if (m_ef) return efRead(a, true);
    switch (a >> 14) {
        case 1: return (a & 0x1000) ? (uint8_t)a : m_ram[a & 0x0FFF];
        case 2: { uint8_t row6; return (uint8_t)((a & 0xC0) | keyboardRows(row6)); }
        case 3: return m_rom[a & 0x1FFF];
        default: return (uint8_t)a;
    }
}

int System::step() {
    int c = 1;
    if (m_cpu) c = m_cpu->step();
    m_ticks += (uint64_t)c * TICKS_PER_CYCLE;
    sync(m_ticks);
    return c;
}

// Snapshots: RAM, the latch, the VDG, the expansion device and the clock
size_t System::stateSize() const { return state().size(); }

void System::saveState(uint8_t* buf) const {
    const auto v = state();
    std::memcpy(buf, v.data(), v.size());
}

std::vector<uint8_t> System::state() const {
    std::vector<uint8_t> v;
    putBlob(v, m_ram);
    putBlob(v, vdg.saveState());
    const std::string kind = m_cart ? m_cart->kind() : "";
    putBlob(v, std::vector<uint8_t>(kind.begin(), kind.end()));
    putBlob(v, m_cart ? m_cart->saveState() : std::vector<uint8_t>());
    put(v, m_latch);
    put(v, m_ticks);
    put(v, m_dev);
    if (m_ef) putBlob(v, ef.saveState());
    return v;
}

void System::loadState(const uint8_t* buf) {
    const uint8_t* p = buf;
    const auto ram = getBlob(p);
    const auto vd = getBlob(p);
    const auto kind = getBlob(p);
    const auto cs = getBlob(p);
    get(p, m_latch);
    get(p, m_ticks);
    get(p, m_dev);
    if (m_ef) ef.loadState(getBlob(p));
    if (ram.size() == m_ram.size()) m_ram = ram;
    vdg.loadState(vd);
    if (m_cart && std::string(kind.begin(), kind.end()) == m_cart->kind()) m_cart->loadState(cs);
    m_level = soundLevel();
}

System* systemOf(MachineDescriptor* m) {
    if (!m || m->buses.empty()) return nullptr;
    return dynamic_cast<System*>(m->buses[0].bus);
}

// ---------------------------------------------------------------------------
// The machine
// ---------------------------------------------------------------------------
namespace {

// Without BASIC: a stand-in at $E000 that clears the screen, says so and waits
std::vector<uint8_t> placeholderRom(const std::string& file) {
    std::vector<uint8_t> r(0x2000, 0x01);
    const uint8_t code[] = {
        0x8E, 0x00, 0xFF,           // E000 LDS #$00FF
        0x7F, 0xBF, 0xFF,           // E003 CLR $BFFF (alphanumerics)
        0xCE, 0x40, 0x00,           // E006 LDX #$4000
        0x86, 0x60,                 // E009 LDAA #$60 (space)
        0xA7, 0x00,                 // E00B fill: STAA 0,X
        0x08,                       // E00D INX
        0x8C, 0x42, 0x00,           // E00E CPX #$4200
        0x26, 0xF8,                 // E011 BNE fill
        0xCE, 0xE0, 0x40,           // E013 LDX #$E040
        0xDF, 0x82,                 // E016 STX $82 (source)
        0xCE, 0x40, 0x00,           // E018 LDX #$4000
        0xDF, 0x80,                 // E01B STX $80 (screen)
        0xDE, 0x82,                 // E01D copy: LDX $82
        0xA6, 0x00,                 // E01F LDAA 0,X
        0x27, 0x0C,                 // E021 BEQ done
        0x08,                       // E023 INX
        0xDF, 0x82,                 // E024 STX $82
        0xDE, 0x80,                 // E026 LDX $80
        0xA7, 0x00,                 // E028 STAA 0,X
        0x08,                       // E02A INX
        0xDF, 0x80,                 // E02B STX $80
        0x20, 0xEE,                 // E02D BRA copy
        0x20, 0xFE,                 // E02F done: BRA *
    };
    std::memcpy(r.data(), code, sizeof code);
    std::string msg = "NO BASIC ROM: PUT " + file + " IN ROMS/MC10/ (SEE DOC/README-MC10.MD)";
    for (auto& c : msg) c = (char)std::toupper((unsigned char)c);
    size_t i = 0;
    for (; i < msg.size() && i < 0x180; ++i) r[0x40 + i] = (uint8_t)((msg[i] & 0x3F) | 0x40);   // VDG codes, not inverted
    r[0x40 + i] = 0;
    r[0x1FFE] = 0xE0;               // reset vector: $E000
    r[0x1FFF] = 0x00;
    return r;
}

// The Alice 32/90 without its ROM: a stand-in at $C000 that sets up the
// EF9345 (40 columns, rows from Y=8), writes the message on row 1 and waits
std::vector<uint8_t> placeholderEfRom(const std::string& file) {
    std::vector<uint8_t> r(0x4000, 0x01);
    std::vector<uint8_t> c;
    const uint16_t WAIT = 0xC0F0, MSG = 0xC100;
    auto jsrWait = [&]() { c.insert(c.end(), {0xBD, (uint8_t)(WAIT >> 8), (uint8_t)WAIT}); };
    c.insert(c.end(), {0x8E, 0x00, 0xFF});                        // LDS #$00FF
    const uint8_t ind[][2] = {{0x00, 0x81}, {0x00, 0x82}, {0x37, 0x83}, {0x00, 0x84}, {0x08, 0x87}};
    for (const auto& p : ind) {                                   // TGS, MAT, PAT, DOR, ROR
        jsrWait();
        c.insert(c.end(), {0x86, p[0], 0xB7, 0xBF, 0x21});        // LDAA #v; STAA R1
        c.insert(c.end(), {0x86, p[1], 0xB7, 0xBF, 0x28});        // LDAA #IND; STAA R0 (and go)
    }
    jsrWait();
    c.insert(c.end(), {0x86, 0x08, 0xB7, 0xBF, 0x26});            // R6 (Y) = 8: row 1
    c.insert(c.end(), {0x7F, 0xBF, 0x27, 0x7F, 0xBF, 0x22});      // R7 (X) = 0; R2 (B) = 0: G0
    c.insert(c.end(), {0x86, 0x70, 0xB7, 0xBF, 0x23});            // R3 (A) = white on black
    c.insert(c.end(), {0xCE, (uint8_t)(MSG >> 8), (uint8_t)MSG});  // LDX #MSG
    const size_t loop = c.size();
    jsrWait();
    c.insert(c.end(), {0xA6, 0x00, 0x27, 0x0B});                  // LDAA 0,X; BEQ done
    c.insert(c.end(), {0xB7, 0xBF, 0x21, 0x86, 0x01, 0xB7, 0xBF, 0x28, 0x08});   // R1; KRF+inc; INX
    c.insert(c.end(), {0x20, (uint8_t)(loop - (c.size() + 2))});  // BRA loop
    c.insert(c.end(), {0x20, 0xFE});                              // done: BRA *
    std::copy(c.begin(), c.end(), r.begin());
    const uint8_t wait[] = {0xF6, 0xBF, 0x20, 0x2B, 0xFB, 0x39};  // LDAB status; BMI WAIT; RTS
    std::copy(wait, wait + sizeof wait, r.begin() + (WAIT - 0xC000));
    std::string msg = "NO ROM: PUT " + file + " IN ROMS/MC10/";
    for (auto& ch : msg) ch = (char)std::toupper((unsigned char)ch);
    msg.resize(std::min<size_t>(msg.size(), 40));
    std::copy(msg.begin(), msg.end(), r.begin() + (MSG - 0xC000));
    r[(MSG - 0xC000) + msg.size()] = 0;
    r[0x3FFE] = 0xC0;               // reset vector: $C000
    r[0x3FFF] = 0x00;
    return r;
}

std::vector<uint8_t> loadMcxRom(Model model, const std::string& option, std::string& err) {
    std::vector<uint8_t> rom;
    const std::string path = !option.empty() ? option
        : firmwareDir() + (model == Model::Alice ? "/alice128bas.rom" : "/mcx128bas.rom");
    if (!readFile(path, rom) || rom.empty()) {
        err = "MCX-128 ROM not found: put " + std::string(model == Model::Alice ? "alice128bas.rom" : "mcx128bas.rom") +
              " (16 KB) in " + firmwareDir() + "/ (or set MMEMU_MC10_FIRMWARE), or give its path";
        return {};
    }
    if (rom.size() == 0x4000 + 24) rom.erase(rom.begin(), rom.begin() + 24);   // the VMC-10 emulator's header
    return rom;
}

}  // namespace

MachineDescriptor* create(Model model, std::string* err, Expansion expansion, const Roms* roms) {
    const bool alice = model == Model::Alice;
    const bool efModel = hasEf9345(model);
    const std::string romName = efModel ? "alice32.rom" : alice ? "alice.rom" : "mc10.rom";
    const size_t romSize = efModel ? 0x4000 : 0x2000;
    std::vector<uint8_t> basic, charset;
    std::string missing, noCharset;
    if (roms) {
        basic = roms->basic;
        charset = roms->charset;
        if (basic.empty()) { if (err) *err = "no BASIC image"; return nullptr; }
    } else {
        if (!readFile(firmwareDir() + "/" + romName, basic) || basic.size() != romSize) {
            missing = std::string(efModel ? "Alice 32/90 BASIC" : alice ? "Alice BASIC" : "Micro Color BASIC") +
                      " ROM not found: put " + romName + " (" + std::to_string(romSize / 1024) + " KB) in " +
                      firmwareDir() + "/ or set MMEMU_MC10_FIRMWARE to its directory";
            if (err) *err = missing;
            std::fprintf(stderr, "%s\n", missing.c_str());
            basic = efModel ? placeholderEfRom(romName) : placeholderRom(romName);
        }
        if (efModel && !readFile(firmwareDir() + "/ef9345.rom", charset) && !readFile(firmwareDir() + "/charset.rom", charset))
            charset.clear();
    }
    if (efModel && charset.size() != Ef9345::CHARSET_SIZE) {
        noCharset = "EF9345 character ROM not found: put ef9345.rom (8 KB; MAME's charset.rom) in " + firmwareDir() +
                    "/: without it the screen shows no characters";
        if (!roms) std::fprintf(stderr, "%s\n", noCharset.c_str());
    }

    auto* sys = new System(model, std::move(basic));
    if (efModel) sys->setCharset(charset);
    auto* desc = new MachineDescriptor();
    desc->machineId = model == Model::Alice90 ? "alice90" : model == Model::Alice32 ? "alice32" : alice ? "alice" : "mc10";
    desc->displayName = model == Model::Alice90 ? "Matra Alice 90" : model == Model::Alice32 ? "Matra Alice 32"
                      : alice ? "Matra Alice" : "TRS-80 MC-10";
    desc->licenseClass = "proprietary";
    desc->uiPanesSet = true;
    desc->uiPanes = {"tape"};
    desc->clockHz = sys->masterHz() / TICKS_PER_CYCLE;
    if (!missing.empty()) desc->warnings.push_back(missing);
    if (!noCharset.empty()) desc->warnings.push_back(noCharset);

    auto* cpu = new M6801(M6801::Variant::MC6803);
    cpu->setDataBus(sys);
    sys->setCpu(cpu);
    desc->buses.push_back({"system", sys});
    desc->cpus.push_back({"main", cpu, sys, sys, nullptr, true, 1});

    // The expansion port's device at power on
    std::vector<uint8_t> mcxRom = roms ? roms->mcx128 : std::vector<uint8_t>();
    if (expansion == Expansion::RamPack16K) {
        sys->setCart(std::unique_ptr<Cart>(new RamPack()));
    } else if (expansion == Expansion::Mcx128 && efModel) {
        desc->warnings.push_back("the MCX-128 is for the MC-10 and the Alice, not the Alice 32/90");
    } else if (expansion == Expansion::Mcx128) {
        std::string e;
        if (mcxRom.empty()) mcxRom = loadMcxRom(model, "", e);
        if (mcxRom.empty()) desc->warnings.push_back(e);
        sys->setCart(std::unique_ptr<Cart>(new Mcx128(mcxRom)));
    }

    auto* io = new IORegistry();
    desc->ioRegistry = io;
    io->registerHandler(&sys->display);
    io->registerHandler(&sys->keyboard);
    io->registerHandler(&sys->sound);
    io->registerHandler(&sys->tapeDeck);
    io->registerHandler(&sys->serial);
    io->registerHandler(&sys->expansionPort);

    desc->onKey = [sys](const std::string& name, bool down) {
        if (name == "reset") { if (down) sys->resetButton(); return true; }
        return sys->keyboard.key(name, down);
    };
    desc->screenButtons = {
        {"RESET", {"reset"}, "The RESET button: BASIC restarts; the program in memory stays."},
    };

    // The expansion port: the 16K RAM pack, a program pack, the MCX-128
    {
        MachineDescriptor::ExpansionSlot s;
        s.name = "expansion";
        s.description = "The expansion port (by default Tandy's 16K RAM pack)";
        s.devices = {
            {"ram16k", "16K RAM Expansion Pack (26-3013): RAM at $5000-$8FFF, 20K in all", {}, ""},
            {"rompak", "A program pack: up to 16K of ROM at $5000; option: the image", {}, ""},
            {"mcx128", "Darren Atkinson's MCX-128: 128K RAM and its BASIC ROM; option: the ROM (mcx128bas.rom)", {}, ""},
        };
        s.install = [sys, model](const std::string& device, const std::string& option, std::string& err) {
            if (device.empty()) { sys->setCart(nullptr); return true; }
            if (device == "ram16k") { sys->setCart(std::unique_ptr<Cart>(new RamPack())); return true; }
            if (device == "rompak") {
                std::vector<uint8_t> rom;
                if (option.empty() || !readFile(option, rom) || rom.empty()) { err = "rompak needs the image's path"; return false; }
                const auto slash = option.find_last_of('/');
                sys->setCart(std::unique_ptr<Cart>(new RomPak(std::move(rom), slash == std::string::npos ? option : option.substr(slash + 1))));
                return true;
            }
            if (device == "mcx128" && hasEf9345(model)) { err = "the MCX-128 is for the MC-10 and the Alice"; return false; }
            if (device == "mcx128") {
                std::vector<uint8_t> rom = loadMcxRom(model, option, err);
                if (rom.empty()) return false;
                sys->setCart(std::unique_ptr<Cart>(new Mcx128(std::move(rom))));
                return true;
            }
            err = "no device '" + device + "' (ram16k, rompak, mcx128)";
            return false;
        };
        s.info = [sys]() { return sys->cart() ? sys->cart()->describe() : std::string(); };
        desc->expansionSlots.push_back(std::move(s));
    }
    // What is on the serial port: its bytes, or the TP-10's paper
    {
        MachineDescriptor::ExpansionSlot s;
        s.name = "serial";
        s.description = "The serial port's device (capture with the serial command); option: the baud rate";
        s.devices = {
            {"printer", "A serial printer or RS-232 device: the bytes as sent", {"300", "600", "1200", "2400", "4800", "9600"}, "600"},
            {"tp10", "TP-10 thermal printer: 32-column paper", {"600"}, "600"},
        };
        s.canEmpty = false;
        s.install = [sys](const std::string& device, const std::string& option, std::string& err) {
            if (device != "printer" && device != "tp10" && !device.empty()) { err = "printer or tp10"; return false; }
            sys->serial.setTp10(device == "tp10");
            if (!option.empty()) {
                const int b = std::atoi(option.c_str());
                if (b < 50 || b > 115200) { err = "baud: 50-115200"; return false; }
                sys->serial.setBaud(b);
            }
            return true;
        };
        s.info = [sys]() { return std::string(sys->serial.tp10() ? "tp10" : "printer") + " (" + std::to_string(sys->serial.baud()) + " baud)"; };
        desc->expansionSlots.push_back(std::move(s));
    }

    desc->symbolFiles.push_back(PathUtil::findResource("roms/mc10/hardware.sym"));
    desc->onReset = [sys](MachineDescriptor&) { sys->powerOn(); };
    desc->schedulerStep = [sys](MachineDescriptor&) -> int { return sys->step(); };
    desc->deviceCycles = [sys]() { return sys->cycles(); };
    desc->onReset(*desc);
    return desc;
}

}  // namespace mc10
