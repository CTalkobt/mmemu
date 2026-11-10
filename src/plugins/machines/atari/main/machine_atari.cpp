#include "machine_atari.h"
#include "atari_sio.h"
#include "atari_cart.h"
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
#include <functional>
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
 * ANTIC, each mirrored through its page; $D500-$D5FF the cartridge's
 * control lines. A cartridge in the slot covers $8000-$9FFF and
 * $A000-$BFFF while it enables them (over BASIC: on the 800 it takes
 * BASIC's place, on the XL/XE it overrides the built-in one).
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

    /** The cartridge (nullptr: the slot is empty); `onChange` after a bank switch. */
    void setCartridge(Cartridge* cart) { m_cart = cart; resetBanking(); }
    Cartridge* cartridge() const { return m_cart; }
    void setCartChanged(std::function<void()> fn) { m_cartChanged = std::move(fn); }

    /** XL/XE banking from PIA port B's output. */
    void setPortB(uint8_t v) {
        if (!m_xl) return;
        m_osOn = v & 0x01;
        m_basicOn = !(v & 0x02);
        m_selfTestOn = !(v & 0x80) && m_osOn;
        xeBanking(v);
    }

    /**
     * Extended RAM banked into $4000-$7FFF by PORTB (atari800 memory.c
     * MEMORY_HandlePORTB): bit 4 low gives the CPU the selected bank, bit 5
     * low ANTIC (on the 130XE and Compy-Shop types; on the others bit 5
     * picks a bank and ANTIC sees what the CPU does). The bank bits:
     * 128K (130XE) 2-3; 192K 2, 3, 6; 320K Rambo 2, 3, 5, 6; 320K Compy-Shop
     * 2, 3, 6, 7; 576K 1, 2, 3, 5, 6; 1088K 1, 2, 3, 5, 6, 7. The self-test
     * ROM keeps $5000-$57FF while it's on.
     */
    enum class XeRam { None, K128, K192, Rambo320, Compy320, K576, K1088 };
    void setXeRam(XeRam type) {
        static const int BANKS[] = {0, 4, 8, 16, 16, 32, 64};
        m_xeType = type;
        m_xe.assign((size_t)BANKS[(int)type] * 0x4000, 0);
        m_cpuBank = m_anticBank = -1;
        xeBanking(m_portB);
    }
    XeRam xeRam() const { return m_xeType; }
    int cpuBank() const { return m_cpuBank; }
    int anticBank() const { return m_anticBank; }
    /** What ANTIC fetches: its own bank on the 130XE and Compy-Shop types. */
    uint8_t anticRead(uint32_t addr) {
        addr &= 0xFFFF;
        if (m_anticBank >= 0 && addr >= 0x4000 && addr < 0x8000 && !(m_selfTestOn && addr >= 0x5000 && addr < 0x5800))
            return m_xe[(size_t)m_anticBank * 0x4000 + (addr - 0x4000)];
        return read(addr, true);
    }
    void resetBanking() {
        if (m_xl) setPortB(0xFF);           // PIA reset: inputs, pulled high
        else { m_osOn = true; m_basicOn = !m_basic.empty() && !m_cart; m_selfTestOn = false; }
    }
    bool basicOn() const { return m_basicOn; }
    /** The extended bank the CPU sees at `addr`, or -1 for base RAM. */
    int xeIndex(uint32_t addr) const {
        if (m_cpuBank < 0 || addr < 0x4000 || addr >= 0x8000) return -1;
        if (m_selfTestOn && addr >= 0x5000 && addr < 0x5800) return -1;
        return m_cpuBank;
    }
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
        if (addr >= 0xD500 && addr < 0xD600) {
            if (m_cart) { m_cart->writeCctl((uint16_t)addr, val); if (m_cartChanged) m_cartChanged(); }
        } else if (addr >= 0xD000 && addr < 0xD800) {
            IOHandler* h = nullptr;
            uint32_t a = ioAddr(addr, h);
            if (h) h->ioWrite(this, a, val);
        } else if (writable(addr)) {
            const int b = xeIndex(addr);
            if (b >= 0) m_xe[(size_t)b * 0x4000 + (addr - 0x4000)] = val;
            else m_ram[addr] = val;
        }
        if (m_observer) m_observer->onMemoryWrite(this, addr, before, val);
    }

    // Snapshots: RAM and banking
    // Snapshots: RAM, banking and the cartridge's bank
    size_t stateSize() const override { return sizeof(m_ram) + 7; }
    void saveState(uint8_t* buf) const override {
        std::memcpy(buf, m_ram, sizeof(m_ram));
        uint8_t* p = buf + sizeof(m_ram);
        p[0] = m_osOn; p[1] = m_basicOn; p[2] = m_selfTestOn;
        int32_t cs = m_cart ? m_cart->state() : 0;
        std::memcpy(p + 3, &cs, 4);
    }
    void loadState(const uint8_t* buf) override {
        std::memcpy(m_ram, buf, sizeof(m_ram));
        const uint8_t* p = buf + sizeof(m_ram);
        m_osOn = p[0]; m_basicOn = p[1]; m_selfTestOn = p[2];
        int32_t cs;
        std::memcpy(&cs, p + 3, 4);
        if (m_cart) { m_cart->setState(cs); if (m_cartChanged) m_cartChanged(); }
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
        if (m_cart && addr >= 0x8000 && addr < 0xC000 &&
            (addr < 0xA000 ? m_cart->rd4() : m_cart->rd5())) return false;
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
        if (addr >= 0xD500 && addr < 0xD600) {
            if (!m_cart || peek) return 0xFF;
            uint8_t v = m_cart->readCctl((uint16_t)addr);
            if (m_cartChanged) m_cartChanged();
            return v;
        }
        if (m_cart && addr >= 0x8000 && addr < 0xC000 &&
            (addr < 0xA000 ? m_cart->rd4() : m_cart->rd5()))
            return m_cart->read((uint16_t)addr);
        if (addr >= 0xD000 && addr < 0xD800) {
            IOHandler* h = nullptr;
            uint32_t a = ioAddr(addr, h);
            if (!h) return 0xFF;
            uint8_t v = 0xFF;
            if (peek) h->ioPeek(this, a, &v);      // no side effects (the PIA clears flags on read)
            else      h->ioRead(this, a, &v);
            return v;
        }
        if (m_xl) {
            if (m_osOn && ((addr >= 0xC000 && addr < 0xD000) || addr >= 0xD800))
                return m_os[addr - 0xC000];
            if (m_selfTestOn && addr >= 0x5000 && addr < 0x5800)
                return m_os[0x1000 + (addr - 0x5000)];   // the ROM's $D000-$D7FF part
            if (m_basicOn && addr >= 0xA000 && addr < 0xC000)
                return m_basic[addr - 0xA000];
            const int b = xeIndex(addr);
            if (b >= 0) return m_xe[(size_t)b * 0x4000 + (addr - 0x4000)];
            return m_ram[addr];
        }
        if (addr >= 0xD800) return m_os[addr - 0xD800];  // OS-B: FP at $D800, OS at $E000
        if (addr >= 0xC000) return 0xFF;                 // nothing there on an 800
        if (m_basicOn && addr >= 0xA000) return m_basic[addr - 0xA000];
        return m_ram[addr];
    }

    void xeBanking(uint8_t v) {
        m_portB = v;
        m_cpuBank = m_anticBank = -1;
        if (m_xeType == XeRam::None || (v & 0x30) == 0x30) return;
        int bank = 0;
        switch (m_xeType) {
            case XeRam::K128:     bank = (v & 0x0C) >> 2; break;
            case XeRam::K192:     bank = ((v & 0x0C) + ((v & 0x40) >> 2)) >> 2; break;
            case XeRam::Rambo320: bank = ((v & 0x0C) + ((v & 0x60) >> 1)) >> 2; break;
            case XeRam::Compy320: bank = ((v & 0x0C) + ((v & 0xC0) >> 2)) >> 2; break;
            case XeRam::K576:     bank = ((v & 0x0E) + ((v & 0x60) >> 1)) >> 1; break;
            case XeRam::K1088:    bank = ((v & 0x0E) + ((v & 0xE0) >> 1)) >> 1; break;
            default: break;
        }
        if (!(v & 0x10)) m_cpuBank = bank;
        const bool ownAntic = m_xeType == XeRam::K128 || m_xeType == XeRam::Compy320;
        if (ownAntic) { if (!(v & 0x20)) m_anticBank = bank; }
        else m_anticBank = m_cpuBank;
    }

    BusConfig m_cfg;
    bool m_xl;
    XeRam m_xeType = XeRam::None;
    std::vector<uint8_t> m_xe;
    int m_cpuBank = -1, m_anticBank = -1;
    uint8_t m_portB = 0xFF;
    std::vector<uint8_t> m_os, m_basic;
    uint8_t m_ram[65536];
    bool m_osOn = true, m_basicOn = false, m_selfTestOn = false;
    Cartridge* m_cart = nullptr;
    std::function<void()> m_cartChanged;
    IOHandler *m_gtia = nullptr, *m_pokey = nullptr, *m_pia = nullptr, *m_antic = nullptr;
};

/** ANTIC's view of memory: the main bus but for its own extended bank. */
class AnticView : public IBus {
public:
    explicit AnticView(AtariBus* bus) : m_bus(bus) { m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF}; }
    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "antic"; }
    uint8_t read8(uint32_t a) override { return m_bus->anticRead(a); }
    uint8_t peek8(uint32_t a) override { return m_bus->anticRead(a); }
    void write8(uint32_t, uint8_t) override {}
    void reset() override {}
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}
private:
    AtariBus* m_bus;
    BusConfig m_cfg;
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
    AtariKeyboard(POKEY* pokey, GTIA* gtia, Antic* antic, IBus* bus, bool xl, uint64_t frameCycles,
                  std::function<void()> resetFn)
        : m_pokey(pokey), m_gtia(gtia), m_antic(antic), m_bus(bus), m_xl(xl), m_frame(frameCycles),
          m_reset(std::move(resetFn)) {}

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
        m_typeCycles += cycles;
        if (m_typeCycles < 2 * m_frame) return;
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
    uint64_t m_frame;            // cycles a frame
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

/**
 * Owns the cartridge in the slot (an I/O handler so the machine finds and
 * deletes it; it has no registers of its own: $D5xx goes through the bus).
 */
class CartridgeSlot : public IOHandler {
public:
    const char* name() const override { return "Cartridge"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}

    void insert(std::unique_ptr<Cartridge> c, const std::string& label) { m_cart = std::move(c); m_label = label; }
    Cartridge* cartridge() const { return m_cart.get(); }
    const std::string& label() const { return m_label; }
private:
    std::unique_ptr<Cartridge> m_cart;
    std::string m_label;
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

MachineDescriptor* create(Model model, std::string* err, bool pal) {
    const bool xl = model != Model::Atari800;
    std::vector<uint8_t> os, basic;
    if (!loadRom(xl ? "roms/atari/osxl.bin" : "roms/atari/osb.bin", xl ? 16384 : 10240, os, err)) return nullptr;
    if (!loadRom("roms/atari/basic.bin", 8192, basic, err)) return nullptr;

    auto* desc = new MachineDescriptor();
    // The ANTIC/GTIA graphics viewer: players/missiles and character modes
    desc->uiPanesSet = true;
    desc->uiPanes = {"drive", "cartridge", "graphics-antic"};
    desc->graphicsFormats = {"antic2", "antic4", "player", "player-2line"};
    switch (model) {
        case Model::Atari800:   desc->machineId = "atari800";   desc->displayName = "Atari 800"; break;
        case Model::Atari800XL: desc->machineId = "atari800xl"; desc->displayName = "Atari 800XL"; break;
        case Model::Atari65XE:  desc->machineId = "atari65xe";  desc->displayName = "Atari 65XE"; break;
    }
    if (pal) { desc->machineId += "-pal"; desc->displayName += " (PAL)"; }
    desc->licenseClass = "proprietary";
    const uint32_t clockHz = pal ? CLOCK_HZ_PAL : CLOCK_HZ;
    const uint64_t frameCycles = (uint64_t)Antic::CYCLES_PER_LINE * (pal ? Antic::LINES_PAL : Antic::LINES_NTSC);

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
    auto* anticView = new AnticView(bus);
    desc->deleters.push_back([anticView]() { delete anticView; });
    antic->setDmaBus(anticView);
    antic->setGtia(gtia);
    antic->setPal(pal);
    gtia->setPal(pal);
    pokey->setClockHz(clockHz);
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

    // The cartridge slot. On the XL/XE TRIG3 reads the cartridge's RD5
    // line (the OS checks it every vertical blank and stops if it changes
    // unexpectedly); on the 800 it is joystick 3's trigger.
    auto* cart = new CartridgeSlot();   // registered below: the registry deletes it
    auto cartChanged = [gtia, bus, xl]() {
        if (xl) gtia->setTrigger(3, !(bus->cartridge() && bus->cartridge()->rd5()));
    };
    bus->setCartChanged(cartChanged);

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

    if (xl) {
        // Extended RAM in $4000-$7FFF banked by PORTB, fitted at run time
        // (atari800's sizes and bank bits)
        MachineDescriptor::ExpansionSlot ram;
        ram.name = "ram";
        ram.description = "Extended RAM banked into $4000-$7FFF by PORTB (the 130XE's, or an upgrade)";
        ram.devices = {{"xe", "Extended RAM: 128K (130XE, ANTIC banked apart), 192K, 320K Rambo, 320K Compy-Shop, 576K, 1088K (the machine's 64K included)",
                        {"128K", "192K", "320K-rambo", "320K-compy", "576K", "1088K"}, "128K"}};
        ram.install = [bus](const std::string& device, const std::string& option, std::string& err) {
            if (device.empty()) { bus->setXeRam(AtariBus::XeRam::None); return true; }
            if (device != "xe") { err = "no device '" + device + "' for the ram slot (xe)"; return false; }
            std::string o = option.empty() ? "128K" : option;
            for (auto& c : o) c = (char)toupper((unsigned char)c);
            AtariBus::XeRam t;
            if (o == "128K") t = AtariBus::XeRam::K128;
            else if (o == "192K") t = AtariBus::XeRam::K192;
            else if (o == "320K-RAMBO" || o == "320K") t = AtariBus::XeRam::Rambo320;
            else if (o == "320K-COMPY") t = AtariBus::XeRam::Compy320;
            else if (o == "576K") t = AtariBus::XeRam::K576;
            else if (o == "1088K") t = AtariBus::XeRam::K1088;
            else { err = "the sizes are 128K, 192K, 320K-rambo, 320K-compy, 576K and 1088K"; return false; }
            bus->setXeRam(t);
            return true;
        };
        ram.info = [bus]() -> std::string {
            static const char* const NAMES[] = {"", "128K", "192K", "320K Rambo", "320K Compy-Shop", "576K", "1088K"};
            const int t = (int)bus->xeRam();
            return t ? std::string("xe (") + NAMES[t] + ")" : "";
        };
        desc->expansionSlots.push_back(std::move(ram));
    }

    MachineDescriptor* self = desc;
    auto* keyboard = new AtariKeyboard(pokey, gtia, antic, bus, xl, frameCycles, [self]() {
        if (self->onReset) self->onReset(*self);
    });

    // POKEY first: the GUI and recordaudio take the first audio device
    io->registerOwnedHandler(pokey);
    io->registerOwnedHandler(antic);
    io->registerOwnedHandler(gtia);
    io->registerOwnedHandler(pia);
    io->registerOwnedHandler(keyboard);
    io->registerOwnedHandler(sio);
    io->registerOwnedHandler(cart);

    desc->onKey = [keyboard](const std::string& name, bool down) { return keyboard->key(name, down); };
    desc->joystickPorts = 2;
    desc->onJoystick = [stick, gtia](int port, uint8_t bits) {
        // bits 0-3: up, down, left, right; bit 4: fire (active low)
        stick->set(port, bits & 0x0F);
        gtia->setTrigger(port, !(bits & 0x10));
    };

    desc->onReset = [bus, banking, io, cpu, cart, cartChanged](MachineDescriptor&) {
        io->resetAll();
        if (cart->cartridge()) cart->cartridge()->reset();
        if (banking) banking->reset();
        bus->resetBanking();
        cartChanged();
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
    desc->clockHz = clockHz;
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

AtariBus* atariBus(MachineDescriptor* m) {
    return m && !m->buses.empty() ? dynamic_cast<AtariBus*>(m->buses[0].bus) : nullptr;
}
}  // namespace

bool bootDisk(MachineDescriptor* m, std::vector<uint8_t> atr, const std::string& label, std::string& err,
              const std::string& path) {
    auto* sio = findHandler<AtariSio>(m);
    auto* kbd = findHandler<AtariKeyboard>(m);
    if (!sio || !kbd || !m->onReset) { err = "Not an Atari 8-bit machine."; return false; }
    if (!sio->mount(0, std::move(atr), label, err, path)) return false;
    m->onReset(*m);
    // The OS reads OPTION early in the cold start: held, the XL/XE boot with BASIC off
    kbd->holdConsoleKey(GTIA::CONSOL_OPTION, (uint64_t)m->clockHz * 3);
    return true;
}

bool bootExecutable(MachineDescriptor* m, const std::vector<uint8_t>& xex,
                    const std::string& label, std::string& err) {
    std::vector<uint8_t> atr;
    if (!xexToAtr(xex, atr, err)) return false;
    return bootDisk(m, std::move(atr), label, err);
}

bool insertCartridge(MachineDescriptor* m, const std::vector<uint8_t>& image,
                     const std::string& label, std::string& err) {
    auto* slot = findHandler<CartridgeSlot>(m);
    auto* bus = atariBus(m);
    if (!slot || !bus || !m->onReset) { err = "Not an Atari 8-bit machine."; return false; }
    auto cart = std::make_unique<Cartridge>();
    if (!cart->load(image, err)) return false;
    slot->insert(std::move(cart), label);
    bus->setCartridge(slot->cartridge());
    m->onReset(*m);   // the OS finds cartridges at power-on
    return true;
}

void removeCartridge(MachineDescriptor* m) {
    auto* slot = findHandler<CartridgeSlot>(m);
    auto* bus = atariBus(m);
    if (!slot || !bus) return;
    bus->setCartridge(nullptr);
    slot->insert(nullptr, "");
    if (m->onReset) m->onReset(*m);
}

std::string cartridgeInfo(MachineDescriptor* m) {
    auto* slot = findHandler<CartridgeSlot>(m);
    if (!slot || !slot->cartridge()) return "";
    return slot->label() + " (" + slot->cartridge()->typeName() + ")";
}

}  // namespace atari
