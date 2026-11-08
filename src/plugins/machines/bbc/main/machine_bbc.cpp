// BBC Micro Model B, after b-em (GPL-2.0-or-later): 6502.c (the memory map,
// the 1 MHz bus), sysvia.c (the slow data bus, IC32), keyboard.c (the
// matrix), mem.c (sideways ROMs).

#include "machine_bbc.h"
#include "bbc_via.h"
#include "sn76489.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/io_handler.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdebug/main/execution_observer.h"
#include "libmem/main/ibus.h"
#include "plugins/6502/main/cpu6502.h"
#include "plugins/devices/bbc_video/main/bbc_video.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace bbc {

namespace {

constexpr uint32_t CPU_HZ = 2000000;

bool loadFile(const std::string& path, size_t size, std::vector<uint8_t>& out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (f) out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (!f || out.size() != size) {
        if (err) *err = "missing or wrong-size ROM " + path;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The keyboard matrix (keyboard.c): a key is row << 4 | column. Row 0 holds
// Shift (column 0), Ctrl (1) and the start-up links (2-9); the System VIA
// scans one key at a time through the slow data bus, or, in autoscan, the
// keyboard raises CA2 while any key in rows 1-7 is down.
// ---------------------------------------------------------------------------
constexpr int KEY_SHIFT = 0x00, KEY_CTRL = 0x01, KEY_CAPS_LOCK = 0x40;
constexpr uint16_t A2B_SHIFT = 0x100, A2B_CTRL = 0x200;

// b-em ascii2bbc for 0x20-0x7F: the key, and whether Shift is needed (for
// letters the key alone; the case comes from Caps Lock, see codeFor)
const uint16_t ASCII2BBC[96] = {
    0x62,           0x30|A2B_SHIFT, 0x31|A2B_SHIFT, 0x11|A2B_SHIFT,   // space ! " #
    0x12|A2B_SHIFT, 0x13|A2B_SHIFT, 0x34|A2B_SHIFT, 0x24|A2B_SHIFT,   // $ % & '
    0x15|A2B_SHIFT, 0x26|A2B_SHIFT, 0x48|A2B_SHIFT, 0x57|A2B_SHIFT,   // ( ) * +
    0x66,           0x17,           0x67,           0x68,             // , - . /
    0x27, 0x30, 0x31, 0x11, 0x12, 0x13, 0x34, 0x24, 0x15, 0x26,       // 0-9
    0x48,           0x57,           0x66|A2B_SHIFT, 0x17|A2B_SHIFT,   // : ; < =
    0x67|A2B_SHIFT, 0x68|A2B_SHIFT, 0x47,                             // > ? @
    0x41, 0x64, 0x52, 0x32, 0x22, 0x43, 0x53, 0x54, 0x25, 0x45, 0x46, 0x56, 0x65,   // A-M
    0x55, 0x36, 0x37, 0x10, 0x33, 0x51, 0x23, 0x35, 0x63, 0x21, 0x42, 0x44, 0x61,   // N-Z
    0x38,           0x78,           0x58,           0x18,             // [ \ ] ^
    0x28,           0x28|A2B_SHIFT,                                   // _ £ (`)
    0x41, 0x64, 0x52, 0x32, 0x22, 0x43, 0x53, 0x54, 0x25, 0x45, 0x46, 0x56, 0x65,   // a-m
    0x55, 0x36, 0x37, 0x10, 0x33, 0x51, 0x23, 0x35, 0x63, 0x21, 0x42, 0x44, 0x61,   // n-z
    0x38|A2B_SHIFT, 0x78|A2B_SHIFT, 0x58|A2B_SHIFT, 0x18|A2B_SHIFT,   // { | } ~
    0x59,                                                             // DEL
};

class BbcKeyboard : public IOHandler, public IKeyboardMatrix {
public:
    std::function<void()> onBreak;
    std::function<void()> onChange;             // the matrix changed: re-check CA2
    std::function<bool()> capsLockOn;

    const char* name() const override { return "Keyboard"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {
        std::memset(m_matrix, 0, sizeof(m_matrix));
        m_queue.clear();
        m_typing = 0;
        m_wait = 0;
    }

    bool down(int col, int row) const { return m_matrix[col & 15][row & 15]; }
    void set(int code, bool d) {
        m_matrix[code & 15][(code >> 4) & 15] = d;
        if (onChange) onChange();
    }

    // Typed text, a key at a time: press (with Shift if needed) for 40 ms,
    // then release for 30 ms. The MOS scans the keyboard every 10 ms and
    // misses keys (and repeats of one) at 25/15 ms
    void tick(uint64_t cycles) override {
        while (cycles) {
            if (m_wait > cycles) { m_wait -= cycles; return; }
            cycles -= m_wait;
            m_wait = 0;
            if (m_typing) {                       // release the held key
                set(m_typing & 0xFF, false);
                if (m_typing & A2B_SHIFT) set(KEY_SHIFT, false);
                if (m_typing & A2B_CTRL) set(KEY_CTRL, false);
                m_typing = 0;
                m_wait = 60000;
                continue;
            }
            if (m_queue.empty()) return;
            const uint8_t ch = m_queue.front();
            uint16_t k;
            if (std::isalpha(ch) && std::islower(ch) && capsOn()) {
                // Lower case with Caps Lock on: press CAPS LOCK first (Shift
                // only reverses it in SHIFT+CAPS LOCK mode)
                k = KEY_CAPS_LOCK;
            } else {
                k = codeFor(ch);
                m_queue.pop_front();
                if (!k) continue;
            }
            if (k & A2B_SHIFT) set(KEY_SHIFT, true);
            if (k & A2B_CTRL) set(KEY_CTRL, true);
            set(k & 0xFF, true);
            m_typing = k;
            m_wait = 80000;
        }
    }

    // Keys by name: placed by position on the BBC keyboard (b-em's physical map)
    bool key(std::string n, bool d) {
        for (auto& c : n) c = (char)std::tolower((unsigned char)c);
        if (n == "break" || n == "f12") { if (d && onBreak) onBreak(); return true; }
        static const std::map<std::string, int> keys = {
            {"left_shift", KEY_SHIFT}, {"right_shift", KEY_SHIFT}, {"shift", KEY_SHIFT},
            {"control", KEY_CTRL}, {"ctrl", KEY_CTRL},
            {"q", 0x10}, {"3", 0x11}, {"4", 0x12}, {"5", 0x13}, {"f4", 0x14}, {"8", 0x15}, {"f7", 0x16},
            {"minus", 0x17}, {"kpeq", 0x18}, {"equal", 0x18}, {"equals", 0x18}, {"caret", 0x18}, {"left", 0x19},
            {"f10", 0x20}, {"f0", 0x20}, {"w", 0x21}, {"e", 0x22}, {"t", 0x23}, {"7", 0x24}, {"i", 0x25},
            {"9", 0x26}, {"0", 0x27}, {"grave", 0x28}, {"underscore", 0x28}, {"down", 0x29},
            {"1", 0x30}, {"2", 0x31}, {"d", 0x32}, {"r", 0x33}, {"6", 0x34}, {"u", 0x35}, {"o", 0x36},
            {"p", 0x37}, {"bracket_left", 0x38}, {"up", 0x39},
            {"caps_lock", 0x40}, {"a", 0x41}, {"x", 0x42}, {"f", 0x43}, {"y", 0x44}, {"j", 0x45},
            {"k", 0x46}, {"at", 0x47}, {"squote", 0x48}, {"colon", 0x48}, {"return", 0x49}, {"enter", 0x49},
            {"shift_lock", 0x50}, {"s", 0x51}, {"c", 0x52}, {"g", 0x53}, {"h", 0x54}, {"n", 0x55},
            {"l", 0x56}, {"semicolon", 0x57}, {"bracket_right", 0x58}, {"delete", 0x59}, {"backspace", 0x59},
            {"tab", 0x60}, {"z", 0x61}, {"space", 0x62}, {"v", 0x63}, {"b", 0x64}, {"m", 0x65},
            {"comma", 0x66}, {"period", 0x67}, {"slash", 0x68}, {"copy", 0x69}, {"home", 0x69}, {"end", 0x69},
            {"run_stop", 0x70}, {"escape", 0x70}, {"esc", 0x70}, {"f1", 0x71}, {"f2", 0x72}, {"f3", 0x73},
            {"f5", 0x74}, {"f6", 0x75}, {"f8", 0x76}, {"f9", 0x77}, {"backslash", 0x78}, {"right", 0x79},
        };
        auto it = keys.find(n);
        if (it == keys.end()) return false;
        set(it->second, d);
        return true;
    }

    // IKeyboardMatrix
    void keyDown(int col, int row) override { set((row << 4) | col, true); }
    void keyUp(int col, int row) override { set((row << 4) | col, false); }
    void clearKeys() override { reset(); if (onChange) onChange(); }
    bool pressKeyByName(const std::string& n, bool d) override { return key(n, d); }
    IPortDevice* getPort(int) override { return nullptr; }
    void enqueueText(const std::string& text) override {
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (c == '\\' && i + 1 < text.size() && text[i + 1] == 'n') { c = '\r'; ++i; }
            m_queue.push_back((uint8_t)c);
        }
    }
    bool typing() const { return m_typing || !m_queue.empty(); }

private:
    bool capsOn() const { return capsLockOn ? capsLockOn() : true; }
    uint16_t codeFor(uint8_t c) const {
        if (c == '\r' || c == '\n') return 0x49;
        if (c == 0x1B) return 0x70;
        if (c < 0x20 || c > 0x7F) return 0;
        uint16_t k = ASCII2BBC[c - 0x20];
        // Capitals: plain with Caps Lock on, else with Shift
        if (std::isupper(c) && !capsOn()) k |= A2B_SHIFT;
        return k;
    }

    bool m_matrix[16][16] = {};
    std::deque<uint8_t> m_queue;
    uint16_t m_typing = 0;
    uint64_t m_wait = 0;
};

// The video and sound, as the frontends find them
class BbcDisplay : public IOHandler, public IVideoOutput {
public:
    explicit BbcDisplay(BbcVideo* v) : m_video(v) {}
    const char* name() const override { return "Video"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    VideoDimensions getDimensions() const override { return m_video->getDimensions(); }
    void renderFrame(uint32_t* buffer) override { m_video->renderFrame(buffer); }
private:
    BbcVideo* m_video;
};

class BbcSound : public IOHandler, public IAudioOutput {
public:
    explicit BbcSound(Sn76489* sn) : m_sn(sn) {}
    const char* name() const override { return "SN76489"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    int nativeSampleRate() const override { return Sn76489::SAMPLE_RATE; }
    int pullSamples(float* buffer, int maxSamples) override { return m_sn->pull(buffer, maxSamples); }
private:
    Sn76489* m_sn;
};

// ---------------------------------------------------------------------------
// The bus (6502.c readmem/writemem, sysvia.c)
// ---------------------------------------------------------------------------
class BbcBus : public IBus {
public:
    std::function<void(bool)> onIrq;

    BbcBus(std::vector<uint8_t> os, BbcVideo* video, BbcKeyboard* kbd, Sn76489* sn)
        : m_os(std::move(os)), m_video(video), m_kbd(kbd), m_sn(sn) {
        m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF};
        m_ram.assign(0x8000, 0);
        for (auto& r : m_roms) r.clear();
        video->setMemory(m_ram.data());
        video->onVsync = [this](bool on) { m_sysvia.setCa1(on ? 1 : 0); };
        kbd->onChange = [this]() { keyUpdate(); };
        kbd->capsLockOn = [this]() { return !(m_ic32 & 0x40); };

        m_sysvia.readPortA = [this]() { updateSdb(); return m_sdb; };
        m_sysvia.writePortA = [this](uint8_t v) { m_sdbOut = v; updateSdb(); };
        m_sysvia.writePortB = [this](uint8_t v) { writeIc32(v); };
        m_sysvia.readPortB = [this]() {
            // Bits 4 and 5: the joystick fire buttons (low when pressed); 6-7: no speech chip
            uint8_t v = 0xFF;
            if (m_fire[0]) v &= ~0x10;
            if (m_fire[1]) v &= ~0x20;
            return v;
        };
        m_sysvia.setCb2Out = [this](int level) {
            if (level && !m_cb2) m_video->latchLightPen();
            m_cb2 = level;
        };
        powerOn();
    }

    void setRom(int slot, std::vector<uint8_t> rom) { m_roms[slot & 15] = std::move(rom); }

    void powerOn() {
        std::fill(m_ram.begin(), m_ram.end(), 0);
        m_romsel = 0;
        m_ic32 = 0;
        m_aciaControl = 0;
        m_sdb = m_sdbOut = 0;
        m_cycles = m_devCycle = 0;
        m_sysvia.reset();
        m_uservia.reset();
        m_video->reset();
        m_sn->reset();
        m_video->setScreenSize(0);
    }

    void setFire(int n, bool down) { m_fire[n & 1] = down; }

    // Instruction framing: accesses are counted so I/O happens on its cycle
    void begin() { m_access = 0; m_stretch = 0; }
    int end(int c) {
        const int total = c + m_stretch;
        m_cycles += (uint64_t)total;
        sync(m_cycles);
        return total;
    }
    uint64_t cycles() const { return m_cycles; }

    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "system"; }

    uint8_t read8(uint32_t addr) override {
        const uint16_t a = addr & 0xFFFF;
        uint8_t v;
        if (a >= 0xFC00 && a < 0xFF00) v = io(a, nullptr);
        else v = peekMem(a);
        ++m_access;
        if (m_observer) m_observer->onMemoryRead(this, a, v);
        return v;
    }
    uint8_t peek8(uint32_t addr) override {
        const uint16_t a = addr & 0xFFFF;
        if (a >= 0xFC00 && a < 0xFF00) return 0xFF;
        return peekMem(a);
    }
    void write8(uint32_t addr, uint8_t v) override {
        const uint16_t a = addr & 0xFFFF;
        if (m_observer) m_observer->onMemoryWrite(this, a, peek8(a), v);
        if (a < 0x8000) m_ram[a] = v;
        else if (a >= 0xFC00 && a < 0xFF00) io(a, &v);
        ++m_access;
    }
    void reset() override {}
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

    uint8_t* ram() { return m_ram.data(); }
    int romsel() const { return m_romsel; }
    uint8_t ic32() const { return m_ic32; }
    BbcVia& sysvia() { return m_sysvia; }
    BbcVia& uservia() { return m_uservia; }

private:
    uint8_t peekMem(uint16_t a) const {
        if (a < 0x8000) return m_ram[a];
        if (a < 0xC000) {
            const auto& r = m_roms[m_romsel];
            return r.empty() ? 0 : r[(a - 0x8000) % r.size()];
        }
        return m_os[a - 0xC000];
    }

    // The devices up to cycle `to`
    void sync(uint64_t to) {
        if (to <= m_devCycle) return;
        const uint64_t d = to - m_devCycle;
        m_devCycle = to;
        m_video->clock(d);
        m_sysvia.poll((int)d);
        m_uservia.poll((int)d);
        m_sn->advance(d);
        if (onIrq) onIrq(irqLine());
    }

    // &FC00-&FEFF. FRED, JIM and the slow parts of SHEILA are on the 1 MHz
    // bus: the access waits for the next 1 MHz edge, 1 or 2 more cycles
    uint8_t io(uint16_t a, const uint8_t* wr) {
        static const bool FE_SLOW[8] = {true, false, true, true, false, false, true, false};
        uint64_t at = m_cycles + (uint64_t)m_access + (uint64_t)m_stretch;
        if (a < 0xFE00 || FE_SLOW[(a >> 5) & 7]) {
            const int extra = (at & 1) ? 2 : 1;
            m_stretch += extra;
            at += (uint64_t)extra;
        }
        sync(at);
        uint8_t r = 0xFE;
        if (a < 0xFE00) r = 0xFF;                               // FRED, JIM: nothing fitted
        else switch (a & 0xFFE0) {
            case 0xFE00:
                if (a < 0xFE08) {                               // 6845
                    if (wr) m_video->crtcWrite(a, *wr); else r = m_video->crtcRead(a);
                } else if (a < 0xFE10) {                        // 6850 ACIA (cassette/RS423)
                    r = acia(a, wr);
                }
                break;
            case 0xFE20:
                if (a < 0xFE30) { if (wr) m_video->ulaWrite(a, *wr); }   // video ULA (write only)
                else if (wr) m_romsel = *wr & 15;                         // ROMSEL (write only)
                break;
            case 0xFE40:                                        // System VIA
                if (wr) m_sysvia.write(a, *wr); else r = m_sysvia.read(a);
                break;
            case 0xFE60:                                        // User VIA
                if (wr) m_uservia.write(a, *wr); else r = m_uservia.read(a);
                break;
            default:
                break;
        }
        if (onIrq) onIrq(irqLine());
        return wr ? 0 : r;
    }

    // The 6850 ACIA, with nothing attached (b-em acia.c): the transmit
    // register is always empty; it interrupts only while the transmit
    // interrupt is enabled; nothing is ever received
    bool aciaIrq() const { return (m_aciaControl & 0x60) == 0x20; }
    uint8_t acia(uint16_t a, const uint8_t* wr) {
        if (wr) {
            if (!(a & 1)) m_aciaControl = *wr;              // control (data written: dropped)
            return 0;
        }
        if (a & 1) return 0;                                 // receive data
        return (uint8_t)(0x02 | (aciaIrq() ? 0x80 : 0));     // status: TDRE
    }
    bool irqLine() const { return m_sysvia.irq() || m_uservia.irq() || aciaIrq(); }

    // The slow data bus (sysvia.c): the System VIA's port A, the keyboard
    // pulling bit 7 low when the addressed key is up (with the keyboard enabled)
    void updateSdb() {
        m_sdb = m_sdbOut;
        m_keyRow = (m_sdb >> 4) & 7;
        m_keyCol = m_sdb & 0xF;
        keyUpdate();
        if (!(m_ic32 & 8) && !keyIsDown()) m_sdb &= 0x7F;
    }
    // IC32, the addressable latch on port B: bit 0 sound write enable, 3
    // keyboard autoscan, 4-5 the screen size, 6-7 the Caps/Shift Lock LEDs
    void writeIc32(uint8_t v) {
        const uint8_t old = m_ic32;
        if (v & 8) m_ic32 |= (uint8_t)(1 << (v & 7));
        else m_ic32 &= (uint8_t)~(1 << (v & 7));
        updateSdb();
        if (!(m_ic32 & 1) && (old & 1)) m_sn->write(m_sdb);
        m_video->setScreenSize(((m_ic32 & 0x10) ? 2 : 0) | ((m_ic32 & 0x20) ? 1 : 0));
    }
    // keyboard.c key_update: CA2 while a key in rows 1-7 is down (autoscan:
    // any column; else the addressed one)
    void keyUpdate() {
        auto colDown = [&](int col) {
            for (int row = 1; row < 8; row++) if (m_kbd->down(col, row)) return true;
            return false;
        };
        bool any = false;
        if (m_ic32 & 8) {
            for (int col = 0; col < 10 && !any; col++) any = colDown(col);
        } else if (m_keyCol < 10) {
            any = colDown(m_keyCol);
        }
        m_sysvia.setCa2(any ? 1 : 0);
    }
    // key_is_down: row 0 columns 2-9 are the start-up links (all open: Mode 7)
    bool keyIsDown() const {
        if (m_keyRow == 0 && m_keyCol >= 2 && m_keyCol <= 9) return false;
        return m_kbd->down(m_keyCol, m_keyRow);
    }

    BusConfig m_cfg;
    std::vector<uint8_t> m_os, m_ram;
    std::vector<uint8_t> m_roms[16];
    BbcVideo* m_video;
    BbcKeyboard* m_kbd;
    Sn76489* m_sn;
    BbcVia m_sysvia, m_uservia;
    int m_romsel = 0;
    uint8_t m_ic32 = 0, m_sdb = 0, m_sdbOut = 0;
    int m_keyRow = 0, m_keyCol = 0, m_cb2 = 0;
    uint8_t m_aciaControl = 0;
    bool m_fire[2] = {};
    uint64_t m_cycles = 0, m_devCycle = 0;
    int m_access = 0, m_stretch = 0;
};

}  // namespace

MachineDescriptor* create(Model, std::string* err) {
    std::vector<uint8_t> os, basic;
    if (!loadFile("roms/bbc/os12.rom", 0x4000, os, err)) return nullptr;
    if (!loadFile("roms/bbc/basic2.rom", 0x4000, basic, err)) return nullptr;

    auto* desc = new MachineDescriptor();
    desc->machineId = "bbcb";
    desc->displayName = "BBC Micro Model B";
    desc->licenseClass = "proprietary";
    desc->uiPanesSet = true;
    desc->uiPanes = {};
    desc->clockHz = CPU_HZ;

    auto* video = new BbcVideo();
    if (!video->loadTeletextFont("roms/bbc/saa5050.fnt", err)) { delete video; delete desc; return nullptr; }
    auto* kbd = new BbcKeyboard();
    auto* sn = new Sn76489();
    auto* display = new BbcDisplay(video);
    auto* sound = new BbcSound(sn);
    auto* bus = new BbcBus(std::move(os), video, kbd, sn);
    bus->setRom(15, std::move(basic));
    desc->buses.push_back({"system", bus});
    auto* cpu = new MOS6502();
    cpu->setDataBus(bus);
    cpu->setCodeBus(bus);
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});
    bus->onIrq = [cpu](bool on) { cpu->setIrqLine(on); };

    auto* io = new IORegistry();
    desc->ioRegistry = io;
    io->registerHandler(display);
    io->registerHandler(kbd);
    io->registerHandler(sound);
    desc->deleters.push_back([video, display, kbd, sn, sound]() {
        delete display; delete video; delete kbd; delete sound; delete sn;
    });

    // BREAK resets the 6502 only: the MOS tells it from power-on by the
    // System VIA's IER, which keeps its value (b-em main_key_break)
    kbd->onBreak = [cpu]() { cpu->reset(); };
    desc->onKey = [kbd](const std::string& n, bool down) { return kbd->key(n, down); };
    desc->screenButtons = {{"BREAK", {"break"}, "BREAK: the 6502 restarts (a soft reset; the program in memory stays)."}};

    // Joystick fire buttons on the System VIA (the analogue sticks need the ADC: not yet)
    desc->joystickPorts = 2;
    desc->primaryJoystickPort = 0;
    desc->onJoystick = [bus](int port, uint8_t bits) { bus->setFire(port, !(bits & 0x10)); };

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

}  // namespace bbc
