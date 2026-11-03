#include "machine_atari.h"
#include "atari_sio.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/ibus.h"
#include "libdebug/main/execution_observer.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/io_handler.h"
#include "libdevices/main/iport_device.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/shared_irq_manager.h"
#include "include/util/path_util.h"
#include "plugins/6502/main/cpu6502.h"
#include "plugins/devices/antic/main/antic.h"
#include "plugins/devices/gtia/main/gtia.h"
#include "plugins/devices/pokey/main/pokey.h"
#include "plugins/devices/pia6520/main/pia6520.h"
#include <cctype>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>

namespace atari {

namespace {

// ---------------------------------------------------------------------------
// NMI wire from ANTIC to the 6502 (edge-triggered in the CPU)
// ---------------------------------------------------------------------------
class NmiLine : public ISignalLine {
public:
    explicit NmiLine(ICore* cpu) : m_cpu(cpu) {}
    bool get() const override { return m_level; }
    void set(bool level) override { m_level = level; m_cpu->setNmiLine(level); }
    void pulse() override { m_cpu->triggerNmi(); }
private:
    ICore* m_cpu;
    bool   m_level = false;
};

// ---------------------------------------------------------------------------
// The memory map
// ---------------------------------------------------------------------------
/**
 * 800: RAM to 48 KB ($C000-$CFFF unmapped), BASIC cartridge at $A000, OS-B
 * at $D800 (floating point) and $E000. XL/XE: 64 KB RAM under the OS
 * ($C000-$CFFF, $D800-$FFFF), BASIC ($A000) and the self-test ROM ($5000),
 * switched by PIA port B bits 0, 1 and 7. $D000-$D7FF: GTIA, POKEY, PIA and
 * ANTIC, each mirrored through its page.
 */
class AtariBus : public IBus {
public:
    AtariBus(bool xl, std::vector<uint8_t> os, std::vector<uint8_t> basic)
        : m_xl(xl), m_os(std::move(os)), m_basic(std::move(basic)) {
        m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF};
        std::memset(m_ram, 0, sizeof(m_ram));
        resetBanking();
    }

    void setDevices(IOHandler* gtia, IOHandler* pokey, IOHandler* pia, IOHandler* antic) {
        m_gtia = gtia; m_pokey = pokey; m_pia = pia; m_antic = antic;
    }

    /** XL/XE banking from PIA port B's output. */
    void setPortB(uint8_t v) {
        if (!m_xl) return;
        m_osOn = v & 0x01;
        m_basicOn = !(v & 0x02);
        m_selfTestOn = !(v & 0x80) && m_osOn;
    }
    void resetBanking() {
        if (m_xl) setPortB(0xFF);           // PIA reset: inputs, pulled high
        else { m_osOn = true; m_basicOn = !m_basic.empty(); m_selfTestOn = false; }
    }
    bool basicOn() const { return m_basicOn; }
    bool osOn() const { return m_osOn; }

    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "system"; }

    uint8_t read8(uint32_t addr) override {
        addr &= 0xFFFF;
        uint8_t v = read(addr, false);
        if (m_observer) m_observer->onMemoryRead(this, addr, v);
        return v;
    }
    uint8_t peek8(uint32_t addr) override { return read(addr & 0xFFFF, true); }

    void write8(uint32_t addr, uint8_t val) override {
        addr &= 0xFFFF;
        uint8_t before = m_ram[addr];
        if (addr >= 0xD000 && addr < 0xD800) {
            IOHandler* h = nullptr;
            uint32_t a = ioAddr(addr, h);
            if (h) h->ioWrite(this, a, val);
        } else if (writable(addr)) {
            m_ram[addr] = val;
        }
        if (m_observer) m_observer->onMemoryWrite(this, addr, before, val);
    }

    // Snapshots: RAM and banking
    size_t stateSize() const override { return sizeof(m_ram) + 3; }
    void saveState(uint8_t* buf) const override {
        std::memcpy(buf, m_ram, sizeof(m_ram));
        buf[sizeof(m_ram)] = m_osOn; buf[sizeof(m_ram) + 1] = m_basicOn; buf[sizeof(m_ram) + 2] = m_selfTestOn;
    }
    void loadState(const uint8_t* buf) override {
        std::memcpy(m_ram, buf, sizeof(m_ram));
        m_osOn = buf[sizeof(m_ram)]; m_basicOn = buf[sizeof(m_ram) + 1]; m_selfTestOn = buf[sizeof(m_ram) + 2];
    }
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

private:
    // Which device answers a $D000-$D7FF address, and the address it decodes
    uint32_t ioAddr(uint32_t addr, IOHandler*& h) const {
        switch ((addr >> 8) & 0x07) {
            case 0: h = m_gtia;  return 0xD000 | (addr & 0x1F);
            case 2: h = m_pokey; return 0xD200 | (addr & 0x0F);
            case 3: {
                // PORTA, PORTB, PACTL, PBCTL: A0 picks the port, A1 data/control
                static const uint8_t PIA_REG[4] = {0, 2, 1, 3};
                h = m_pia;
                return 0xD300 | PIA_REG[addr & 3];
            }
            case 4: h = m_antic; return 0xD400 | (addr & 0x0F);
            default: h = nullptr; return 0;   // $D100, $D500 (cartridge control)...
        }
    }

    bool writable(uint32_t addr) const {
        if (m_xl) {
            if (m_osOn && ((addr >= 0xC000 && addr < 0xD000) || addr >= 0xD800)) return false;
            if (m_basicOn && addr >= 0xA000 && addr < 0xC000) return false;
            if (m_selfTestOn && addr >= 0x5000 && addr < 0x5800) return false;
            return true;
        }
        if (addr >= 0xC000) return false;
        if (m_basicOn && addr >= 0xA000) return false;
        return true;
    }

    uint8_t read(uint32_t addr, bool peek) {
        if (addr >= 0xD000 && addr < 0xD800) {
            IOHandler* h = nullptr;
            uint32_t a = ioAddr(addr, h);
            if (!h) return 0xFF;
            if (peek && h == m_pia) return 0xFF;   // reading the PIA clears its flags
            uint8_t v = 0xFF;
            h->ioRead(this, a, &v);
            return v;
        }
        if (m_xl) {
            if (m_osOn && ((addr >= 0xC000 && addr < 0xD000) || addr >= 0xD800))
                return m_os[addr - 0xC000];
            if (m_selfTestOn && addr >= 0x5000 && addr < 0x5800)
                return m_os[0x1000 + (addr - 0x5000)];   // the ROM's $D000-$D7FF part
            if (m_basicOn && addr >= 0xA000 && addr < 0xC000)
                return m_basic[addr - 0xA000];
            return m_ram[addr];
        }
        if (addr >= 0xD800) return m_os[addr - 0xD800];  // OS-B: FP at $D800, OS at $E000
        if (addr >= 0xC000) return 0xFF;                 // nothing there on an 800
        if (m_basicOn && addr >= 0xA000) return m_basic[addr - 0xA000];
        return m_ram[addr];
    }

    BusConfig m_cfg;
    bool m_xl;
    std::vector<uint8_t> m_os, m_basic;
    uint8_t m_ram[65536];
    bool m_osOn = true, m_basicOn = false, m_selfTestOn = false;
    IOHandler *m_gtia = nullptr, *m_pokey = nullptr, *m_pia = nullptr, *m_antic = nullptr;
};

// ---------------------------------------------------------------------------
// PIA ports
// ---------------------------------------------------------------------------
/** Port A: joysticks 0 and 1 (low and high nibble, active low). */
class JoystickPort : public IPortDevice {
public:
    uint8_t readPort() override { return m_state; }
    void writePort(uint8_t) override {}
    void setDdr(uint8_t) override {}
    void set(int stick, uint8_t bits4) {
        if (stick == 0) m_state = (uint8_t)((m_state & 0xF0) | (bits4 & 0x0F));
        else            m_state = (uint8_t)((m_state & 0x0F) | ((bits4 & 0x0F) << 4));
    }
private:
    uint8_t m_state = 0xFF;
};

/** Port B on the XL/XE: memory banking from its outputs (inputs float high). */
class BankingPort : public IPortDevice {
public:
    explicit BankingPort(AtariBus* bus) : m_bus(bus) {}
    uint8_t readPort() override { return 0xFF; }
    void writePort(uint8_t v) override { m_out = v; apply(); }
    void setDdr(uint8_t d) override { m_ddr = d; apply(); }
    void reset() { m_out = 0; m_ddr = 0; apply(); }
private:
    void apply() { m_bus->setPortB((uint8_t)((m_out & m_ddr) | ~m_ddr)); }
    AtariBus* m_bus;
    uint8_t m_out = 0, m_ddr = 0;
};

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------
struct KeyDef { uint8_t code; bool shift; bool ctrl; };

/** Key names (the GUI's and CLI `key`'s) to POKEY key codes. */
const std::map<std::string, KeyDef>& keyMap() {
    static const std::map<std::string, KeyDef> m = [] {
        std::map<std::string, KeyDef> k;
        const char* letters = "abcdefghijklmnopqrstuvwxyz";
        const uint8_t letterCodes[26] = {
            0x3F, 0x15, 0x12, 0x3A, 0x2A, 0x38, 0x3D, 0x39, 0x0D, 0x01, 0x05, 0x00, 0x25,
            0x23, 0x08, 0x0A, 0x2F, 0x28, 0x3E, 0x2D, 0x0B, 0x10, 0x2E, 0x16, 0x2B, 0x17 };
        for (int i = 0; i < 26; ++i) k[std::string(1, letters[i])] = {letterCodes[i], false, false};
        const uint8_t digitCodes[10] = { 0x32, 0x1F, 0x1E, 0x1A, 0x18, 0x1D, 0x1B, 0x33, 0x35, 0x30 };
        for (int i = 0; i < 10; ++i) k[std::string(1, (char)('0' + i))] = {digitCodes[i], false, false};
        k["space"] = {0x21, false, false};   k["return"] = {0x0C, false, false};
        k["delete"] = {0x34, false, false};  k["backspace"] = {0x34, false, false};
        k["tab"] = {0x2C, false, false};     k["run_stop"] = {0x1C, false, false};
        k["escape"] = {0x1C, false, false};  k["esc"] = {0x1C, false, false};
        k["semicolon"] = {0x02, false, false}; k["colon"] = {0x02, true, false};
        k["minus"] = {0x0E, false, false};   k["leftarrow"] = {0x0E, true, false};  // _
        k["kpeq"] = {0x0F, false, false};    k["equal"] = {0x0F, false, false};
        k["plus"] = {0x06, false, false};    k["asterisk"] = {0x07, false, false};
        k["uparrow"] = {0x07, true, false};  // ^
        k["comma"] = {0x20, false, false};   k["bracket_left"] = {0x20, true, false};
        k["period"] = {0x22, false, false};  k["bracket_right"] = {0x22, true, false};
        k["slash"] = {0x26, false, false};   k["question"] = {0x26, true, false};
        k["less"] = {0x36, false, false};    k["greater"] = {0x37, false, false};
        k["exclaim"] = {0x1F, true, false};  k["dquote"] = {0x1E, true, false};
        k["hash"] = {0x1A, true, false};     k["dollar"] = {0x18, true, false};
        k["percent"] = {0x1D, true, false};  k["ampersand"] = {0x1B, true, false};
        k["squote"] = {0x33, true, false};   k["at"] = {0x35, true, false};
        k["lparen"] = {0x30, true, false};   k["rparen"] = {0x32, true, false};
        k["up"] = {0x0E, false, true};       k["down"] = {0x0F, false, true};
        k["left"] = {0x06, false, true};     k["right"] = {0x07, false, true};
        k["home"] = {0x36, true, false};     k["clear"] = {0x36, true, false};
        k["insert"] = {0x37, true, false};
        k["f1"] = {0x27, false, false};      k["inverse"] = {0x27, false, false};   // the Atari key
        k["f6"] = {0x11, false, false};      k["help"] = {0x11, false, false};
        k["f8"] = {0x3C, false, false};      k["caps"] = {0x3C, false, false};
        return k;
    }();
    return m;
}

/** ASCII to a key (for typed text). */
bool charKey(char c, KeyDef& out) {
    static const std::map<char, std::string> sym = {
        {' ', "space"}, {'\n', "return"}, {'\r', "return"}, {';', "semicolon"}, {':', "colon"},
        {'-', "minus"}, {'_', "leftarrow"}, {'=', "equal"}, {'+', "plus"}, {'*', "asterisk"},
        {'^', "uparrow"}, {',', "comma"}, {'[', "bracket_left"}, {'.', "period"},
        {']', "bracket_right"}, {'/', "slash"}, {'?', "question"}, {'<', "less"}, {'>', "greater"},
        {'!', "exclaim"}, {'"', "dquote"}, {'#', "hash"}, {'$', "dollar"}, {'%', "percent"},
        {'&', "ampersand"}, {'\'', "squote"}, {'@', "at"}, {'(', "lparen"}, {')', "rparen"},
        {'|', "equal"}, {'\\', "plus"}, {'\t', "tab"}, {'\b', "delete"} };
    std::string name;
    if (c >= 'a' && c <= 'z') name = std::string(1, c);
    else if (c >= 'A' && c <= 'Z') name = std::string(1, (char)(c - 'A' + 'a'));   // caps lock is on at boot
    else if (c >= '0' && c <= '9') name = std::string(1, c);
    else if (auto it = sym.find(c); it != sym.end()) name = it->second;
    else return false;
    out = keyMap().at(name);
    if (c == '|' || c == '\\') out.shift = true;   // shifted = and +
    return true;
}

/**
 * The keyboard: key names to POKEY codes with the shift and control keys,
 * the console keys (GTIA), RESET, BREAK; and typed text, a key a few frames
 * at a time. Registered as an I/O handler (no registers) for its tick and
 * so the CLI `type` command finds it.
 */
class AtariKeyboard : public IOHandler, public IKeyboardMatrix {
public:
    AtariKeyboard(POKEY* pokey, GTIA* gtia, Antic* antic, IBus* bus, bool xl, std::function<void()> resetFn)
        : m_pokey(pokey), m_gtia(gtia), m_antic(antic), m_bus(bus), m_xl(xl), m_reset(std::move(resetFn)) {}

    const char* name() const override { return "Keyboard"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {
        m_held.clear(); m_shift = m_ctrl = false; m_queue.clear(); m_typing = 0; m_lastCode = 0xFF;
    }

    bool key(const std::string& rawName, bool down) {
        std::string n;
        for (char c : rawName) n += (char)std::tolower((unsigned char)c);
        if (n == "left_shift" || n == "right_shift" || n == "shift" || n == "lshift" || n == "rshift") {
            m_shift = down;
            m_pokey->setShift(down);
            return true;
        }
        if (n == "control" || n == "ctrl") { m_ctrl = down; return true; }
        if (n == "f2" || n == "option") { m_gtia->setConsoleKey(GTIA::CONSOL_OPTION, down); return true; }
        if (n == "f3" || n == "select") { m_gtia->setConsoleKey(GTIA::CONSOL_SELECT, down); return true; }
        if (n == "f4" || n == "start")  { m_gtia->setConsoleKey(GTIA::CONSOL_START, down); return true; }
        if (n == "f5" || n == "reset") {
            if (down) { if (m_xl) m_reset(); else m_antic->pressResetKey(); }
            return true;
        }
        if (n == "f7" || n == "break" || n == "pause") { if (down) m_pokey->breakKey(); return true; }
        auto it = keyMap().find(n);
        if (it == keyMap().end()) return false;
        press(it->second, n, down);
        return true;
    }

    // IKeyboardMatrix
    void keyDown(int, int) override {}
    void keyUp(int, int) override {}
    void clearKeys() override { m_held.clear(); m_pokey->keyUp(); }
    bool pressKeyByName(const std::string& n, bool down) override { return key(n, down); }
    void enqueueText(const std::string& text) override {
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (c == '\\' && i + 1 < text.size() && text[i + 1] == 'n') { c = '\n'; ++i; }
            KeyDef k;
            if (charKey(c, k)) m_queue.push_back(k);
        }
    }
    IPortDevice* getPort(int) override { return nullptr; }

    /** Hold a console key for `cycles`, then release it (booting with OPTION). */
    void holdConsoleKey(uint8_t bit, uint64_t cycles) {
        m_gtia->setConsoleKey(bit, true);
        m_holdBit = bit;
        m_holdCycles = cycles;
    }

    /**
     * Typing: each key held 2 frames, then 2 frames up (about 15 keys a
     * second). The OS ignores the same key code again until its debounce
     * delay runs out, so a repeated key waits 4 frames more; and it holds
     * one key in CH ($2FC), so the next waits until that has been read
     * (BASIC tokenising a line is slower than typing).
     */
    void tick(uint64_t cycles) override {
        if (m_holdCycles) {
            if (m_holdCycles <= cycles) { m_gtia->setConsoleKey(m_holdBit, false); m_holdCycles = 0; }
            else m_holdCycles -= cycles;
        }
        if (m_queue.empty() && !m_typing) return;
        constexpr uint64_t FRAME = 114 * 262;
        m_typeCycles += cycles;
        if (m_typeCycles < 2 * FRAME) return;
        m_typeCycles = 0;
        if (m_typing == 1) {                   // release
            m_pokey->keyUp();
            if (m_typedShift) m_pokey->setShift(false);
            m_typing = 2;
        } else if (!m_queue.empty()) {         // next key
            // The OS keeps one key in CH ($2FC); wait until it has taken the last
            if (m_bus->peek8(0x02FC) != 0xFF) return;
            KeyDef k = m_queue.front();
            uint8_t code = (uint8_t)(k.code | (k.shift ? 0x40 : 0) | (k.ctrl ? 0x80 : 0));
            if (code == m_lastCode && m_typing == 2) {   // same key again: wait longer
                m_typing = 3;
                return;
            }
            m_lastCode = code;
            m_queue.pop_front();
            m_typedShift = k.shift;
            if (k.shift) m_pokey->setShift(true);
            m_pokey->keyDown(code);
            m_typing = 1;
        } else {
            m_typing = 0;
        }
    }

private:
    void press(const KeyDef& k, const std::string& n, bool down) {
        if (down) {
            m_held[n] = true;
            bool shift = k.shift || m_shift, ctrl = k.ctrl || m_ctrl;
            m_pokey->keyDown((uint8_t)(k.code | (shift ? 0x40 : 0) | (ctrl ? 0x80 : 0)));
        } else {
            m_held.erase(n);
            if (m_held.empty()) m_pokey->keyUp();
        }
    }

    POKEY* m_pokey;
    GTIA* m_gtia;
    Antic* m_antic;
    IBus* m_bus;
    bool m_xl;
    std::function<void()> m_reset;
    std::map<std::string, bool> m_held;
    bool m_shift = false, m_ctrl = false;
    std::deque<KeyDef> m_queue;
    uint8_t m_holdBit = 0;
    uint64_t m_holdCycles = 0;
    int m_typing = 0;            // 0 idle, 1 key down, 2 key up (gap), 3 longer gap
    uint8_t m_lastCode = 0xFF;
    bool m_typedShift = false;
    uint64_t m_typeCycles = 0;
};

bool loadRom(const std::string& rel, size_t size, std::vector<uint8_t>& out, std::string* err) {
    std::string path = PathUtil::findResource(rel);
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "ROM not found: " + rel; return false; }
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (out.size() != size) {
        if (err) *err = rel + ": expected " + std::to_string(size) + " bytes, got " + std::to_string(out.size());
        return false;
    }
    return true;
}

}  // namespace

MachineDescriptor* create(Model model, std::string* err) {
    const bool xl = model != Model::Atari800;
    std::vector<uint8_t> os, basic;
    if (!loadRom(xl ? "roms/atari/osxl.bin" : "roms/atari/osb.bin", xl ? 16384 : 10240, os, err)) return nullptr;
    if (!loadRom("roms/atari/basic.bin", 8192, basic, err)) return nullptr;

    auto* desc = new MachineDescriptor();
    switch (model) {
        case Model::Atari800:   desc->machineId = "atari800";   desc->displayName = "Atari 800"; break;
        case Model::Atari800XL: desc->machineId = "atari800xl"; desc->displayName = "Atari 800XL"; break;
        case Model::Atari65XE:  desc->machineId = "atari65xe";  desc->displayName = "Atari 65XE"; break;
    }
    desc->licenseClass = "proprietary";

    auto* bus = new AtariBus(xl, std::move(os), std::move(basic));
    desc->buses.push_back({"system", bus});   // the descriptor deletes its buses and CPUs

    auto* cpu = new MOS6502();
    cpu->setDataBus(bus);
    cpu->setCodeBus(bus);
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});

    auto* io = new IORegistry();
    desc->ioRegistry = io;

    auto* antic = new Antic();
    auto* gtia  = new GTIA();
    auto* pokey = new POKEY();
    auto* pia   = new PIA6520("PIA", 0xD300);
    antic->setDmaBus(bus);
    antic->setGtia(gtia);
    pokey->setClockHz(CLOCK_HZ);
    bus->setDevices(gtia, pokey, pia, antic);

    auto* stick = new JoystickPort();
    desc->deleters.push_back([stick]() { delete stick; });
    pia->setPortADevice(stick);
    BankingPort* banking = nullptr;
    if (xl) {
        banking = new BankingPort(bus);
        desc->deleters.push_back([banking]() { delete banking; });
        pia->setPortBDevice(banking);
    }

    // The serial bus: POKEY's serial lines, the PIA's CB2 as the command line
    auto* sio = new AtariSio(pokey);
    pokey->setSerialOut([sio](uint8_t b) { sio->serialOut(b); });
    pia->setCB2Line(sio->commandLine());

    // Interrupts: POKEY and the PIA share IRQ; ANTIC drives NMI
    auto* irq = new SharedIrqManager(cpu);
    desc->deleters.push_back([irq]() { delete irq; });
    pokey->setIrqLine(irq->createLine());
    pia->setIrqALine(irq->createLine());
    pia->setIrqBLine(irq->createLine());
    auto* nmi = new NmiLine(cpu);
    desc->deleters.push_back([nmi]() { delete nmi; });
    antic->setNmiLine(nmi);

    MachineDescriptor* self = desc;
    auto* keyboard = new AtariKeyboard(pokey, gtia, antic, bus, xl, [self]() {
        if (self->onReset) self->onReset(*self);
    });

    // POKEY first: the GUI and recordaudio take the first audio device
    io->registerOwnedHandler(pokey);
    io->registerOwnedHandler(antic);
    io->registerOwnedHandler(gtia);
    io->registerOwnedHandler(pia);
    io->registerOwnedHandler(keyboard);
    io->registerOwnedHandler(sio);

    desc->onKey = [keyboard](const std::string& name, bool down) { return keyboard->key(name, down); };
    desc->onJoystick = [stick, gtia](int port, uint8_t bits) {
        // bits 0-3: up, down, left, right; bit 4: fire (active low)
        stick->set(port, bits & 0x0F);
        gtia->setTrigger(port, !(bits & 0x10));
    };

    desc->onReset = [bus, banking, io, cpu](MachineDescriptor&) {
        io->resetAll();
        if (banking) banking->reset();
        bus->resetBanking();
        cpu->reset();
    };

    // One CPU instruction, or one cycle while ANTIC holds the CPU (WSYNC, DMA)
    desc->schedulerStep = [antic, io, cpu](MachineDescriptor&) -> int {
        if (antic->cpuBlocked()) {
            antic->consumeBlockedCycle();
            io->tickAll(1);
            return 1;
        }
        int c = cpu->step();
        io->tickAll((uint64_t)c);
        return c;
    };

    // Real time (#169): pace ANTIC's cycle count, which includes the CPU's waits
    desc->clockHz = CLOCK_HZ;
    desc->deviceCycles = [antic]() { return antic->totalCycles(); };

    desc->onReset(*desc);
    return desc;
}

namespace {
template <class T> T* findHandler(MachineDescriptor* m) {
    if (!m || !m->ioRegistry) return nullptr;
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if (auto* t = dynamic_cast<T*>(h)) return t;
    return nullptr;
}
}  // namespace

bool bootDisk(MachineDescriptor* m, std::vector<uint8_t> atr, const std::string& label, std::string& err) {
    auto* sio = findHandler<AtariSio>(m);
    auto* kbd = findHandler<AtariKeyboard>(m);
    if (!sio || !kbd || !m->onReset) { err = "Not an Atari 8-bit machine."; return false; }
    if (!sio->mount(0, std::move(atr), label, err)) return false;
    m->onReset(*m);
    kbd->holdConsoleKey(GTIA::CONSOL_OPTION, CLOCK_HZ * 3);   // the OS reads it early in the cold start
    return true;
}

bool bootExecutable(MachineDescriptor* m, const std::vector<uint8_t>& xex,
                    const std::string& label, std::string& err) {
    std::vector<uint8_t> atr;
    if (!xexToAtr(xex, atr, err)) return false;
    return bootDisk(m, std::move(atr), label, err);
}

}  // namespace atari
