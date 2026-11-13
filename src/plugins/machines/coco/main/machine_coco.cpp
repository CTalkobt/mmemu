// TRS-80 Color Computer 1, 2 and 2B. The wiring of the SAM, VDG and PIAs
// after the Color Computer Technical Reference Manual and XRoar
// (dragon/dragon.c, dragon/coco.c; GPL-3.0-or-later), mmsim's own code.

#include "coco_system.h"
#include "plugins/6809/main/cpu6809.h"
#include "plugins/devices/acia6551/main/acia6551.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdebug/main/execution_observer.h"
#include "util/path_util.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <map>

namespace coco {

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

// Key codes: row << 3 | column
constexpr int K_SHIFT = 6 << 3 | 7;
constexpr int SHIFTED = 0x100;

int letterCode(char c) {
    c = (char)std::tolower((unsigned char)c);
    return 1 + (c - 'a');                 // A = row 0 column 1 ... Z = row 3 column 2
}

// A character as a key (with SHIFTED), or -1
int charCode(char c) {
    if (std::isalpha((unsigned char)c)) return letterCode(c);
    if (c >= '0' && c <= '9') return 32 + (c - '0');
    static const char* SHIFT_DIGITS = "!\"#$%&'()";
    if (const char* p = c ? std::strchr(SHIFT_DIGITS, c) : nullptr) return (33 + (int)(p - SHIFT_DIGITS)) | SHIFTED;
    switch (c) {
        case '@': return 0;
        case ' ': return 31;
        case '\n': case '\r': return 48;
        case ':': return 42;
        case ';': return 43;
        case ',': return 44;
        case '-': return 45;
        case '.': return 46;
        case '/': return 47;
        case '*': return 42 | SHIFTED;
        case '+': return 43 | SHIFTED;
        case '<': return 44 | SHIFTED;
        case '=': return 45 | SHIFTED;
        case '>': return 46 | SHIFTED;
        case '?': return 47 | SHIFTED;
        case '^': return 27;
        case '_': return 27 | SHIFTED;
        case '[': return 28 | SHIFTED;
        case ']': return 30 | SHIFTED;
        case '\\': return 49 | SHIFTED;
        case 0x08: return 29;
        case 0x0C: return 49;
        case 0x03: return 50;
        default: return -1;
    }
}

}  // namespace

std::string firmwareDir() {
    const char* v = std::getenv("MMEMU_COCO_FIRMWARE");
    return v ? v : PathUtil::findResource("roms/coco");
}

// ---------------------------------------------------------------------------
// RAM
// ---------------------------------------------------------------------------
void Ram::configure(int kb) {
    m_kb = kb <= 4 ? 4 : kb <= 16 ? 16 : kb <= 32 ? 32 : 64;
    switch (m_kb) {
        case 4: m_addrBits = 12; m_rowBits = 6; break;       // 4K x1
        case 16: m_addrBits = 14; m_rowBits = 7; break;      // 16K x1
        case 32: m_addrBits = 15; m_rowBits = 8; break;      // 32K x1
        default: m_addrBits = 16; m_rowBits = 8; break;      // 64K x1
    }
    m_banks = 1;
    m_bank[0].assign((size_t)1 << m_addrBits, 0);
    m_bank[1].clear();
}

uint8_t* Ram::at(uint8_t bank, uint16_t row, uint16_t col) {
    if (bank >= m_banks) return nullptr;
    const unsigned idx = (row & ((1u << m_rowBits) - 1)) | ((col & ((1u << (m_addrBits - m_rowBits)) - 1)) << m_rowBits);
    return &m_bank[bank][idx];
}

const uint8_t* Ram::at(uint8_t bank, uint16_t row, uint16_t col) const {
    return const_cast<Ram*>(this)->at(bank, row, col);
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------
void Keyboard::reset() {
    std::memset(m_matrix, 0, sizeof(m_matrix));
    m_shifts = 0;
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
    if ((col & 7) == 7 && m_shifts > 0) v |= 0x40;
    return v;
}

void Keyboard::pressChord(int code, bool down) {
    if (code & SHIFTED) m_shifts += down ? 1 : -1;
    if (m_shifts < 0) m_shifts = 0;
    int row = (code >> 3) & 7;
    if (m_dragon && row < 6) row = (row + 2) % 6;     // the Dragon: digits on rows 0-1, letters from row 2
    set(row, code & 7, down);
}

bool Keyboard::key(std::string n, bool down) {
    for (auto& c : n) c = (char)std::tolower((unsigned char)c);
    static const std::map<std::string, int> keys = {
        {"at", 0}, {"up", 27}, {"down", 28}, {"left", 29}, {"right", 30}, {"space", 31},
        {"colon", 42}, {"semicolon", 43}, {"comma", 44}, {"minus", 45}, {"period", 46}, {"slash", 47},
        {"return", 48}, {"enter", 48}, {"clear", 49}, {"home", 49}, {"break", 50}, {"escape", 50},
        {"esc", 50}, {"run_stop", 50}, {"left_shift", K_SHIFT}, {"right_shift", K_SHIFT}, {"shift", K_SHIFT},
        {"delete", 29}, {"backspace", 29},
        // The PC's shifted symbols, as the GUI names them: SHIFT and the CoCo key
        {"exclaim", 33 | SHIFTED}, {"dquote", 34 | SHIFTED}, {"hash", 35 | SHIFTED}, {"dollar", 36 | SHIFTED},
        {"percent", 37 | SHIFTED}, {"ampersand", 38 | SHIFTED}, {"squote", 39 | SHIFTED},
        {"lparen", 40 | SHIFTED}, {"rparen", 41 | SHIFTED}, {"asterisk", 42 | SHIFTED}, {"plus", 43 | SHIFTED},
        {"less", 44 | SHIFTED}, {"kpeq", 45 | SHIFTED}, {"equal", 45 | SHIFTED}, {"equals", 45 | SHIFTED},
        {"greater", 46 | SHIFTED}, {"question", 47 | SHIFTED}, {"uparrow", 27}, {"caret", 27},
        {"leftarrow", 27 | SHIFTED}, {"underscore", 27 | SHIFTED}, {"bracket_left", 28 | SHIFTED},
        {"bracket_right", 30 | SHIFTED}, {"backslash", 49 | SHIFTED},
    };
    int code = -1;
    if (n.size() == 1 && std::isalpha((unsigned char)n[0])) code = letterCode(n[0]);
    else if (n.size() == 1 && std::isdigit((unsigned char)n[0])) code = 32 + (n[0] - '0');
    else {
        auto it = keys.find(n);
        if (it == keys.end()) return false;
        code = it->second;
    }
    pressChord(code, down);
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

void Keyboard::clock(uint64_t ticks) {
    while (ticks) {
        if (m_wait > ticks) { m_wait -= ticks; return; }
        ticks -= m_wait;
        m_wait = 0;
        if (m_typing >= 0) {
            pressChord(m_typing, false);
            m_typing = -1;
            m_wait = KEY_GAP;
            continue;
        }
        if (m_queue.empty()) return;
        m_typing = m_queue.front();
        m_queue.pop_front();
        pressChord(m_typing, true);
        m_wait = KEY_HOLD;
    }
}

// ---------------------------------------------------------------------------
// The bit banger's transmit line
// ---------------------------------------------------------------------------
BitBanger::~BitBanger() {
    if (m_file) std::fclose(m_file);
}

bool BitBanger::attachSerial(const std::string& spec) {
    if (spec.rfind("file:", 0) == 0) {
        if (m_file) std::fclose(m_file);
        m_file = std::fopen(spec.substr(5).c_str(), "ab");
        return m_file != nullptr;
    }
    if (spec == "none" && m_file) {
        std::fclose(m_file);
        m_file = nullptr;
        return true;
    }
    return false;
}

bool BitBanger::setOption(const std::string& key, const std::string& value, std::string& err) {
    if (key != "baud") { err = "no such option (baud)"; return false; }
    const int b = std::atoi(value.c_str());
    if (b < 50 || b > 115200) { err = "baud: 50-115200"; return false; }
    m_baud = b;
    return true;
}

void BitBanger::txLevel(uint64_t now, bool level) {
    decodeFrame(now);
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

// A frame is done ten bit times after its start bit: sample the middle of each data bit
void BitBanger::decodeFrame(uint64_t now) {
    if (!m_inFrame) return;
    const double bit = m_hz / m_baud;
    if ((double)(now - m_frameStart) < bit * 9.5) return;
    uint8_t v = 0;
    for (int i = 0; i < 8; ++i) {
        const uint64_t t = m_frameStart + (uint64_t)(bit * (1.5 + i));
        bool level = false;
        for (const auto& e : m_edges) {
            if (e.first > t) break;
            level = e.second;
        }
        if (level) v |= (uint8_t)(1 << i);
    }
    m_inFrame = false;
    sent.push_back(v);
    if (m_file) {
        std::fputc(v, m_file);
        std::fflush(m_file);
    }
    // A start bit already under way (the line went low again after the stop bit)
    const uint64_t stop = m_frameStart + (uint64_t)(bit * 9.0);
    bool level = false;
    uint64_t lastFall = 0;
    for (const auto& e : m_edges) {
        if (e.first < stop) { level = e.second; continue; }
        if (!e.second && level) lastFall = e.first;
        level = e.second;
    }
    if (lastFall && !m_level) {
        m_inFrame = true;
        m_frameStart = lastFall;
        std::vector<std::pair<uint64_t, bool>> rest;
        for (const auto& e : m_edges) if (e.first > lastFall) rest.push_back(e);
        m_edges = rest;
    }
}

// ---------------------------------------------------------------------------
// Sound
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
    std::memcpy(buffer, m_vdg->frame(), sizeof(uint32_t) * Mc6847::OUT_W * Mc6847::OUT_H);
}

bool CartPort::attachSerial(const std::string& spec) {
    return m_sys->cart() && m_sys->cart()->attachSerial(spec);
}

bool CartPort::mountDisk(int unit, const std::string& path) {
    return m_sys->cart() && m_sys->cart()->mountDisk(unit - 1, path);
}

void CartPort::ejectDisk(int unit) {
    if (m_sys->cart()) m_sys->cart()->ejectDisk(unit - 1);
}

bool CartPort::getDiskStatus(int unit, int& track, int& sector, bool& led) {
    sector = 0;
    return m_sys->cart() && m_sys->cart()->diskStatus(unit - 1, track, led);
}

std::string CartPort::getMountedDiskPath(int unit) {
    return m_sys->cart() ? m_sys->cart()->diskPath(unit - 1) : "";
}

void CartPort::getDeviceInfo(DeviceInfo& out) const {
    IOHandler::getDeviceInfo(out);
}

// ---------------------------------------------------------------------------
// The system
// ---------------------------------------------------------------------------
System::System(Model model, int ramKb, std::vector<uint8_t> extBasic, std::vector<uint8_t> colorBasic, Tv tv)
    : vdg(model == Model::CoCo2B), m_model(model) {
    m_pal = tv == Tv::PAL || (tv == Tv::Default && isDragon(model));
    m_masterHz = !m_pal ? MASTER_HZ : isDragon(model) ? DRAGON_PAL_HZ : COCO_PAL_HZ;
    sound.setMasterHz(m_masterHz);
    rs232.setMasterHz(m_masterHz);
    tape.setMasterHz(m_masterHz);
    m_cfg = {16, 8, BusRole::DATA, false, 0xFFFF};
    m_rom.assign(0x4000, 0xFF);
    m_haveExt = !extBasic.empty();
    for (size_t i = 0; i < 0x2000; ++i) {
        if (m_haveExt) m_rom[i] = extBasic[i % extBasic.size()];
        if (!colorBasic.empty()) m_rom[0x2000 + i] = colorBasic[i % colorBasic.size()];
    }
    m_ram.configure(ramKb);

    // The VDG's sync pulses: HS to PIA0 CA1 and the SAM, FS to PIA0 CB1 and the SAM
    // (a PAL CoCo's board inverts HS on its way to the PIA)
    const bool hsInvert = m_pal && !isDragon(model);
    vdg.onHs = [this, hsInvert](bool level) { pia0.setCa1(level != hsInvert); sam.hsync(level); };
    vdg.onFs = [this](bool level) { pia0.setCb1(level); sam.fsync(level); };
    vdg.fetch = [this](uint16_t, int n, uint16_t* out) { fetchVideo(n, out); };
    sam.onVideoChange = [this]() { vdg.update(); };

    pia0.a.preRead = [this]() { keyboardUpdate(); joystickUpdate(); };
    pia0.b.preRead = [this]() { keyboardUpdate(); ramSizeUpdate(); };
    pia0.a.onControl = [this]() { soundChanged(); };          // CA2, CB2: the joystick / sound selects
    pia0.b.onControl = [this]() { soundChanged(); };
    pia0.onIrq = [this]() { updateLines(); };
    pia1.onIrq = [this]() { updateLines(); };
    pia1.a.postWrite = [this]() {
        const uint8_t pa = pia1.portA();
        soundChanged();
        tape.output((uint8_t)(pa >> 2));
        rs232.txLevel(m_dev, pa & 2);
    };
    pia1.a.onControl = [this]() { tape.setMotor(pia1.ca2()); };   // CA2: the cassette relay
    pia1.b.preRead = [this]() { ramSizeUpdate(); };
    pia1.b.postWrite = [this]() {
        vdg.setMode((uint8_t)(pia1.portB() & 0xF8));            // PB3 CSS, PB4-6 GM0-2, PB7 A/G
        soundChanged();
    };
    pia1.b.onControl = [this]() { soundChanged(); };          // CB2: the sound mux enable

    // PAL: the board stops the VDG twice a frame for 50 blank lines in all
    if (m_pal) {
        Mc6847::PalPadding p;
        if (isDragon(model)) {
            p.stop0 = 262; p.delay0 = 25;
            p.stop1 = 254; p.delay1 = 25;
            p.hsInhibit = model == Model::Dragon64;
        } else {
            p.stop0 = 16; p.delay0 = 24;
            p.stop1 = 256; p.delay1 = 26;
        }
        vdg.setPal(p);
    }
    if (isDragon(model)) wireDragon();
}

System::~System() = default;

void System::setRamKb(int kb) {
    m_ram.configure(kb);
    ramSizeUpdate();
}

uint8_t System::ramByte(uint32_t phys) const {
    const auto& b = m_ram.m_bank[0];
    return b.empty() ? 0xFF : b[phys % b.size()];
}

void System::setCart(std::unique_ptr<Cart> cart) {
    m_cart = std::move(cart);
    m_cartLine = true;
    pia1.setCb1(true);
    m_halt = false;
    if (m_nmi && m_cpu) m_cpu->setNmiLine(false);
    m_nmi = false;
    if (!m_cart) return;
    m_cart->setCart = [this](bool level) {
        m_cartLine = level;
        pia1.setCb1(level);
    };
    m_cart->setNmi = [this](bool on) {
        m_nmi = on;
        if (m_cpu) m_cpu->setNmiLine(on);
    };
    m_cart->setHalt = [this](bool on) { m_halt = on; };
    m_cart->setClockHz(m_masterHz);
    m_cart->reset();
}

void System::powerOn() {
    for (auto& b : m_ram.m_bank) std::fill(b.begin(), b.end(), 0);
    resetButton();
}

void System::resetButton() {
    pia0.a.inSink = pia0.b.inSink = pia1.a.inSink = pia1.b.inSink = 0xFF;
    pia0.a.inSource = 0;
    pia0.b.inSource = 0xFF;          // the keyboard's columns read high unless a key pulls one down
    pia1.a.inSource = pia1.b.inSource = 0;
    if (dragon()) {
        pia1.b.inSource |= 0x01;     // the printer's BUSY pulled up (and pulled low by a ready printer)
        pia1.b.inSink &= (uint8_t)~0x01;
        printer.reset();
        m_ackLeft = 0;
        pia1.setCa1(true);
        if (m_acia) m_acia->reset();
    }
    sam.reset();
    pia0.reset();
    pia1.reset();
    vdg.reset();
    keyboard.reset();
    if (m_cart) m_cart->reset();
    m_halt = false;
    ramSizeUpdate();
    if (dragon()) dragonRomSelect();
    m_level = soundLevel();
    if (m_cpu) m_cpu->reset();
    updateLines();
}

void System::updateLines() {
    if (!m_cpu) return;
    m_cpu->setIrqLine(pia0.irq());          // PIA0: HS, FS
    m_cpu->setFirqLine(pia1.irq());         // PIA1: CD (RS-232 carrier), CART
}

void System::keyboardUpdate() {
    // Columns strobed low by PIA0 port B's outputs pull their keys' rows low
    // on port A; rows driven low by port A outputs pull columns low (the
    // reverse scan some games use). The fire buttons pull PA0 (right) and PA1 (left).
    const uint8_t colsLow = (uint8_t)(pia0.b.ddr & ~pia0.b.out);
    const uint8_t rowsDriven = (uint8_t)(pia0.a.ddr & ~pia0.a.out & 0x7F);
    uint8_t rowsLow = 0, colsPulled = 0;
    for (int c = 0; c < 8; ++c) {
        const uint8_t k = keyboard.column(c);
        if (colsLow & (1 << c)) rowsLow |= k;
        if (k & rowsDriven) colsPulled |= (uint8_t)(1 << c);
    }
    if (m_button[0]) rowsLow |= 1;
    if (m_button[1]) rowsLow |= 2;
    pia0.a.inSink = (uint8_t)((pia0.a.inSink & 0x80) | (~rowsLow & 0x7F));
    pia0.b.inSink = (uint8_t)~colsPulled;
}

void System::joystickUpdate() {
    // The comparator (PA7): the joystick the selects pick (CB2 right/left,
    // CA2 X/Y) against the DAC (PIA1 PA2-PA7)
    const int port = pia0.cb2() ? 1 : 0;
    const int axis = pia0.ca2() ? 1 : 0;
    const unsigned dac = (unsigned)((pia1.portA() & 0xFC) | 2) << 8;
    if (m_axis[port][axis] >= dac) pia0.a.inSink |= 0x80;
    else pia0.a.inSink &= 0x7F;
}

// PIA1 PB2 tells BASIC the RAM size: low with 4K chips, high with 16K, and
// with 64K tied to PIA0 PB6 (which BASIC toggles to see whether it follows)
void System::ramSizeUpdate() {
    if (dragon()) { dragonRamSizeUpdate(); return; }
    const int kb = m_ram.kb();
    if (kb == 4) {
        pia1.b.inSink &= (uint8_t)~0x04;
        pia1.b.inSource &= (uint8_t)~0x04;
    } else if (kb == 16) {
        pia1.b.inSink |= 0x04;
        pia1.b.inSource |= 0x04;
    } else {
        const bool pb6 = (pia0.b.ddr & 0x40) ? (pia0.b.out & 0x40) != 0 : true;
        if (pb6) { pia1.b.inSource |= 0x04; pia1.b.inSink |= 0x04; }
        else { pia1.b.inSource &= (uint8_t)~0x04; pia1.b.inSink &= (uint8_t)~0x04; }
        const bool pb2 = (pia1.b.ddr & 0x04) ? (pia1.b.out & 0x04) != 0 : true;
        if (!pb2) pia0.b.inSink &= (uint8_t)~0x40;
    }
}

void System::fetchVideo(int n, uint16_t* out) {
    // D7 is the VDG's A/S, D6 its INV; GM0 (PIA1 PB4) its INT/EXT
    const uint16_t ext = (pia1.portB() & 0x10) ? Mc6847::F_EXT : 0;
    while (n > 0) {
        uint16_t row = 0, col = 0;
        const int k = sam.videoBytes(n, row, col);
        const uint8_t* p = m_ram.at(0, row, col);
        for (int i = 0; i < k; ++i) {
            const uint8_t d = p ? p[i] : 0xFF;
            *out++ = (uint16_t)(d | ext | ((d & 0x80) ? Mc6847::F_AS : 0) | ((d & 0x40) ? Mc6847::F_INV : 0));
        }
        n -= k;
    }
}

float System::soundLevel() const {
    float v = 0.0f;
    if (pia1.cb2()) {                       // the analog mux is on
        const int src = (pia0.cb2() ? 2 : 0) | (pia0.ca2() ? 1 : 0);
        if (src == 0) v = ((float)(pia1.portA() >> 2) - 31.5f) / 63.0f;          // the DAC
        else if (src == 1 && tape.playing()) v = tape.input() ? -0.25f : 0.25f;  // the cassette
    }
    if (pia1.b.ddr & 0x02) v += (pia1.b.out & 0x02) ? 0.25f : 0.0f;              // single-bit sound
    if (m_cart) v += m_cart->audio();
    return std::max(-1.0f, std::min(1.0f, v));
}

void System::soundChanged() { m_level = soundLevel(); }

void System::sync(uint64_t to) {
    if (to <= m_dev) return;
    const uint64_t d = to - m_dev;
    sound.run(d, m_level);
    m_dev = to;
    vdg.advance(d);
    const bool tapeWas = tape.input();
    tape.clock(d);
    if (tape.input()) pia1.a.inSink |= 0x01;
    else pia1.a.inSink &= (uint8_t)~0x01;
    if (tape.input() != tapeWas && tape.playing()) soundChanged();
    if (m_cart) m_cart->clock(d);
    keyboard.clock(d);
    if (dragon()) dragonClock(d);
    else rs232.txLevel(m_dev, pia1.portA() & 2);
}

uint8_t System::access(uint16_t a, bool write, uint8_t v) {
    const uint64_t start = m_ticks + m_instr;
    if (a >= 0xFF00) {
        sync(start);
    } else if (write && a < 0x8000) {
        sync(start);
        vdg.update();                   // the VDG has what it fetched before this write
    }
    const Mc6883::Cycle c = sam.access(a, write);
    m_instr += (uint64_t)c.ticks;
    ++m_accesses;
    const bool cts = c.s == Mc6883::S_CTS;
    const bool scs = c.s == Mc6883::S_IO2;
    const bool cartSees = m_cart && (cts || scs || (a >= 0xFF60 && a < 0xFFC0));
    if (write) {
        if (c.write) {
            if (a >= 0x8000) { sync(start); vdg.update(); }
            if (uint8_t* p = m_ram.at(c.bank, c.row, c.col)) *p = v;
        }
        if (c.s == Mc6883::S_IO0) {
            if (piaDecoded(a)) pia0.write(a, v);
            else if (m_acia && (a & 4)) m_acia->ioWrite(nullptr, 0xFF00u | (a & 7), v);
        } else if (c.s == Mc6883::S_IO1 && piaDecoded(a)) {
            pia1.write(a, v);
        }
        if (cartSees) {
            m_cart->write(a, cts, scs, v);
            soundChanged();
        }
        return v;
    }
    uint8_t d = 0xFF;
    switch (c.s) {
        case Mc6883::S_RAM:
            if (const uint8_t* p = m_ram.at(c.bank, c.row, c.col)) d = *p;
            break;
        case Mc6883::S_ROM0:
        case Mc6883::S_ROM1:
            if (const uint8_t* r = romByte(c.s == Mc6883::S_ROM0 ? (uint16_t)(a & 0x1FFF) : (uint16_t)(0x2000 | (a & 0x1FFF)))) d = *r;
            break;
        case Mc6883::S_IO0:
            if (piaDecoded(a)) d = pia0.read(a);
            else if (m_acia && (a & 4)) m_acia->ioRead(nullptr, 0xFF00u | (a & 7), &d);
            break;
        case Mc6883::S_IO1:
            if (piaDecoded(a)) d = pia1.read(a);
            break;
        default:
            break;
    }
    if (cartSees) {
        uint8_t cd;
        if (m_cart->read(a, cts, scs, cd)) d = cd;
    }
    return d;
}

uint8_t System::read8(uint32_t addr) {
    const uint16_t a = (uint16_t)addr;
    const uint8_t v = access(a, false, 0);
    if (m_observer) m_observer->onMemoryRead(this, a, v);
    return v;
}

void System::write8(uint32_t addr, uint8_t v) {
    const uint16_t a = (uint16_t)addr;
    if (m_observer) m_observer->onMemoryWrite(this, a, peek8(a), v);
    access(a, true, v);
}

uint8_t System::peek8(uint32_t addr) {
    const uint16_t a = (uint16_t)addr;
    const uint8_t s = sam.select(a, false);
    const bool cts = s == Mc6883::S_CTS, scs = s == Mc6883::S_IO2;
    uint8_t d = 0xFF;
    switch (s) {
        case Mc6883::S_RAM: {
            const auto c = sam.decode(a, false);
            if (const uint8_t* p = m_ram.at(c.bank, c.row, c.col)) d = *p;
            break;
        }
        case Mc6883::S_ROM0:
        case Mc6883::S_ROM1:
            if (const uint8_t* r = romByte(s == Mc6883::S_ROM0 ? (uint16_t)(a & 0x1FFF) : (uint16_t)(0x2000 | (a & 0x1FFF)))) d = *r;
            break;
        case Mc6883::S_IO0:
            if (piaDecoded(a)) d = pia0.peek(a);
            else if (m_acia && (a & 4)) m_acia->ioPeek(nullptr, 0xFF00u | (a & 7), &d);
            break;
        case Mc6883::S_IO1: if (piaDecoded(a)) d = pia1.peek(a); break;
        default: break;
    }
    if (m_cart && (cts || scs || (a >= 0xFF60 && a < 0xFFC0))) {
        uint8_t cd;
        if (m_cart->peek(a, cts, scs, cd)) d = cd;
    }
    return d;
}

int System::step() {
    m_instr = 0;
    m_accesses = 0;
    int cycles = 1;
    if (!m_halt && m_cpu) cycles = m_cpu->step();
    // Cycles without a bus access (the 6809's "don't care" cycles, a
    // halted CPU) put $FFFF on the bus
    for (int dead = cycles - m_accesses; dead > 0; --dead) m_instr += (uint64_t)sam.access(0xFFFF, false).ticks;
    m_ticks += m_instr;
    sync(m_ticks);
    updateLines();
    const uint64_t slow = m_ticks / TICKS_SLOW;
    const int r = (int)(slow - m_reported);
    m_reported = slow;
    return r;
}

// Snapshots: RAM, the chips and the clock
size_t System::stateSize() const { return state().size(); }

void System::saveState(uint8_t* buf) const {
    const auto v = state();
    std::memcpy(buf, v.data(), v.size());
}

std::vector<uint8_t> System::state() const {
    std::vector<uint8_t> v;
    putBlob(v, m_ram.m_bank[0]);
    putBlob(v, m_ram.m_bank[1]);
    putBlob(v, sam.saveState());
    putBlob(v, pia0.saveState());
    putBlob(v, pia1.saveState());
    putBlob(v, vdg.saveState());
    put(v, m_ticks);
    put(v, m_dev);
    put(v, m_ram.kb());
    return v;
}

void System::loadState(const uint8_t* buf) {
    const uint8_t* p = buf;
    auto b0 = getBlob(p);
    auto b1 = getBlob(p);
    auto s = getBlob(p);
    auto p0 = getBlob(p);
    auto p1 = getBlob(p);
    auto vd = getBlob(p);
    get(p, m_ticks);
    get(p, m_dev);
    int kb = 64;
    get(p, kb);
    m_ram.configure(kb);
    if (b0.size() == m_ram.m_bank[0].size()) m_ram.m_bank[0] = b0;
    m_ram.m_bank[1] = b1;
    sam.loadState(s);
    pia0.loadState(p0);
    pia1.loadState(p1);
    vdg.loadState(vd);
    m_reported = m_ticks / TICKS_SLOW;
    if (dragon()) dragonRomSelect();
    soundChanged();
    updateLines();
}

System* systemOf(MachineDescriptor* m) {
    if (!m || m->buses.empty()) return nullptr;
    return dynamic_cast<System*>(m->buses[0].bus);
}

// ---------------------------------------------------------------------------
// The machine
// ---------------------------------------------------------------------------
namespace {

bool loadRom(const std::string& dir, const std::vector<std::string>& names, std::vector<uint8_t>& out, std::string& found) {
    for (const auto& n : names) {
        if (readFile(dir + "/" + n, out) && out.size() == 0x2000) {
            found = n;
            return true;
        }
    }
    out.clear();
    return false;
}

// Without Color BASIC: a stand-in at $A000 that says so on the screen
// ($0000, where the SAM shows it at reset) and waits
std::vector<uint8_t> placeholderRom() {
    std::vector<uint8_t> r(0x2000, 0x12);
    const uint8_t code[] = {
        0x8E, 0x00, 0x00,           // LDX #$0000
        0x86, 0x60,                 // LDA #$60 (space)
        0xA7, 0x80,                 // STA ,X+
        0x8C, 0x02, 0x00,           // CMPX #$0200
        0x26, 0xF9,                 // BNE
        0x8E, 0x00, 0x00,           // LDX #$0000
        0x10, 0x8E, 0xA0, 0x20,     // LDY #$A020
        0xA6, 0xA0,                 // LDA ,Y+
        0x27, 0x04,                 // BEQ done
        0xA7, 0x80,                 // STA ,X+
        0x20, 0xF8,                 // BRA
        0x20, 0xFE,                 // done: BRA *
    };
    std::memcpy(r.data(), code, sizeof code);
    const char* msg = "NO COLOR BASIC ROM: PUT BAS13.ROM IN ROMS/COCO/ (SEE DOC/README-COCO.MD)";
    for (size_t i = 0; msg[i]; ++i) r[0x20 + i] = (uint8_t)msg[i];
    r[0x20 + std::strlen(msg)] = 0;
    r[0x1FFE] = 0xA0;               // reset vector: $A000
    r[0x1FFF] = 0x00;
    return r;
}

std::unique_ptr<Cart> makeCart(const std::string& device, const std::string& option, std::string& err) {
    if (device == "orch90") {
        std::vector<uint8_t> rom;
        const std::string path = option.empty() ? firmwareDir() + "/orch90.rom" : option;
        if (!readFile(path, rom) && !option.empty()) { err = "can't read " + option; return nullptr; }
        return std::unique_ptr<Cart>(new Orchestra90(std::move(rom)));
    }
    if (device == "rs232") return std::unique_ptr<Cart>(new DeluxeRs232());
    if (device == "fd500") {
        std::vector<uint8_t> rom;
        std::string found;
        if (!option.empty()) {
            if (!readFile(option, rom) || rom.empty()) { err = "can't read Disk BASIC ROM " + option; return nullptr; }
        } else if (!loadRom(firmwareDir(), {"disk11.rom", "disk10.rom"}, rom, found)) {
            err = "Disk BASIC ROM not found: put disk11.rom (Disk Extended Color BASIC 1.1, 8 KB) in " + firmwareDir() +
                  "/ (or set MMEMU_COCO_FIRMWARE), or give its path";
            return nullptr;
        }
        return std::unique_ptr<Cart>(new Fd500(std::move(rom)));
    }
    if (device == "dragondos") {
        std::vector<uint8_t> rom = loadDragonDosRom(option, err);
        if (rom.empty()) return nullptr;
        return std::unique_ptr<Cart>(new DragonDos(std::move(rom)));
    }
    if (device == "rompak") {
        std::vector<uint8_t> rom;
        if (option.empty() || !readFile(option, rom) || rom.empty()) { err = "rompak needs the image's path"; return nullptr; }
        const auto slash = option.find_last_of('/');
        return std::unique_ptr<Cart>(new RomPak(std::move(rom), slash == std::string::npos ? option : option.substr(slash + 1)));
    }
    err = "no cartridge '" + device + "' (mpi, orch90, rs232, fd500, dragondos, rompak)";
    return nullptr;
}

}  // namespace

MachineDescriptor* create(Model model, std::string* err, int ramKb, const Roms* roms, Tv tv) {
    std::vector<uint8_t> color, ext, dragon, dragon64k;
    std::string missing, notice;
    const bool isD = isDragon(model);
    if (isD && roms) {
        dragon = roms->dragonBasic;
        dragon64k = roms->dragon64k;
        if (dragon.empty()) { if (err) *err = "no Dragon BASIC image"; return nullptr; }
    } else if (isD) {
        if (!loadDragonRoms(model, dragon, dragon64k, missing)) {
            if (err) *err = missing;
            std::fprintf(stderr, "%s\n", missing.c_str());
            dragon = dragonPlaceholderRom();
        } else {
            notice = missing;                 // the Dragon 64 without its 64K-mode BASIC
            missing.clear();
        }
    } else if (roms) {
        color = roms->colorBasic;
        ext = roms->extBasic;
        if (color.empty()) { if (err) *err = "no Color BASIC image"; return nullptr; }
    } else {
        const std::string dir = firmwareDir();
        const std::vector<std::string> colorNames = model == Model::CoCo1
            ? std::vector<std::string>{"bas12.rom", "bas13.rom", "bas11.rom", "bas10.rom"}
            : std::vector<std::string>{"bas13.rom", "bas12.rom", "bas11.rom", "bas10.rom"};
        std::string found;
        if (!loadRom(dir, colorNames, color, found)) {
            missing = "Color BASIC ROM not found: put " + colorNames[0] + " (Color BASIC, 8 KB; also tried " +
                      colorNames[1] + ", " + colorNames[2] + ", " + colorNames[3] + ") in " + dir +
                      "/ or set MMEMU_COCO_FIRMWARE to its directory";
            if (err) *err = missing;
            std::fprintf(stderr, "%s\n", missing.c_str());
            color = placeholderRom();
        } else {
            loadRom(dir, {"extbas11.rom", "extbas10.rom"}, ext, found);
        }
    }

    const bool haveExt = !ext.empty();
    auto* sys = new System(model, ramKb, std::move(ext), std::move(color), tv);
    if (isD) sys->setDragonRoms(dragon, dragon64k);
    const bool pal = sys->pal();

    auto* desc = new MachineDescriptor();
    switch (model) {
        case Model::CoCo1: desc->machineId = "coco1"; desc->displayName = "TRS-80 Color Computer"; break;
        case Model::CoCo2: desc->machineId = "coco2"; desc->displayName = "TRS-80 Color Computer 2"; break;
        case Model::CoCo2B: desc->machineId = "coco2b"; desc->displayName = "TRS-80 Color Computer 2B"; break;
        case Model::Dragon32: desc->machineId = "dragon32"; desc->displayName = "Dragon 32"; break;
        case Model::Dragon64:
            desc->machineId = pal ? "dragon64" : "tano";
            desc->displayName = pal ? "Dragon 64" : "Tano Dragon (NTSC Dragon 64)";
            break;
    }
    if (!isD && pal) desc->displayName += " (PAL)";
    if (model == Model::Dragon32 && !pal) desc->displayName += " (NTSC)";
    desc->licenseClass = "proprietary";
    desc->uiPanesSet = true;
    desc->uiPanes = {"tape", "cartridge"};
    desc->clockHz = sys->masterHz() / TICKS_SLOW;
    if (!missing.empty()) desc->warnings.push_back(missing);
    else if (!notice.empty()) desc->warnings.push_back(notice);
    else if (!isD && !haveExt && !roms)
        desc->warnings.push_back("Extended Color BASIC (extbas11.rom) not found in " + firmwareDir() + ": Color BASIC only");

    auto* cpu = new M6809();
    cpu->setDataBus(sys);
    sys->setCpu(cpu);
    desc->buses.push_back({"system", sys});
    desc->cpus.push_back({"main", cpu, sys, sys, nullptr, true, 1});

    auto* io = new IORegistry();
    desc->ioRegistry = io;
    io->registerHandler(&sys->display);
    io->registerHandler(&sys->keyboard);
    io->registerHandler(&sys->sound);
    io->registerHandler(&sys->tape);
    if (!isD) io->registerHandler(&sys->rs232);
    if (sys->acia()) io->registerHandler(&sys->aciaPort);
    if (isD) io->registerHandler(&sys->printer);
    io->registerHandler(&sys->cartPort);

    // Keys; RESET and the artifact colours as buttons under the screen
    desc->onKey = [sys](const std::string& name, bool down) {
        if (name == "reset") { if (down) sys->resetButton(); return true; }
        if (name == "artifact") {
            if (down) {
                const auto a = sys->vdg.artifact();
                sys->vdg.setArtifact(a == Mc6847::Artifact::Off ? Mc6847::Artifact::BlueRed
                                     : a == Mc6847::Artifact::BlueRed ? Mc6847::Artifact::RedBlue : Mc6847::Artifact::Off);
            }
            return true;
        }
        return sys->keyboard.key(name, down);
    };
    desc->screenButtons = {
        {"RESET", {"reset"}, "The RESET button: BASIC restarts; the program in memory stays."},
    };
    // A PAL set shows no artifact colours
    if (!pal) desc->screenButtons.push_back({"Artifact", {"artifact"}, "PMODE 4 artifact colours: off, blue-red, red-blue (NTSC phase)."});

    // Joysticks: 0 right, 1 left (the right one is most games')
    desc->joystickPorts = 2;
    desc->primaryJoystickPort = 0;
    desc->onJoystick = [sys](int port, uint8_t bits) {
        sys->setAxis(port, 0, !(bits & 0x04) ? 0 : !(bits & 0x08) ? 65535 : 32768);
        sys->setAxis(port, 1, !(bits & 0x01) ? 0 : !(bits & 0x02) ? 65535 : 32768);
        sys->setButton(port, !(bits & 0x10));
    };
    // The mouse moves the right joystick (a host pixel = 256 steps of 65536)
    desc->onMouse = [sys](int dx, int dy, uint8_t buttons, int) {
        for (int axis = 0; axis < 2; ++axis) {
            const int d = axis ? dy : dx;
            const int v = std::max(0, std::min(65535, (int)sys->axis(0, axis) + d * 256));
            sys->setAxis(0, axis, (uint16_t)v);
        }
        sys->setButton(0, buttons & 1);
    };

    // RAM: 4, 16, 32 or 64 KB
    {
        MachineDescriptor::ExpansionSlot s;
        s.name = "ram";
        s.description = "RAM fitted: 4K (4K x1 chips), 16K (16K x1), 32K or 64K (64K x1); BASIC finds the size at power on";
        s.devices = {{"ram", "The RAM chips", {"4K", "16K", "32K", "64K"}, "64K"}};
        s.canEmpty = false;
        s.install = [sys](const std::string& device, const std::string& option, std::string& err) {
            if (device != "ram") { err = "the ram slot takes 'ram' (4K, 16K, 32K, 64K)"; return false; }
            const uint64_t bytes = MachineDescriptor::parseMemorySize(option.empty() ? "64K" : option);
            if (bytes != 4096 && bytes != 16384 && bytes != 32768 && bytes != 65536) { err = "4K, 16K, 32K or 64K"; return false; }
            sys->setRamKb((int)(bytes / 1024));
            return true;
        };
        s.info = [sys]() { return "ram (" + std::to_string(sys->ramKb()) + "K)"; };
        desc->expansionSlots.push_back(std::move(s));
    }
    // The cartridge port: a Multi-Pak, the Orchestra-90, the Deluxe RS-232
    // Pak or the FD-500 (ROM Paks with `cart`)
    {
        MachineDescriptor::ExpansionSlot s;
        s.name = "cart";
        s.description = "The cartridge port (ROM Paks: the cart command)";
        if (isD) {
            s.devices = {
                {"dragondos", "DragonDOS disk controller (WD2797 at $FF40, latch $FF48); option: the ROM", {}, ""},
                {"rompak", "A ROM cartridge; option: the image (.rom, .dgn, .ccc)", {}, ""},
            };
        } else {
            s.devices = {
                {"mpi", "Multi-Pak Interface: 4 slots (expansion mpi1-mpi4); option: the switch position", {"1", "2", "3", "4"}, "4"},
                {"orch90", "Orchestra-90 stereo DACs ($FF7A/$FF7B); option: its ROM image", {}, ""},
                {"rs232", "Deluxe RS-232 Program Pak (6551 ACIA at $FF68; the serial command)", {}, ""},
                {"fd500", "FD-500 disk controller (WD2793) with Disk BASIC; option: the ROM", {}, ""},
                {"rompak", "A ROM Pak; option: the image (.rom, .ccc)", {}, ""},
            };
        }
        s.install = [sys, isD](const std::string& device, const std::string& option, std::string& err) {
            if (device.empty()) { sys->setCart(nullptr); return true; }
            const bool fits = isD ? (device == "dragondos" || device == "rompak") : device != "dragondos";
            if (!fits) { err = "no cartridge '" + device + "' for this machine"; return false; }
            if (device == "mpi") {
                const int pos = option.empty() ? 4 : std::atoi(option.c_str());
                if (pos < 1 || pos > 4) { err = "the switch is 1-4"; return false; }
                auto* mpi = dynamic_cast<MultiPak*>(sys->cart());
                if (!mpi) {
                    auto fresh = std::unique_ptr<MultiPak>(new MultiPak());
                    fresh->setSwitch(pos - 1);
                    // What was in the port moves to the switch's slot
                    if (auto old = sys->takeCart()) fresh->insert(pos - 1, std::move(old));
                    sys->setCart(std::move(fresh));
                } else {
                    mpi->setSwitch(pos - 1);
                }
                return true;
            }
            auto c = makeCart(device, option, err);
            if (!c) return false;
            sys->setCart(std::move(c));
            return true;
        };
        s.info = [sys]() { return sys->cart() ? sys->cart()->describe() : std::string(); };
        desc->expansionSlots.push_back(std::move(s));
    }
    for (int n = 0; n < (isD ? 0 : 4); ++n) {
        MachineDescriptor::ExpansionSlot s;
        s.name = "mpi" + std::to_string(n + 1);
        s.description = "Multi-Pak slot " + std::to_string(n + 1) + " (with expansion cart mpi)";
        s.devices = {
            {"orch90", "Orchestra-90; option: its ROM image", {}, ""},
            {"rs232", "Deluxe RS-232 Program Pak", {}, ""},
            {"fd500", "FD-500 disk controller (WD2793) with Disk BASIC; option: the ROM", {}, ""},
            {"rompak", "A ROM Pak; option: the image (.rom, .ccc)", {}, ""},
        };
        s.install = [sys, n](const std::string& device, const std::string& option, std::string& err) {
            auto* mpi = dynamic_cast<MultiPak*>(sys->cart());
            if (!mpi) { err = "no Multi-Pak in the cartridge port (expansion cart mpi)"; return false; }
            if (device.empty()) { mpi->insert(n, nullptr); return true; }
            auto c = makeCart(device, option, err);
            if (!c) return false;
            mpi->insert(n, std::move(c));
            return true;
        };
        s.info = [sys, n]() -> std::string {
            auto* mpi = dynamic_cast<MultiPak*>(sys->cart());
            Cart* c = mpi ? mpi->slot(n) : nullptr;
            return c ? c->describe() : std::string();
        };
        desc->expansionSlots.push_back(std::move(s));
    }

    desc->symbolFiles.push_back(PathUtil::findResource(isD ? "roms/dragon/hardware.sym" : "roms/coco/hardware.sym"));

    desc->onReset = [sys](MachineDescriptor&) { sys->powerOn(); };
    desc->schedulerStep = [sys](MachineDescriptor&) -> int { return sys->step(); };
    desc->deviceCycles = [sys]() { return sys->slowCycles(); };
    desc->onReset(*desc);
    return desc;
}

bool insertRomPak(MachineDescriptor* m, const std::vector<uint8_t>& image, const std::string& label, std::string& err) {
    System* sys = systemOf(m);
    if (!sys) { err = "not a Color Computer"; return false; }
    if (image.empty()) { err = "empty image"; return false; }
    std::unique_ptr<Cart> pak(new RomPak(image, label));
    if (auto* mpi = dynamic_cast<MultiPak*>(sys->cart())) mpi->insert(mpi->switchPosition(), std::move(pak));
    else sys->setCart(std::move(pak));
    return true;
}

void removeRomPak(MachineDescriptor* m) {
    System* sys = systemOf(m);
    if (!sys) return;
    if (auto* mpi = dynamic_cast<MultiPak*>(sys->cart())) {
        for (int i = 0; i < 4; ++i)
            if (mpi->slot(i) && mpi->slot(i)->kind() == "rompak") mpi->insert(i, nullptr);
    } else if (sys->cart() && sys->cart()->kind() == "rompak") {
        sys->setCart(nullptr);
    }
}

std::string cartridgeInfo(MachineDescriptor* m) {
    System* sys = systemOf(m);
    return sys && sys->cart() ? sys->cart()->describe() : std::string();
}

}  // namespace coco
