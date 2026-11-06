// Commodore Plus/4 and C16. Memory map, ROM banking, the 7501 I/O port and
// the keyboard after VICE (src/plus4/plus4mem.c, ted-mem.c ted08_store,
// data/PLUS4/gtk3_sym.vkm), GPL-2.0-or-later, VICE team.

#include "machine_plus4.h"
#include "libcore/main/intercept.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/io_handler.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/shared_irq_manager.h"
#include "libdebug/main/execution_observer.h"
#include "libmem/main/ibus.h"
#include "plugins/6502/main/cpu6502.h"
#include "plugins/devices/ted/main/ted.h"
#include "plugins/devices/iec_drive/main/iec_drive.h"
#include "plugins/devices/datasette/main/datasette.h"
#include "plugins/devices/acia6551/main/acia6551.h"
#include "libdevices/main/isignal_line.h"
#include <functional>
#include "util/path_util.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <vector>

namespace plus4 {
namespace {

using Rom = std::vector<uint8_t>;

bool loadRom(const std::string& rel, Rom& out, std::string* err) {
    std::string path = PathUtil::findResource(rel);
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "missing ROM " + rel; return false; }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (out.size() != 0x4000) { if (err) *err = rel + ": not 16 KB"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// The bus (plus4mem.c)
// ---------------------------------------------------------------------------

/** A signal line backed by the bus's state. */
class FnLine : public ISignalLine {
public:
    FnLine(std::function<bool()> get, std::function<void(bool)> set = nullptr)
        : m_get(std::move(get)), m_set(std::move(set)) {}
    bool get() const override { return m_get ? m_get() : true; }
    void set(bool level) override { if (m_set) m_set(level); }
    void pulse() override { set(false); set(true); }
private:
    std::function<bool()> m_get;
    std::function<void(bool)> m_set;
};

class Plus4Bus : public IBus {
public:
    Plus4Bus(Model model, Rom basic, Rom kernal, Rom funcLo, Rom funcHi)
        : m_ramMask(model == Model::Plus4 ? 0xFFFF : 0x3FFF),
          m_basic(std::move(basic)), m_kernal(std::move(kernal)),
          m_funcLo(std::move(funcLo)), m_funcHi(std::move(funcHi)) {
        m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF};
        powerOn();
    }
    void setTed(Ted* ted) { m_ted = ted; }
    void setAcia(IOHandler* acia) { m_acia = acia; }
    void setDrive(IecDrive* d) { m_drive = d; }

    /** The cartridge ROMs, C1 low/high (bank 2) and C2 low/high (bank 3),
     *  16 KB each; an empty one reads $FF (plus4cart.c c1lo/c1hi_read). */
    using CartRoms = std::array<Rom, 4>;
    void setCartridge(CartRoms roms, std::string label) { m_cart = std::move(roms); m_cartLabel = std::move(label); }
    const std::string& cartridgeLabel() const { return m_cartLabel; }

    // The cassette port (plus4mem.c mem_proc_port_store/read, plus4pio1.c)
    /** Motor on: port bit 3 low (or an input). */
    bool tapeMotorOn() const { return !(m_portDir & m_portData & 0x08); }
    /** The write line: port bit 1 (shared with the serial CLK output). */
    bool tapeWrite() const { return (~m_portDir | m_portData) & 0x02; }
    void setTapeRead(bool level) { m_tapeRead = level; }
    /** The sense switch on the 6529 at $FD10, bit 2 (low = a button down). */
    void setTapeSense(bool level) { m_tapeSense = level; }
    bool tapeSense() const { return m_tapeSense; }

    // Banking state
    void setRomOn(bool on) { m_romOn = on; }
    bool romOn() const { return m_romOn; }
    uint8_t romBanks() const { return m_banks; }        // bits 0-1 low, 2-3 high
    uint8_t keyboardLatch() const { return m_kbdLatch; }
    void resetBanks() { m_romOn = true; m_banks = 0; m_kbdLatch = 0xFF; m_portDir = 0; m_portData = 0; portChanged(); }
    void powerOn() {
        // VICE initialises RAM with a pattern; a fixed one keeps runs repeatable
        for (size_t i = 0; i < sizeof m_ram; ++i) m_ram[i] = (i & 0x40) ? 0xFF : 0x00;
        resetBanks();
    }

    uint8_t ramByte(uint16_t addr) const { return m_ram[addr & m_ramMask]; }
    /** What the TED sees: RAM, or ROM in the current banking (character data from ROM). */
    uint8_t tedFetch(uint16_t addr, bool fromRom) {
        if (fromRom && addr >= 0x8000) return romRead(addr);
        return m_ram[addr & m_ramMask];
    }

    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "system"; }

    uint8_t read8(uint32_t addr) override {
        addr &= 0xFFFF;
        uint8_t v = read((uint16_t)addr, false);
        if (m_observer) m_observer->onMemoryRead(this, addr, v);
        return v;
    }
    uint8_t peek8(uint32_t addr) override { return read((uint16_t)(addr & 0xFFFF), true); }

    void write8(uint32_t addr, uint8_t val) override {
        addr &= 0xFFFF;
        uint8_t before = m_observer ? read((uint16_t)addr, true) : 0;
        if (addr < 2) {
            if (addr == 0) m_portDir = val & 0xDF;
            else           m_portData = val;
            portChanged();
        }
        if (addr >= 0xFD00 && addr < 0xFF40 && io(addr, &val, true, false)) {
            // an I/O register
        } else {
            m_ram[addr & m_ramMask] = val;                  // writes under ROM go to RAM
        }
        if (m_observer) m_observer->onMemoryWrite(this, addr, before, val);
    }

    size_t stateSize() const override { return sizeof m_ram; }
    void saveState(uint8_t* buf) const override { std::memcpy(buf, m_ram, sizeof m_ram); }
    void loadState(const uint8_t* buf) override { std::memcpy(m_ram, buf, sizeof m_ram); }
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

private:
    uint8_t romRead(uint16_t addr) const {
        if (addr < 0xC000) {
            switch (m_banks & 3) {
                case 0: return m_basic[addr - 0x8000];
                case 1: return m_funcLo.empty() ? 0xFF : m_funcLo[addr - 0x8000];
                case 2: return cartByte(0, addr);           // C1 low
                default: return cartByte(2, addr);          // C2 low
            }
        }
        if (addr >= 0xFC00 && addr < 0xFD00) return m_kernal[addr - 0xC000];   // always the KERNAL
        switch ((m_banks >> 2) & 3) {
            case 0: return m_kernal[addr - 0xC000];
            case 1: return m_funcHi.empty() ? 0xFF : m_funcHi[addr - 0xC000];
            case 2: return cartByte(1, addr);               // C1 high
            default: return cartByte(3, addr);              // C2 high
        }
    }
    uint8_t cartByte(int rom, uint16_t addr) const {
        const Rom& r = m_cart[rom];
        return r.empty() ? 0xFF : r[addr & 0x3FFF];
    }

    uint8_t read(uint16_t addr, bool peek) {
        if (addr < 2) {
            if (addr == 0) return m_portDir;
            // 7501 port (mem_proc_port_read): serial CLK/DATA in (bits 6/7,
            // high = released), cassette read (4), motor and write in (3, 1)
            // idle high; bit 5 absent
            uint8_t input = 0xFF;
            if (!m_tapeRead) input &= (uint8_t)~0x10;
            if (m_drive) {
                if (!m_drive->clkLine())  input &= (uint8_t)~0x40;
                if (!m_drive->dataLine()) input &= (uint8_t)~0x80;
            }
            return (uint8_t)(((input & ~m_portDir) | (m_portData & m_portDir)) & 0xDF);
        }
        if (addr >= 0xFD00 && addr < 0xFF40) {
            uint8_t v = 0xFF;
            if (io(addr, &v, false, peek)) return v;
        }
        if (addr >= 0x8000 && m_romOn) return romRead(addr);
        return m_ram[addr & m_ramMask];
    }

    /** The serial bus outputs: bit 0 DATA, 1 CLK, 2 ATN; a 1 pulls the
     *  line low (through a 7406), an input bit leaves it released. */
    void portChanged() {
        if (!m_drive) return;
        uint8_t out = m_portData & m_portDir;
        m_drive->setComputerLines(out & 0x04, out & 0x02, out & 0x01);
    }

    /** An I/O access at $FD00-$FF3F; false where the TED leaves it to the
     *  RAM/ROM underneath ($FF20-$FF3D, and reads of $FF3E/$FF3F with ROM
     *  selected: plus4mem.c ram_ffxx_read / rom_ffxx_read / *_ffxx_store). */
    bool io(uint16_t addr, uint8_t* v, bool write, bool peek) {
        if (addr >= 0xFF00) {                               // TED
            if (!m_ted) return true;
            if (write)     return m_ted->ioWrite(this, addr, *v);
            if (peek)      return m_ted->ioPeek(this, addr, v);
            return m_ted->ioRead(this, addr, v);
        }
        if (addr < 0xFD10) {                                // ACIA (Plus/4 only)
            if (m_acia) {
                if (write)     m_acia->ioWrite(this, addr, *v);
                else if (peek) m_acia->ioPeek(this, addr, v);
                else           m_acia->ioRead(this, addr, v);
            }
            return true;
        }
        if (addr < 0xFD20) {                                // 6529B user port
            // Quasi-bidirectional: a pin reads low if written low or pulled
            // low outside (bit 2: the cassette sense switch)
            if (write) m_userPort = *v;
            else       *v = (uint8_t)(m_userPort & (m_tapeSense ? 0xFF : ~0x04));
            return true;
        }
        if (addr >= 0xFD30 && addr < 0xFD40) {              // 6529B keyboard row latch
            if (write) m_kbdLatch = *v; else *v = m_kbdLatch;
            return true;
        }
        if (addr >= 0xFDD0 && addr < 0xFDE0) {              // ROM bank select (write only)
            if (write) m_banks = addr & 0x0F;
            return true;
        }
        // $FE00-$FEFF (TCBM drives) and the rest: nothing there
        return true;
    }

    BusConfig m_cfg;
    uint16_t m_ramMask;
    uint8_t m_ram[0x10000];
    Rom m_basic, m_kernal, m_funcLo, m_funcHi;
    CartRoms m_cart;
    std::string m_cartLabel;
    Ted* m_ted = nullptr;
    IOHandler* m_acia = nullptr;
    IecDrive* m_drive = nullptr;
    bool m_tapeRead = true, m_tapeSense = true;
    bool m_romOn = true;
    uint8_t m_banks = 0;
    uint8_t m_kbdLatch = 0xFF, m_userPort = 0xFF;
    uint8_t m_portDir = 0, m_portData = 0;
};

// ---------------------------------------------------------------------------
// The keyboard: an 8x8 matrix, rows selected by the $FD30 latch (active
// low), columns read at TED $FF08. Positions from VICE's data/PLUS4/gtk3_sym.vkm.
// ---------------------------------------------------------------------------

class Plus4Keyboard : public IOHandler, public IKeyboardMatrix {
public:
    Plus4Keyboard() {
        static const struct { const char* name; int row, col; } keys[] = {
            {"delete", 0, 0}, {"return", 0, 1}, {"pound", 0, 2}, {"help", 0, 3},
            {"f1", 0, 4}, {"f2", 0, 5}, {"f3", 0, 6}, {"at", 0, 7},
            {"3", 1, 0}, {"w", 1, 1}, {"a", 1, 2}, {"4", 1, 3}, {"z", 1, 4}, {"s", 1, 5}, {"e", 1, 6}, {"left_shift", 1, 7},
            {"5", 2, 0}, {"r", 2, 1}, {"d", 2, 2}, {"6", 2, 3}, {"c", 2, 4}, {"f", 2, 5}, {"t", 2, 6}, {"x", 2, 7},
            {"7", 3, 0}, {"y", 3, 1}, {"g", 3, 2}, {"8", 3, 3}, {"b", 3, 4}, {"h", 3, 5}, {"u", 3, 6}, {"v", 3, 7},
            {"9", 4, 0}, {"i", 4, 1}, {"j", 4, 2}, {"0", 4, 3}, {"m", 4, 4}, {"k", 4, 5}, {"o", 4, 6}, {"n", 4, 7},
            {"down", 5, 0}, {"p", 5, 1}, {"l", 5, 2}, {"up", 5, 3}, {"period", 5, 4}, {"colon", 5, 5}, {"minus", 5, 6}, {"comma", 5, 7},
            {"left", 6, 0}, {"asterisk", 6, 1}, {"semicolon", 6, 2}, {"right", 6, 3}, {"escape", 6, 4}, {"equal", 6, 5}, {"plus", 6, 6}, {"slash", 6, 7},
            {"1", 7, 0}, {"home", 7, 1}, {"control", 7, 2}, {"2", 7, 3}, {"space", 7, 4}, {"cbm", 7, 5}, {"q", 7, 6}, {"run_stop", 7, 7},
        };
        for (const auto& k : keys) m_map[k.name] = {k.row, k.col};
        m_map["right_shift"] = m_map["left_shift"];          // one SHIFT line
        // Other names for the same keys (GUI and other machines)
        m_alias = {{"alt", "cbm"}, {"commodore", "cbm"}, {"ctrl", "control"}, {"kpeq", "equal"},
                   {"backspace", "delete"}, {"shift", "left_shift"}, {"f8", "help"}, {"clr", "home"}};
        // Characters typed with SHIFT
        m_shifted = {{"exclaim", "1"}, {"dquote", "2"}, {"hash", "3"}, {"dollar", "4"}, {"percent", "5"},
                     {"ampersand", "6"}, {"squote", "7"}, {"lparen", "8"}, {"rparen", "9"},
                     {"question", "slash"}, {"less", "comma"}, {"greater", "period"},
                     {"bracket_left", "colon"}, {"bracket_right", "semicolon"},
                     {"f4", "f1"}, {"f5", "f2"}, {"f6", "f3"}, {"f7", "help"}, {"insert", "delete"}};
        clearKeys();
    }

    /** $FF08: the columns of the rows the latch selects, plus the joysticks (ted08_store). */
    uint8_t read(uint8_t latch, uint8_t ff08) const {
        uint8_t v = 0xFF;
        for (int r = 0; r < 8; ++r)
            if (!(latch & (1 << r))) v &= m_rows[r];
        if (!(ff08 & 0x04)) {                               // joystick 1
            v &= (uint8_t)~(m_joy[0] & 0x0F);
            if (m_joy[0] & 0x10) v &= (uint8_t)~0x40;
        }
        if (!(ff08 & 0x02)) {                               // joystick 2
            v &= (uint8_t)~(m_joy[1] & 0x0F);
            if (m_joy[1] & 0x10) v &= (uint8_t)~0x80;
        }
        return v;
    }
    /** Joystick port 0/1: active-low up, down, left, right, fire. */
    void setJoystick(int port, uint8_t activeLow) { if (port >= 0 && port < 2) m_joy[port] = (uint8_t)(~activeLow & 0x1F); }

    bool key(const std::string& name, bool down) {
        std::string n = name;
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (auto a = m_alias.find(n); a != m_alias.end()) n = a->second;
        auto it = m_map.find(n);
        if (it == m_map.end()) {
            auto s = m_shifted.find(n);
            if (s == m_shifted.end()) return false;
            set("left_shift", down);
            set(s->second, down);
            return true;
        }
        set(n, down);
        return true;
    }

    // IOHandler (ticks the typing queue)
    const char* name() const override { return "Keyboard"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override { m_queue.clear(); clearKeys(); }
    void tick(uint64_t cycles) override {
        if (m_queue.empty()) return;
        if (m_timer > cycles) { m_timer -= cycles; return; }
        auto ev = m_queue.front();
        m_queue.pop_front();
        if (ev.first.empty()) clearKeys(); else key(ev.first, ev.second);
        m_timer = 40000;                                    // ~23 ms at the TED clock
    }

    // IKeyboardMatrix
    void keyDown(int row, int col) override { if (row >= 0 && row < 8) m_rows[row] &= (uint8_t)~(1 << col); }
    void keyUp(int row, int col) override { if (row >= 0 && row < 8) m_rows[row] |= (uint8_t)(1 << col); }
    void clearKeys() override { std::memset(m_rows, 0xFF, sizeof m_rows); }
    bool pressKeyByName(const std::string& n, bool down) override { return key(n, down); }
    IPortDevice* getPort(int) override { return nullptr; }
    void enqueueText(const std::string& text) override {
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            std::string name;
            if (c == '\\' && i + 1 < text.size() && (text[i + 1] == 'n' || text[i + 1] == 'r')) { ++i; name = "return"; }
            else if (c == '\n' || c == '\r') name = "return";
            else if (std::isalpha((unsigned char)c)) name = std::string(1, (char)std::tolower((unsigned char)c));   // plain key: capitals
            else if (std::isdigit((unsigned char)c)) name = std::string(1, c);
            else {
                static const std::map<char, const char*> sym = {
                    {' ', "space"}, {'!', "exclaim"}, {'"', "dquote"}, {'#', "hash"}, {'$', "dollar"}, {'%', "percent"},
                    {'&', "ampersand"}, {'\'', "squote"}, {'(', "lparen"}, {')', "rparen"}, {'*', "asterisk"},
                    {'+', "plus"}, {',', "comma"}, {'-', "minus"}, {'.', "period"}, {'/', "slash"}, {':', "colon"},
                    {';', "semicolon"}, {'<', "less"}, {'=', "equal"}, {'>', "greater"}, {'?', "question"}, {'@', "at"},
                    {'[', "bracket_left"}, {']', "bracket_right"}};
                auto it = sym.find(c);
                if (it == sym.end()) continue;
                name = it->second;
            }
            m_queue.push_back({name, true});
            m_queue.push_back({name, false});
            m_queue.push_back({"", false});
        }
    }

private:
    void set(const std::string& n, bool down) {
        auto it = m_map.find(n);
        if (it == m_map.end()) return;
        if (down) keyDown(it->second.first, it->second.second);
        else      keyUp(it->second.first, it->second.second);
    }
    std::map<std::string, std::pair<int, int>> m_map;
    std::map<std::string, std::string> m_alias, m_shifted;
    uint8_t m_rows[8];
    uint8_t m_joy[2] = {0, 0};
    std::deque<std::pair<std::string, bool>> m_queue;
    uint64_t m_timer = 0;
};

/**
 * A cartridge image into the four 16 KB ROMs, after VICE (plus4cart.c
 * cartridge_detect, plus4-generic.c generic_bin_attach / generic_crt_attach):
 * a raw 8 or 16 KB image is C1 low, 32 KB C1 low and high; a .crt has the
 * header "PLUS4 CARTRIDGE ", hardware type 0 (generic) and CHIP packets with
 * bank 0 (C1) or 1 (C2) at $8000 (low) or $C000 (high), 4, 8 or 16 KB.
 * A short ROM is padded with $FF.
 */
bool parseCartridge(const std::vector<uint8_t>& img, Plus4Bus::CartRoms& roms, std::string& type, std::string& err) {
    auto put = [&](int rom, const uint8_t* data, size_t n) {
        roms[rom].assign(0x4000, 0xFF);
        std::memcpy(roms[rom].data(), data, std::min<size_t>(n, 0x4000));
    };
    auto be16 = [&](size_t o) { return (uint32_t)(img[o] << 8 | img[o + 1]); };
    auto be32 = [&](size_t o) { return be16(o) << 16 | be16(o + 2); };
    static const char kSig[] = "PLUS4 CARTRIDGE ";
    if (img.size() >= 0x40 && std::memcmp(img.data(), kSig, 16) == 0) {
        if (be16(0x16) != 0) { err = "unsupported .crt hardware type " + std::to_string(be16(0x16)) + " (only generic, 0)"; return false; }
        size_t o = std::max<uint32_t>(be32(0x10), 0x40);
        int chips = 0;
        while (o + 16 <= img.size() && std::memcmp(&img[o], "CHIP", 4) == 0) {
            uint32_t len = be32(o + 4), bank = be16(o + 10), start = be16(o + 12), size = be16(o + 14);
            if (len < 16 + size || o + 16 + size > img.size()) { err = "truncated CHIP packet"; return false; }
            if (bank > 1 || (start != 0x8000 && start != 0xC000) ||
                (size != 0x1000 && size != 0x2000 && size != 0x4000)) {
                char msg[96];
                std::snprintf(msg, sizeof msg, "unsupported CHIP packet (bank %u, $%04X, %u bytes)",
                              (unsigned)bank, (unsigned)start, (unsigned)size);
                err = msg;
                return false;
            }
            put((int)bank * 2 + (start == 0xC000), &img[o + 16], size);
            ++chips;
            o += len;
        }
        if (!chips) { err = "no CHIP packets"; return false; }
        std::string name(img.begin() + 0x20, img.begin() + 0x40);
        name = name.substr(0, name.find('\0'));
        type = "Plus/4 .crt" + (name.empty() ? std::string() : ": " + name);
        return true;
    }
    if (img.size() == 0x2000 || img.size() == 0x4000) {
        put(0, img.data(), img.size());
        type = std::to_string(img.size() / 1024) + " KB C1 low";
        return true;
    }
    if (img.size() == 0x8000) {
        put(0, img.data(), 0x4000);
        put(1, img.data() + 0x4000, 0x4000);
        type = "32 KB C1 low/high";
        return true;
    }
    err = "not a Plus/4 cartridge (a raw 8, 16 or 32 KB image, or a .crt with a PLUS4 CARTRIDGE header)";
    return false;
}

Plus4Bus* plus4Bus(MachineDescriptor* m) {
    if (!m) return nullptr;
    for (auto& b : m->buses)
        if (auto* p = dynamic_cast<Plus4Bus*>(b.bus)) return p;
    return nullptr;
}

} // namespace

bool insertCartridge(MachineDescriptor* m, const std::vector<uint8_t>& image,
                     const std::string& label, std::string& err) {
    auto* bus = plus4Bus(m);
    if (!bus) { err = "Not a Plus/4, C16 or C116."; return false; }
    Plus4Bus::CartRoms roms;
    std::string type;
    if (!parseCartridge(image, roms, type, err)) return false;
    bus->setCartridge(std::move(roms), label + " (" + type + ")");
    if (m->onReset) m->onReset(*m);   // the KERNAL polls the banks at reset
    return true;
}

void removeCartridge(MachineDescriptor* m) {
    auto* bus = plus4Bus(m);
    if (!bus) return;
    bus->setCartridge({}, "");
    if (m->onReset) m->onReset(*m);
}

std::string cartridgeInfo(MachineDescriptor* m) {
    auto* bus = plus4Bus(m);
    return bus ? bus->cartridgeLabel() : "";
}

MachineDescriptor* create(Model model, bool ntsc, std::string* err) {
    Rom basic, kernal, funcLo, funcHi;
    if (!loadRom("roms/plus4/basic.318006-01.bin", basic, err)) return nullptr;
    if (!loadRom(ntsc ? "roms/plus4/kernal.318005-05.bin" : "roms/plus4/kernal.318004-05.bin", kernal, err))
        return nullptr;
    if (model == Model::Plus4) {
        if (!loadRom("roms/plus4/3-plus-1.317053-01.bin", funcLo, err)) return nullptr;
        if (!loadRom("roms/plus4/3-plus-1.317054-01.bin", funcHi, err)) return nullptr;
    }

    auto* desc = new MachineDescriptor();
    const char* id = model == Model::C16 ? "c16" : model == Model::C116 ? "c116" : "plus4";
    desc->machineId = std::string(id) + (ntsc ? "-ntsc" : "");
    desc->displayName = model == Model::C16  ? "Commodore 16"
                      : model == Model::C116 ? "Commodore 116" : "Commodore Plus/4";
    if (ntsc) desc->displayName += " (NTSC)";
    desc->licenseClass = "proprietary";
    desc->uiPanesSet = true;
    desc->uiPanes = {"cartridge"};

    auto* bus = new Plus4Bus(model, std::move(basic), std::move(kernal), std::move(funcLo), std::move(funcHi));
    desc->buses.push_back({"system", bus});
    auto* cpu = new MOS6502();
    cpu->setDataBus(bus);
    cpu->setCodeBus(bus);
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});

    auto* io = new IORegistry();
    desc->ioRegistry = io;
    auto* ted = new Ted(ntsc ? Ted::Standard::NTSC : Ted::Standard::PAL);
    auto* kbd = new Plus4Keyboard();
    io->registerHandler(ted);
    io->registerHandler(kbd);
    // Unit 8 on the serial bus: a host folder (the current directory) or a
    // disk image (`disk mount 8 <folder or image>`)
    auto* drive = new IecDrive(8);
    drive->setClockHz(Ted::clockHz(ntsc ? Ted::Standard::NTSC : Ted::Standard::PAL));
    io->registerHandler(drive);
    // The 1531 datasette on the cassette port (CLI `tape mount|play|record|save`)
    auto* tape = new Datasette();
    tape->setName("Tape");
    tape->setHalfWaveMode(true, ntsc);
    auto* motorLine = new FnLine([bus]() { return !bus->tapeMotorOn(); });   // low = running
    auto* writeLine = new FnLine([bus]() { return bus->tapeWrite(); });
    auto* readLine  = new FnLine(nullptr, [bus](bool l) { bus->setTapeRead(l); });
    auto* senseLine = new FnLine([bus]() { return bus->tapeSense(); }, [bus](bool l) { bus->setTapeSense(l); });
    tape->setMotorLine(motorLine);
    tape->setWriteLine(writeLine);
    tape->setReadPulseLine(readLine);
    tape->setSenseLine(senseLine);
    io->registerHandler(tape);
    desc->deleters.push_back([ted, kbd, drive, tape, motorLine, writeLine, readLine, senseLine]() {
        delete ted; delete kbd; delete drive; delete tape;
        delete motorLine; delete writeLine; delete readLine; delete senseLine;
    });
    bus->setTed(ted);
    bus->setDrive(drive);

    auto* irq = new SharedIrqManager(cpu);
    desc->deleters.push_back([irq]() { delete irq; });

    // The Plus/4's RS-232: a 6551 at $FD00 on the IRQ (the C16 has none;
    // CLI `serial loopback|tcp:PORT|none`)
    if (model == Model::Plus4) {
        auto* acia = new Acia6551(0xFD00);
        acia->setClockHz((uint32_t)Ted::clockHz(ntsc ? Ted::Standard::NTSC : Ted::Standard::PAL));
        acia->setIrqLine(irq->createLine());
        io->registerHandler(acia);
        bus->setAcia(acia);
        desc->deleters.push_back([acia]() { delete acia; });
    }
    ted->setIrqLine(irq->createLine());
    ted->setFetch([bus](uint16_t a, bool fromRom) { return bus->tedFetch(a, fromRom); });
    ted->setKeyboardRead([bus, kbd](uint8_t ff08) { return kbd->read(bus->keyboardLatch(), ff08); });
    ted->setRomSelect([bus](bool on) { bus->setRomOn(on); });

    desc->onKey = [kbd](const std::string& n, bool down) { return kbd->key(n, down); };
    // The KERNAL fetches file names and SAVE data from RAM under the ROMs
    // (BASIC 3.5 keeps strings at the top of RAM)
    desc->readRam = [bus](uint16_t a) { return bus->ramByte(a); };
    desc->joystickPorts = 2;
    desc->onJoystick = [kbd](int port, uint8_t bits) { kbd->setJoystick(port, bits); };

    desc->onReset = [bus, io, cpu](MachineDescriptor&) {
        bus->resetBanks();
        io->resetAll();
        cpu->reset();
    };

    // The CPU runs at the TED's single clock (half speed) while it says so
    // and loses the cycles TED takes for DMA; time is counted in TED cycles.
    desc->schedulerStep = [io, cpu, ted](MachineDescriptor&) -> int {
        int ticks = 0;
        while (ted->cpuBlocked()) {
            io->tickAll(1);
            ted->consumeBlockedCycle();
            ++ticks;
        }
        int c = cpu->step();
        int t = ted->cpuSingleClock() ? 2 * c : c;
        io->tickAll((uint64_t)t);
        return ticks + t;
    };
    desc->clockHz = Ted::clockHz(ntsc ? Ted::Standard::NTSC : Ted::Standard::PAL);

    // LOAD and SAVE from the host folder (KERNAL zero page from the ROM:
    // SETNAM $F40C stores $AB/$AF/$B0, SETLFS $F413 $AC/$AE/$AD, READST $90)
    installIntercepts(*desc, nlohmann::json::parse(R"({"intercepts":[
        {"type": "cbm.kernal.fileio", "name": "kernal-fileio", "load": "$FFD5", "save": "$FFD8", "path": ".",
         "zeroPage": {"status": "$90", "fnlen": "$AB", "sa": "$AD", "fa": "$AE", "fnadr": "$AF"}}]})"));
    return desc;
}

} // namespace plus4
