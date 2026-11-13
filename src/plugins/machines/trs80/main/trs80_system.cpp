// The TRS-80 Model I, III and 4: memory maps, ports and interrupt wiring
// after the Radio Shack technical reference manuals, xtrs (Tim Mann;
// trs_memory.c, trs_io.c, trs_interrupt.c, trs_disk.c; MIT-style licence)
// and MAME (trs80.cpp, trs80m3.cpp; BSD-3-Clause). mmsim's own code.

#include "trs80_system.h"
#include "plugins/z80/main/cpu_z80.h"
#include "libcore/main/machine_desc.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>

namespace trs80 {

namespace {

template <class T> void put(std::vector<uint8_t>& v, const T& x) {
    const auto* p = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), p, p + sizeof(T));
}
template <class T> void get(const uint8_t*& p, T& x) {
    std::memcpy(&x, p, sizeof(T));
    p += sizeof(T);
}

// Key codes: row << 3 | bit
constexpr int K_SHIFT = 7 << 3 | 0;
constexpr int K_RSHIFT = 7 << 3 | 1;
constexpr int SHIFTED = 0x100;
constexpr double KEY_HOLD = 0.06, KEY_GAP = 0.04;      // typed keys: 60 ms down, 40 ms up

int charCode(char c) {
    if (c >= 'A' && c <= 'Z') return c - '@';
    if (c >= 'a' && c <= 'z') return (c - '`') | SHIFTED;
    if (c == '@') return 0;
    if (c >= '0' && c <= '9') return 32 + (c - '0');
    switch (c) {
    case ':': return 42; case ';': return 43; case ',': return 44; case '-': return 45;
    case '.': return 46; case '/': return 47;
    case '\n': case '\r': return 48;
    case ' ': return 55;
    case '!': return 33 | SHIFTED; case '"': return 34 | SHIFTED; case '#': return 35 | SHIFTED;
    case '$': return 36 | SHIFTED; case '%': return 37 | SHIFTED; case '&': return 38 | SHIFTED;
    case '\'': return 39 | SHIFTED; case '(': return 40 | SHIFTED; case ')': return 41 | SHIFTED;
    case '*': return 42 | SHIFTED; case '+': return 43 | SHIFTED; case '<': return 44 | SHIFTED;
    case '=': return 45 | SHIFTED; case '>': return 46 | SHIFTED; case '?': return 47 | SHIFTED;
    default: return -1;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// The I/O space as a bus
// ---------------------------------------------------------------------------
namespace {
class IoBus : public IBus {
public:
    explicit IoBus(System* s) : m_sys(s) { m_cfg = {16, 8, BusRole::IO_PORT, true, 0xFFFF}; }
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
    std::memset(m_rows, 0, sizeof m_rows);
    m_shifts = 0;
    m_queue.clear();
    m_typing = -1;
    m_wait = 0;
}

void Keyboard::set(int row, int bit, bool down) {
    if (row < 0 || row > 7 || bit < 0 || bit > 7) return;
    if (down) m_rows[row] |= (uint8_t)(1 << bit);
    else m_rows[row] &= (uint8_t)~(1 << bit);
}

uint8_t Keyboard::read(uint16_t addr) const {
    uint8_t v = 0;
    for (int r = 0; r < 8; ++r) {
        if (!(addr & (1 << r))) continue;
        v |= m_rows[r];
        if (r == 7 && m_shifts > 0) v |= 0x01;
    }
    return v;
}

void Keyboard::press(int code, bool down) {
    if (code & SHIFTED) m_shifts = std::max(0, m_shifts + (down ? 1 : -1));
    set((code >> 3) & 7, code & 7, down);
}

bool Keyboard::key(std::string n, bool down) {
    for (auto& c : n) c = (char)std::tolower((unsigned char)c);
    static const std::map<std::string, int> keys = {
        {"at", 0}, {"colon", 42}, {"semicolon", 43}, {"comma", 44}, {"minus", 45}, {"period", 46}, {"slash", 47},
        {"return", 48}, {"enter", 48}, {"kp_enter", 48}, {"clear", 49}, {"home", 49}, {"break", 50}, {"escape", 50},
        {"esc", 50}, {"run_stop", 50}, {"end", 50}, {"up", 51}, {"down", 52}, {"left", 53}, {"backspace", 53},
        {"delete", 53}, {"right", 54}, {"tab", 54}, {"space", 55},
        {"shift", K_SHIFT}, {"left_shift", K_SHIFT}, {"lshift", K_SHIFT},
        // The PC's shifted symbols, as the GUI names them: SHIFT and the TRS-80 key
        {"exclaim", 33 | SHIFTED}, {"dquote", 34 | SHIFTED}, {"hash", 35 | SHIFTED}, {"dollar", 36 | SHIFTED},
        {"percent", 37 | SHIFTED}, {"ampersand", 38 | SHIFTED}, {"squote", 39 | SHIFTED}, {"quote", 39 | SHIFTED},
        {"apostrophe", 39 | SHIFTED}, {"lparen", 40 | SHIFTED}, {"rparen", 41 | SHIFTED},
        {"asterisk", 42 | SHIFTED}, {"plus", 43 | SHIFTED}, {"less", 44 | SHIFTED}, {"equal", 45 | SHIFTED},
        {"equals", 45 | SHIFTED}, {"kpeq", 45 | SHIFTED}, {"greater", 46 | SHIFTED}, {"question", 47 | SHIFTED},
    };
    int code = -1;
    if (n.size() == 1 && std::isalpha((unsigned char)n[0])) code = std::toupper((unsigned char)n[0]) - '@';
    else if (n.size() == 1 && std::isdigit((unsigned char)n[0])) code = 32 + (n[0] - '0');
    else if (n == "right_shift" || n == "rshift") code = m_model3 ? K_RSHIFT : K_SHIFT;
    else if (m_model4 && (n == "ctrl" || n == "control" || n == "left_ctrl" || n == "right_ctrl")) code = 7 << 3 | 2;
    else if (m_model4 && (n == "caps_lock" || n == "caps")) code = 7 << 3 | 3;
    else if (m_model4 && n.size() == 2 && n[0] == 'f' && n[1] >= '1' && n[1] <= '3') code = (7 << 3) | (4 + n[1] - '1');
    else {
        auto it = keys.find(n);
        if (it == keys.end()) return false;
        code = it->second;
    }
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
// Sound
// ---------------------------------------------------------------------------
void Sound::run(uint64_t ticks, float level) {
    while (ticks) {
        const uint64_t n = std::min<uint64_t>(ticks, (uint64_t)m_ticksPerSample - m_acc);
        m_sum += level * (double)n;
        m_acc += n;
        ticks -= n;
        if (m_acc >= (uint64_t)m_ticksPerSample) {
            if (m_samples.size() < 44100) m_samples.push_back((float)(m_sum / m_ticksPerSample));
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
// The screen: Model I 64 x 16 of 6 x 12 dots; III/4 64 x 16 of 8 x 12; 4 80 x
// 24 of 8 x 10. Each line is shown twice (the monitors' aspect).
// ---------------------------------------------------------------------------
namespace {
constexpr uint32_t FG = 0xFFF0F0F0, BG = 0xFF000000;
}

IVideoOutput::VideoDimensions Display::getDimensions() const {
    switch (m_sys->model()) {
    case Model::M1Level1: case Model::M1Level2: return {384, 384, 384, 384};
    case Model::M3: return {512, 384, 512, 384};
    default: return {640, 480, 640, 480};
    }
}

void Display::renderFrame(uint32_t* buf) {
    const auto dim = getDimensions();
    const int W = dim.width, H = dim.height / 2;          // H logical lines, each drawn twice
    std::fill(buf, buf + (size_t)W * dim.height, BG);
    const bool m1 = isModel1(m_sys->model());
    const bool m80 = m_sys->mode80();
    const int cols = m80 ? 80 : 64, rows = m80 ? 24 : 16, lines = m80 ? 10 : 12;
    const int cw = m1 ? 6 : 8, half = cw / 2;
    const int x0 = (W - cols * cw) / 2, y0 = (H - rows * lines) / 2;
    const bool wide = m_sys->wide(), inv = m_sys->inverse(), alt = m_sys->altChars();
    const int vmask = isModel4(m_sys->model()) ? 0x7FF : 0x3FF;
    const uint16_t start = isModel4(m_sys->model()) ? m_sys->crtcStart() : 0;
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; col += wide ? 2 : 1) {
            const uint8_t c = m_sys->videoByte((start + row * cols + col) & vmask);
            for (int ra = 0; ra < lines; ++ra) {
                uint8_t dots;
                if (inv) {
                    dots = m_sys->glyph(c & 0x7F, ra);
                    if (c & 0x80) dots = (uint8_t)~dots;
                } else if (!m1 && (c & 0xC0) == 0xC0) {
                    dots = m_sys->glyph(alt ? (c & 0xBF) : c, ra);
                } else if (c & 0x80) {
                    // 2 x 3 blocks: bits 0/1 the top pair, 2/3 the middle, 4/5 the bottom
                    const int third = (ra & 0x0C) >> 2;
                    const bool l = (c >> (third * 2)) & 1, r = (c >> (third * 2 + 1)) & 1;
                    dots = (uint8_t)((l ? (0xFF << (8 - half)) : 0) | (r ? (0xFF >> half) & (0xFF << (8 - cw)) : 0));
                } else {
                    dots = m_sys->glyph(c & 0x7F, ra);
                }
                const int y = y0 + row * lines + ra;
                uint32_t* line0 = buf + (size_t)(y * 2) * W;
                uint32_t* line1 = line0 + W;
                for (int x = 0; x < cw; ++x) {
                    const uint32_t px = (dots & (0x80 >> x)) ? FG : BG;
                    const int reps = wide ? 2 : 1;
                    for (int k = 0; k < reps; ++k) {
                        const int sx = x0 + col * cw + x * reps + k;
                        if (sx < 0 || sx >= W) continue;
                        line0[sx] = px;
                        line1[sx] = px;
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The floppy drives as the frontends see them: unit 1-4 = drive :0-:3
// ---------------------------------------------------------------------------
bool DiskPort::mountDisk(int unit, const std::string& path) { return m_sys->mountDisk(unit - 1, path); }

void DiskPort::ejectDisk(int unit) { m_sys->ejectDisk(unit - 1); }

bool DiskPort::getDiskStatus(int unit, int& track, int& sector, bool& led) {
    if (!m_sys->hasFdc() || unit < 1 || unit > 4) return false;
    const int d = unit - 1;
    track = m_sys->fdc().driveCylinder(d);
    sector = m_sys->fdc().sectorRegister();
    led = m_sys->fdc().motor() && m_sys->fdc().selectedDrive() == d;
    return true;
}

std::string DiskPort::getMountedDiskPath(int unit) { return m_sys->diskPath(unit - 1); }

void DiskPort::getDeviceInfo(DeviceInfo& out) const {
    m_sys->fdc().getDeviceInfo(out);
    for (int d = 0; d < 4; ++d)
        if (!m_sys->diskPath(d).empty()) out.state.push_back({"Drive :" + std::to_string(d), m_sys->diskPath(d)});
}

// ---------------------------------------------------------------------------
// The machine
// ---------------------------------------------------------------------------
System::System(Model model, const Config& cfg, std::vector<uint8_t> rom)
    : m_model(model), m_rom(std::move(rom)) {
    const bool m1 = isModel1(model);
    m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF};
    m_io = new IoBus(this);
    m_masterHz = m1 ? M1_MASTER_HZ : M3_MASTER_HZ;
    m_baseTicks = m_ticksPerT = m1 ? M1_TICKS : M3_TICKS;
    m_fdc.reset(new Wd17xx(m1 ? Wd17xx::Variant::FD1771 : Wd17xx::Variant::FD1793, m1 ? 0x37EC : 0xF0));
    m_fdc->setClockHz((uint32_t)m_masterHz);
    m_fdc->setReadyMode(Wd17xx::ReadyMode::DiskAndMotor);
    m_fdc->onIntrq([this](bool on) {
        m_intrq = on;
        if (isModel1(m_model)) {
            if (on) m_intLatch |= 0x40; else m_intLatch &= (uint8_t)~0x40;
            updateIrq();
        } else {
            if (on) { m_nmiLatch |= 0x80; m_wait = false; } else m_nmiLatch &= (uint8_t)~0x80;
            updateNmi();
        }
    });
    m_fdc->onDrq([this](bool on) {
        m_drq = on;
        if (on) m_wait = false;
    });
    tape.setMasterHz(m_masterHz);
    tape.speed = [this]() { return tapeSpeed(); };
    tape.onEdge = [this](bool rise) {
        if (isModel1(m_model)) return;
        // The 1500 baud edges are clocked into the latch if enabled
        const uint8_t bit = rise ? 0x01 : 0x02;
        m_intLatch = (uint8_t)((m_intLatch & ~bit) | (m_intMask & bit));
        updateIrq();
    };
    sound.setMasterHz(m_masterHz);
    uart.setMasterHz(m_masterHz);
    uart.setSwitches(m1 ? 0x6F : 0xFF);           // Model I: 8 bits, no parity, 9600 baud
    uart.onInterrupt = [this](bool rcv, bool snd, bool errs) {
        if (isModel1(m_model)) return;
        m_intLatch = (uint8_t)((m_intLatch & 0x8F) | (rcv ? 0x20 : 0) | (snd ? 0x10 : 0) | (errs ? 0x40 : 0));
        updateIrq();
    };
    keyboard.setModel3(!m1);
    keyboard.setModel4(isModel4(model));
    buildFont({});
    configure(cfg);
}

System::~System() { m_fdc->flush(); }

void System::configure(const Config& cfg) {
    m_config = cfg;
    if (m_model == Model::M1Level1) { m_config.expansion = false; m_config.doubler = Doubler::None; }
    int kb = cfg.ramKb;
    if (isModel4(m_model)) kb = kb > 64 ? 128 : 64;
    else if (isModel1(m_model)) {
        kb = kb <= 4 ? 4 : kb <= 16 ? 16 : kb <= 32 ? 32 : 48;
        if (!m_config.expansion && kb > 16) kb = 16;     // over 16K is in the EI
    } else kb = kb <= 16 ? 16 : kb <= 32 ? 32 : 48;
    m_config.ramKb = kb;
    m_ram.assign((size_t)kb * 1024, 0);
}

bool System::hasFdc() const {
    if (isModel1(m_model)) return m_config.expansion;
    if (m_model == Model::M3) return m_config.fdc;
    return true;
}

void System::setCharGen(const std::vector<uint8_t>& rom) { buildFont(rom); }

void System::buildFont(const std::vector<uint8_t>& gen) {
    std::memset(m_font, 0, sizeof m_font);
    if (isModel1(m_model)) {
        if (gen.size() >= 1024) {
            // MCM6670: 8 rows of bits 5-0; g j p q y , ; drop two lines
            for (int c = 0; c < 128; ++c) {
                const bool desc = c == 0x2C || c == 0x3B || c == 0x67 || c == 0x6A || c == 0x70 || c == 0x71 || c == 0x79;
                for (int r = 0; r < 8; ++r) m_font[c][r + (desc ? 2 : 0)] = (uint8_t)((gen[(size_t)c * 8 + r] & 0x3F) << 2);
            }
        } else {
            for (int c = 0; c < 128; ++c)
                for (int r = 0; r < 12; ++r) m_font[c][r] = (uint8_t)(BUILTIN_FONT[c][r] << 2);
        }
        return;
    }
    if (gen.size() >= 2048) {
        for (int c = 0; c < 256; ++c)
            for (int r = 0; r < 8; ++r) m_font[c][r] = gen[(size_t)c * 8 + r];
        return;
    }
    for (int c = 0; c < 128; ++c)
        for (int r = 0; r < 12; ++r) m_font[c][r] = (uint8_t)(BUILTIN_FONT[c][r] << 1);
    // Stand-ins for the special characters: $C0-$FF as $40-$7F inverted,
    // the alternate set ($80-$BF, reached from $C0-$FF) underlined
    for (int i = 0; i < 64; ++i) {
        for (int r = 0; r < 12; ++r) {
            m_font[0xC0 + i][r] = (uint8_t)~m_font[0x40 + i][r];
            m_font[0x80 + i][r] = m_font[0x40 + i][r];
        }
        m_font[0x80 + i][9] = 0x7E;
    }
}

// Interrupts --------------------------------------------------------------
void System::updateIrq() {
    if (!m_cpu) return;
    const bool on = isModel1(m_model) ? (m_config.expansion && m_intLatch != 0) : (m_intLatch & m_intMask) != 0;
    m_cpu->setIrqLine(on);
}

void System::updateNmi() {
    if (!m_cpu) return;
    const bool on = isModel1(m_model) ? m_resetNmi : ((m_nmiLatch & m_nmiMask) != 0 || m_resetNmi);
    m_cpu->setNmiLine(on);
}

Tape::Speed System::tapeSpeed() const {
    if (m_model == Model::M1Level1) return Tape::Speed::Baud250;
    if (!isModel1(m_model) && (m_intMask & 0x03)) return Tape::Speed::Baud1500;
    return Tape::Speed::Baud500;
}

void System::setFast(bool on) {
    if (!isModel4(m_model)) return;
    m_ticksPerT = on ? 5 : 10;
}

bool System::wide() const { return isModel1(m_model) ? (m_portFF & 0x08) : (m_portEC & 0x04); }

float System::soundLevel() const {
    static const float LEVEL[4] = {0.0f, 0.4f, -0.4f, 0.0f};
    float v = tape.motor() ? 0.0f : LEVEL[m_cassOut & 3];
    if (isModel4(m_model) && m_soundBit) v += 0.4f;
    return v;
}

// Time --------------------------------------------------------------------
void System::advance(uint64_t t) {
    m_ticks += t;
    m_fdc->tick(t);
    tape.advance(m_ticks);
    uart.clock(t);
    keyboard.clock(t, m_masterHz);
    sound.run(t, soundLevel());

    // The heartbeat: Model I EI 40 Hz; III 30 Hz; 4 30 or 60 Hz
    const bool m1 = isModel1(m_model);
    if (!m1 || m_config.expansion) {
        const double hz = m1 ? 40.0 : (isModel4(m_model) && fast()) ? 60.0 : 30.0;
        const uint64_t period = (uint64_t)(m_masterHz / hz);
        m_rtcAcc += t;
        if (m_rtcAcc >= period) {
            m_rtcAcc %= period;
            m_intLatch |= m1 ? 0x80 : 0x04;
            updateIrq();
        }
    }
    // The drive motors' one-shot
    if (m_motorLeft) {
        if (m_motorLeft > t) m_motorLeft -= t;
        else {
            m_motorLeft = 0;
            m_fdc->setMotor(false);
            if (!m1) {
                m_nmiLatch |= 0x40;
                m_wait = false;
                updateNmi();
            }
        }
    }
    if (m_resetNmiLeft) {
        if (m_resetNmiLeft > t) m_resetNmiLeft -= t;
        else { m_resetNmiLeft = 0; m_resetNmi = false; updateNmi(); }
    }
}

int System::step() {
    uint64_t t;
    if (m_wait || !m_cpu) t = 4ull * (uint64_t)m_ticksPerT;     // WAIT: the Z80 holds its bus cycle
    else t = (uint64_t)m_cpu->step() * (uint64_t)m_ticksPerT;
    advance(t);
    const uint64_t dc = m_ticks / (uint64_t)m_baseTicks;
    const int r = (int)(dc - m_reported);
    m_reported = dc;
    return r;
}

void System::powerOn() {
    std::fill(m_ram.begin(), m_ram.end(), 0);
    std::fill(std::begin(m_video), std::end(m_video), 0x20);
    m_portFF = m_portEC = m_port84 = 0;
    m_crtcReg = 0;
    m_crtcStart = 0;
    keyboard.reset();
    hardwareReset();
    if (m_cpu) m_cpu->reset();
    updateIrq();
    updateNmi();
}

void System::resetButton() {
    hardwareReset();
    if (isModel1(m_model)) {
        // The Model I's button pulls /NMI (Level II restarts from its handler)
        m_resetNmi = true;
        m_resetNmiLeft = 2000ull * (uint64_t)m_ticksPerT;
    } else if (m_cpu) {
        m_cpu->reset();                         // the III/4's resets the Z80
    }
    updateIrq();
    updateNmi();
}

void System::hardwareReset() {
    m_romIn = true;
    m_port84 = 0;
    m_soundBit = false;
    m_intLatch = 0;
    m_intMask = 0;
    m_nmiLatch = 0x01;
    m_nmiMask = 0x20;
    m_resetNmi = false;
    m_resetNmiLeft = 0;
    m_rtcAcc = 0;
    m_ticksPerT = m_baseTicks;
    m_fdc->reset();
    m_fdc->writeRegister(0, 0xD0);            // the master reset's Restore ended at once: no INTRQ
    m_drq = m_intrq = m_wait = false;
    m_motorLeft = 0;
    m_fdc->setMotor(false);
    m_fdc->selectDrive(-1);
    m_select = 0;
    if (isModel1(m_model)) setDensity(false);
    else m_fdc->setDoubleDensity(false);
    m_cassOut = 0;
    tape.setMotor(m_ticks, false);
    uart.reset();
}

// Memory ------------------------------------------------------------------
uint8_t System::read8(uint32_t addr) { return access((uint16_t)addr, false, 0, false); }
void System::write8(uint32_t addr, uint8_t v) { access((uint16_t)addr, true, v, false); }
uint8_t System::peek8(uint32_t addr) { return access((uint16_t)addr, false, 0, true); }

uint8_t System::access(uint16_t a, bool write, uint8_t v, bool peek) {
    if (isModel1(m_model)) return m1Access(a, write, v, peek);
    if (m_model == Model::M3) return m3Access(a, write, v, peek);
    return m4Access(a, write, v, peek);
}

void System::videoWrite(uint16_t i, uint8_t v) {
    // The Model I without the lower case mod stores no D6: it reads back as NOT (D5 or D7)
    if (isModel1(m_model) && !m_config.lowercase) v = (v & 0xA0) ? (uint8_t)(v & 0xBF) : (uint8_t)(v | 0x40);
    m_video[i & 0x7FF] = v;
}

uint8_t System::m1Access(uint16_t a, bool write, uint8_t v, bool peek) {
    if (a >= 0x4000) {
        const size_t i = a - 0x4000u;
        if (i >= m_ram.size()) return 0xFF;
        if (write) m_ram[i] = v;
        return m_ram[i];
    }
    if (a >= 0x3C00) {
        if (write) videoWrite(a & 0x3FF, v);
        return m_video[a & 0x3FF];
    }
    if (a >= 0x3800) return write ? 0 : keyboard.read(a);
    if (a < 0x3000) return (!write && a < m_rom.size()) ? m_rom[a] : 0xFF;
    if (a < 0x37E0 || !m_config.expansion) return 0xFF;
    switch (a & 0x0C) {
    case 0x00:                                  // $37E0-$37E3: the interrupt latch / drive select
        if (write) { driveSelect(v); return 0; }
        {
            const uint8_t l = m_intLatch;
            if (!peek) { m_intLatch &= (uint8_t)~0x80; updateIrq(); }     // reading acknowledges the clock
            return l;
        }
    case 0x04: return 0xFF;                     // $37E4: cassette 1/2 select (write only)
    case 0x08:                                  // $37E8: the printer
        if (write) { printer.write(v); return 0; }
        return printer.status();
    default:                                    // $37EC-$37EF: the FD1771
        if (write) { fdcWrite(a & 3, v); return 0; }
        return fdcRead(a & 3, peek);
    }
}

uint8_t System::m3Access(uint16_t a, bool write, uint8_t v, bool) {
    if (a >= 0x4000) {
        const size_t i = a - 0x4000u;
        if (i >= m_ram.size()) return 0xFF;
        if (write) m_ram[i] = v;
        return m_ram[i];
    }
    if (a >= 0x3C00) {
        if (write) videoWrite(a & 0x3FF, v);
        return m_video[a & 0x3FF];
    }
    if (a >= 0x3800) return write ? 0 : keyboard.read(a);
    if ((a & ~1) == 0x37E8) {
        if (write) { printer.write(v); return 0; }
        return printer.status();
    }
    return (!write && a < m_rom.size()) ? m_rom[a] : 0xFF;
}

// The Model 4's 32K banks (port $84 bits 6-4): the lower 32K from block 2 or
// 3 (6, 7), the upper from block 2 or 3 (2, 3); 128K only
uint8_t& System::m4RamRef(uint16_t a) {
    uint32_t off = 0;
    if (m_ram.size() > 0x10000) {
        const int bank = (m_port84 >> 4) & 7;
        if (a < 0x8000) off = bank == 6 ? 0x10000 : bank == 7 ? 0x18000 : 0;
        else off = bank == 2 ? 0x8000 : bank == 3 ? 0x10000 : 0;
    }
    return m_ram[(a + off) % m_ram.size()];
}

uint8_t System::m4Ram(uint16_t a) const { return const_cast<System*>(this)->m4RamRef(a); }

uint8_t System::m4Access(uint16_t a, bool write, uint8_t v, bool) {
    const int map = m_port84 & 3;
    const bool p = m_model == Model::M4P;
    if (map == 3) {
        if (write) m4RamRef(a) = v;
        return m4Ram(a);
    }
    if (map == 2) {
        if (a < 0xF400) {
            if (write) m4RamRef(a) = v;
            return m4Ram(a);
        }
        if (a >= 0xF800) {
            if (write) videoWrite(a - 0xF800, v);
            return m_video[a - 0xF800];
        }
        return write ? 0 : keyboard.read(a);
    }
    const uint16_t page = (m_port84 & 0x80) ? 0x400 : 0;
    if (a >= 0x4000) {
        if (write) m4RamRef(a) = v;
        return m4Ram(a);
    }
    if (a >= 0x3C00) {
        if (write) videoWrite((uint16_t)(page + (a & 0x3FF)), v);
        return m_video[page + (a & 0x3FF)];
    }
    if (a >= 0x3800) return write ? 0 : keyboard.read(a);
    // $0000-$37FF: the ROM (map 0) or RAM (map 1; the 4P's map 0 reads it);
    // the 4P's boot ROM over $0000-$0FFF while it is in
    if (p && m_romIn && a < 0x1000 && !write) return a < m_rom.size() ? m_rom[a] : 0xFF;
    if (map == 1) {
        if (write) m4RamRef(a) = v;
        return m4Ram(a);
    }
    if ((a & ~1) == 0x37E8) {
        if (write) { printer.write(v); return 0; }
        return printer.status();
    }
    if (write) return 0;
    if (p) return m4Ram(a);
    return a < m_rom.size() ? m_rom[a] : 0xFF;
}

// The floppy controller ----------------------------------------------------
uint8_t System::fdcRead(int reg, bool peek) {
    if (!hasFdc()) return 0xFF;
    bool any = false;
    for (int d = 0; d < 4; ++d) any |= m_fdc->disk(d) != nullptr;
    if (peek) return any || reg ? m_fdc->peekRegister(reg) : 0xFF;
    const uint8_t v = m_fdc->readRegister(reg);
    // With no disk at all the status reads $FF (as xtrs): the ROMs then skip the boot
    return (reg == 0 && !any) ? 0xFF : v;
}

void System::setDensity(bool dd) {
    m_dd = dd;
    m_fdc->setVariant(dd ? Wd17xx::Variant::FD1793 : Wd17xx::Variant::FD1771);
    m_fdc->setDoubleDensity(dd);
}

void System::fdcWrite(int reg, uint8_t v) {
    if (!hasFdc()) return;
    if (isModel1(m_model)) {
        const Doubler d = m_config.doubler;
        const bool percom = d == Doubler::Percom || d == Doubler::Both;
        const bool tandy = d == Doubler::Tandy || d == Doubler::Both;
        // The Percom Doubler takes commands $FE (FD1771) and $FF (its 1791)
        if (reg == 0 && percom && (v == 0xFE || v == 0xFF)) { setDensity(v == 0xFF); return; }
        // The Radio Shack doubler watches the sector register: $80 double density, $A0 single
        if (reg == 2 && tandy) {
            if ((v & 0xE0) == 0x80) setDensity(true);
            else if ((v & 0xE0) == 0xA0) setDensity(false);
        }
    }
    m_fdc->writeRegister(reg, v);
}

void System::driveSelect(uint8_t v) {
    m_select = v;
    int side = 0;
    if (isModel1(m_model)) {
        // Drive 3's select is also the side select (for drives 0-2)
        if ((v & 7) && (v & 8)) { side = 1; v &= (uint8_t)~8; }
    } else {
        side = (v & 0x10) ? 1 : 0;
        m_fdc->setDoubleDensity(v & 0x80);
        if (v & 0x40) { if (!m_drq && !m_intrq) m_wait = true; }
        else m_wait = false;
    }
    const int drive = (v & 1) ? 0 : (v & 2) ? 1 : (v & 4) ? 2 : (v & 8) ? 3 : -1;
    m_fdc->selectDrive(drive);
    m_fdc->setSide(side);
    if (drive >= 0) {
        m_fdc->setMotor(true);
        m_motorLeft = (uint64_t)(m_masterHz * 3);      // the motors run 3 s after a select
        if (!isModel1(m_model)) { m_nmiLatch &= (uint8_t)~0x40; updateNmi(); }
    }
}

bool System::mountDisk(int drive, const std::string& path) {
    if (!hasFdc() || drive < 0 || drive > 3) return false;
    std::vector<uint8_t> b;
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        b.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    FloppyFormat fmt = FloppyDisk::detect(b, path);
    // A TRS-80 .dsk that is not DMK or JV3 is JV1 (10 x 256-byte sectors a track)
    if (fmt == FloppyFormat::JVC && b.size() % 2560 == 0) fmt = FloppyFormat::JV1;
    std::string err;
    auto disk = FloppyDisk::load(path, &err, fmt);
    if (!disk) return false;
    m_fdc->insertDisk(drive, std::move(disk));
    m_diskPath[drive] = path;
    return true;
}

void System::ejectDisk(int drive) {
    if (drive < 0 || drive > 3) return;
    m_fdc->ejectDisk(drive);
    m_diskPath[drive].clear();
}

// Ports -------------------------------------------------------------------
void System::portFFOut(uint8_t v) {
    m_portFF = v;
    tape.advance(m_ticks);
    tape.setMotor(m_ticks, v & 0x04);
    tape.output(m_ticks, v & 3);
    tape.clearFlipflop();
    m_cassOut = v & 3;
}

void System::portECOut(uint8_t v) {
    m_portEC = v;
    tape.setMotor(m_ticks, v & 0x02);
    setFast(v & 0x40);
}

uint8_t System::peekPort(uint8_t port) {
    if (isModel1(m_model)) {
        if (port == 0xFF) return (uint8_t)(((m_portFF & 0x08) ? 0x3F : 0x7F) | (tape.flipflop() ? 0x80 : 0));
        if (port >= 0xE8 && port <= 0xEB) return m_config.rs232 ? uart.peek(port & 3) : 0xFF;
        return 0xFF;
    }
    if (port >= 0xE0 && port <= 0xE3) return (uint8_t)~m_intLatch;
    if (port >= 0xE4 && port <= 0xE7) return (uint8_t)~m_nmiLatch;
    if (port >= 0xE8 && port <= 0xEB) return m_config.rs232 ? uart.peek(port & 3) : 0xFF;
    if (port >= 0xEC && port <= 0xEF) return 0xFF;
    if (port >= 0xF0 && port <= 0xF3) return fdcRead(port & 3, true);
    if (port >= 0xF8 && port <= 0xFB) return printer.status();
    if (port >= 0xFC) return (uint8_t)((m_portEC & 0x7E) | (tape.flipflop() ? 0x80 : 0) | (tape.highLevel() ? 1 : 0));
    return 0xFF;
}

uint8_t System::in(uint8_t port) {
    if (isModel1(m_model)) {
        if (port == 0xFF) tape.advance(m_ticks);
        if (port >= 0xE8 && port <= 0xEB && m_config.rs232) return uart.in(port & 3);
        return peekPort(port);
    }
    if (port >= 0xE8 && port <= 0xEB) return m_config.rs232 ? uart.in(port & 3) : 0xFF;
    if (port >= 0xEC && port <= 0xEF) {            // acknowledges the clock
        m_intLatch &= (uint8_t)~0x04;
        updateIrq();
        return 0xFF;
    }
    if (port >= 0xF0 && port <= 0xF3) return fdcRead(port & 3, false);
    if (port >= 0xFC) {
        tape.advance(m_ticks);
        const uint8_t v = peekPort(port);
        m_intLatch &= (uint8_t)~0x03;              // and the cassette's interrupts
        updateIrq();
        return v;
    }
    return peekPort(port);
}

void System::out(uint8_t port, uint8_t v) {
    if (isModel1(m_model)) {
        if (port == 0xFF) portFFOut(v);
        else if (port >= 0xE8 && port <= 0xEB && m_config.rs232) uart.out(port & 3, v);
        return;
    }
    if (port >= 0xE0 && port <= 0xE3) { m_intMask = v; updateIrq(); return; }
    if (port >= 0xE4 && port <= 0xE7) { m_nmiMask = (uint8_t)((v & 0xC0) | 0x20); updateNmi(); return; }
    if (port >= 0xE8 && port <= 0xEB) { if (m_config.rs232) uart.out(port & 3, v); return; }
    if (port >= 0xEC && port <= 0xEF) { portECOut(v); return; }
    if (port >= 0xF0 && port <= 0xF3) { fdcWrite(port & 3, v); return; }
    if (port >= 0xF4 && port <= 0xF7) { if (hasFdc()) driveSelect(v); return; }
    if (port >= 0xF8 && port <= 0xFB) { printer.write(v); return; }
    if (port >= 0xFC) {
        tape.advance(m_ticks);
        tape.output(m_ticks, v & 3);
        tape.clearFlipflop();
        m_cassOut = v & 3;
        return;
    }
    if (!isModel4(m_model)) return;
    if (port >= 0x84 && port <= 0x87) { m_port84 = v; return; }
    if (port == 0x88) { m_crtcReg = v & 0x1F; return; }
    if (port == 0x89) {
        if (m_crtcReg == 12) m_crtcStart = (uint16_t)(((v & 0x3F) << 8) | (m_crtcStart & 0xFF));
        else if (m_crtcReg == 13) m_crtcStart = (uint16_t)((m_crtcStart & 0xFF00) | v);
        return;
    }
    if (port >= 0x90 && port <= 0x93) { m_soundBit = v & 1; return; }
    if (port >= 0x9C && port <= 0x9F && m_model == Model::M4P) { m_romIn = v & 1; return; }
}

// Snapshots: memory, the ports and the clock ------------------------------
size_t System::stateSize() const { return state().size(); }

void System::saveState(uint8_t* buf) const {
    const auto v = state();
    std::memcpy(buf, v.data(), v.size());
}

std::vector<uint8_t> System::state() const {
    std::vector<uint8_t> v;
    put(v, (uint32_t)m_ram.size());
    v.insert(v.end(), m_ram.begin(), m_ram.end());
    v.insert(v.end(), std::begin(m_video), std::end(m_video));
    put(v, m_portFF); put(v, m_portEC); put(v, m_port84); put(v, m_romIn);
    put(v, m_crtcReg); put(v, m_crtcStart); put(v, m_soundBit); put(v, m_cassOut);
    put(v, m_intLatch); put(v, m_intMask); put(v, m_nmiLatch); put(v, m_nmiMask);
    put(v, m_dd); put(v, m_select); put(v, m_ticksPerT);
    put(v, m_ticks); put(v, m_rtcAcc); put(v, m_motorLeft);
    return v;
}

void System::loadState(const uint8_t* buf) {
    const uint8_t* p = buf;
    uint32_t n = 0;
    get(p, n);
    if (n == m_ram.size()) std::memcpy(m_ram.data(), p, n);
    p += n;
    std::memcpy(m_video, p, sizeof m_video);
    p += sizeof m_video;
    get(p, m_portFF); get(p, m_portEC); get(p, m_port84); get(p, m_romIn);
    get(p, m_crtcReg); get(p, m_crtcStart); get(p, m_soundBit); get(p, m_cassOut);
    get(p, m_intLatch); get(p, m_intMask); get(p, m_nmiLatch); get(p, m_nmiMask);
    get(p, m_dd); get(p, m_select); get(p, m_ticksPerT);
    get(p, m_ticks); get(p, m_rtcAcc); get(p, m_motorLeft);
    m_reported = m_ticks / (uint64_t)m_baseTicks;
    if (isModel1(m_model)) setDensity(m_dd);
    updateIrq();
    updateNmi();
}

System* systemOf(MachineDescriptor* m) {
    if (!m || m->buses.empty()) return nullptr;
    return dynamic_cast<System*>(m->buses[0].bus);
}

}  // namespace trs80
