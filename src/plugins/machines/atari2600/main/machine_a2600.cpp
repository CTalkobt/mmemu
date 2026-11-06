// Atari 2600: the 6507's memory map, cartridge bank switching and console
// switches after Stella (src/emucore/System, Cart*.cxx, CartDetector.cxx,
// Switches.cxx), GPL-2.0-or-later, Stella team.

#include "machine_a2600.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/io_handler.h"
#include "libdebug/main/execution_observer.h"
#include "libmem/main/ibus.h"
#include "plugins/6502/main/cpu6502.h"
#include "plugins/devices/tia/main/tia.h"
#include "plugins/devices/riot6532/main/riot6532.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <vector>

namespace a2600 {
namespace {

using Rom = std::vector<uint8_t>;

bool search(const Rom& img, std::initializer_list<uint8_t> sig, int min = 1) {
    std::vector<uint8_t> s(sig);
    int n = 0;
    for (size_t i = 0; i + s.size() <= img.size(); ++i)
        if (std::equal(s.begin(), s.end(), img.begin() + (long)i) && ++n >= min) return true;
    return false;
}

// CartDetector::isProbablySC: each 4K bank repeats its first 128 bytes
bool isSC(const Rom& img) {
    for (size_t at = 0; at + 4096 <= img.size(); at += 4096)
        if (std::memcmp(&img[at], &img[at + 128], 128) != 0) return false;
    return img.size() >= 4096;
}
bool isE0(const Rom& img) {
    for (auto sig : {std::initializer_list<uint8_t>{0x8D, 0xE0, 0x1F}, {0x8D, 0xE0, 0x5F}, {0x8D, 0xE9, 0xFF},
                     {0x0C, 0xE0, 0x1F}, {0xAD, 0xE0, 0x1F}, {0xAD, 0xE9, 0xFF}, {0xAD, 0xED, 0xFF}, {0xAD, 0xF3, 0xBF}})
        if (search(img, sig)) return true;
    return false;
}
bool is3F(const Rom& img) { return search(img, {0x85, 0x3F}, 2); }   // STA $3F twice

}  // namespace

// CartDetector::autodetectType, for the types supported here
std::string detectType(const std::vector<uint8_t>& img) {
    const size_t n = img.size();
    if (n <= 2048 || (n == 4096 && std::memcmp(img.data(), img.data() + 2048, 2048) == 0)) return "2K";
    if (n == 4096) return "4K";
    if (n == 8192) {
        if (isSC(img)) return "F8SC";
        if (std::memcmp(img.data(), img.data() + 4096, 4096) == 0) return "4K";
        if (isE0(img)) return "E0";
        if (is3F(img)) return "3F";
        return "F8";
    }
    if (n == 12288) return "FA";
    if (n == 16384) return isSC(img) ? "F6SC" : "F6";
    if (n == 32768) return isSC(img) ? "F4SC" : (is3F(img) ? "3F" : "F4");
    if (is3F(img)) return "3F";
    return "";
}

namespace {

/** A cartridge and its bank switching (Stella CartEnhanced, CartE0, Cart3F, CartFA). */
class Cart {
public:
    Cart(Rom rom, std::string type) : m_rom(std::move(rom)), m_type(std::move(type)) {
        if (m_type.size() > 2 && m_type.substr(m_type.size() - 2) == "SC") m_ram.assign(128, 0);
        if (m_type == "FA") m_ram.assign(256, 0);
        if (m_type == "2K") { Rom r(4096); for (size_t i = 0; i < 4096; ++i) r[i] = m_rom[i % m_rom.size()]; m_rom = r; }
        reset();
    }
    const std::string& type() const { return m_type; }
    int banks() const { return (int)(m_rom.size() / 4096); }

    void reset() {
        m_bank = std::max(0, banks() - 1);      // the last bank (its reset vector)
        m_slice[0] = 4; m_slice[1] = 5; m_slice[2] = 6; m_slice[3] = 7;   // E0
        m_bank3f = 0;
        std::fill(m_ram.begin(), m_ram.end(), 0);
    }

    // a: $000-$FFF of the cartridge space
    uint8_t read(uint16_t a, bool peek) {
        if (!peek) hotspot(a);
        if (!m_ram.empty()) {
            const uint16_t rs = (uint16_t)m_ram.size();
            if (a >= rs && a < 2 * rs) return m_ram[a - rs];          // the read port
            if (a < rs) return m_ram[a];                              // the write port (Stella: garbage)
        }
        return m_rom[offset(a)];
    }
    void write(uint16_t a, uint8_t v) {
        hotspot(a);
        if (!m_ram.empty() && a < m_ram.size()) m_ram[a] = v;
    }
    /** A write below $1000 the cartridge sees: 3F's bank select at $00-$3F. */
    void lowWrite(uint16_t addr13, uint8_t v) {
        if (m_type == "3F" && addr13 <= 0x3F) m_bank3f = v % (int)(m_rom.size() / 2048);
    }
    std::string state() const {
        if (m_type == "E0") return "slices " + std::to_string(m_slice[0]) + "," + std::to_string(m_slice[1]) + "," + std::to_string(m_slice[2]);
        if (m_type == "3F") return "bank " + std::to_string(m_bank3f);
        return "bank " + std::to_string(m_bank);
    }

private:
    uint32_t offset(uint16_t a) const {
        if (m_type == "E0") return (uint32_t)(m_slice[a >> 10] * 1024 + (a & 0x3FF));
        if (m_type == "3F") {
            if (a < 0x800) return (uint32_t)(m_bank3f * 2048 + a);
            return (uint32_t)(m_rom.size() - 2048 + (a & 0x7FF));
        }
        return (uint32_t)(m_bank * 4096 + a);
    }
    void hotspot(uint16_t a) {
        const std::string base = m_type.substr(0, 2);
        if (base == "F8" && a >= 0xFF8 && a <= 0xFF9) m_bank = a - 0xFF8;
        else if (base == "F6" && a >= 0xFF6 && a <= 0xFF9) m_bank = a - 0xFF6;
        else if (base == "F4" && a >= 0xFF4 && a <= 0xFFB) m_bank = a - 0xFF4;
        else if (m_type == "FA" && a >= 0xFF8 && a <= 0xFFA) m_bank = a - 0xFF8;
        else if (m_type == "E0" && a >= 0xFE0 && a <= 0xFF7) m_slice[(a - 0xFE0) >> 3] = a & 7;
    }

    Rom m_rom;
    std::string m_type;
    std::vector<uint8_t> m_ram;
    int m_bank = 0, m_bank3f = 0;
    int m_slice[4] = {4, 5, 6, 7};
};

/**
 * The 6507's bus. The TIA and RIOT are brought up to the cycle of each
 * read; writes to them are held until the instruction ends and applied on
 * its last cycles (where the 6502 writes), then WSYNC stalls the CPU.
 */
class A2600Bus : public IBus {
public:
    A2600Bus(Tia* tia, Riot6532* riot) : m_tia(tia), m_riot(riot) { m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF}; }

    void setCart(std::unique_ptr<Cart> c, std::string label) { m_cart = std::move(c); m_label = std::move(label); }
    Cart* cart() const { return m_cart.get(); }
    const std::string& label() const { return m_label; }

    uint64_t cycles() const { return m_cycles; }
    void resetClock() { m_cycles = m_tiaCycle = 0; m_pending.clear(); }

    void begin() { m_access = 0; }
    /** After an instruction of `c` cycles: the held writes, WSYNC; returns the stall. */
    int end(int c) {
        const uint64_t base = m_cycles;
        const size_t n = m_pending.size();
        for (size_t i = 0; i < n; ++i) {
            const uint64_t at = base + (uint64_t)c - (n - i);
            syncTo(at);
            apply(m_pending[i].first, m_pending[i].second);
        }
        m_pending.clear();
        int stall = 0;
        if (m_tia->haltRequested()) {               // WSYNC: until the line ends
            syncTo(base + (uint64_t)c);
            stall = (int)((m_tia->clocksToLineEnd() + 2) / 3);
            m_tia->clearHalt();
        }
        m_cycles = base + (uint64_t)c + (uint64_t)stall;
        syncTo(m_cycles);
        return stall;
    }

    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "system"; }

    uint8_t read8(uint32_t addr) override {
        const uint16_t a = addr & 0x1FFF;
        const uint64_t at = m_cycles + m_access++;
        uint8_t v;
        if (a & 0x1000) v = m_cart ? m_cart->read(a & 0x0FFF, false) : 0xFF;
        else if (!(a & 0x80)) { syncTo(at); v = m_tia->read(a & 0x0F, m_dataBus); }
        else if (!(a & 0x200)) v = m_riot->ram(a);
        else { syncTo(at); v = m_riot->read(a); }
        m_dataBus = v;
        if (m_observer) m_observer->onMemoryRead(this, addr & 0xFFFF, v);
        return v;
    }
    uint8_t peek8(uint32_t addr) override {
        const uint16_t a = addr & 0x1FFF;
        if (a & 0x1000) return m_cart ? m_cart->read(a & 0x0FFF, true) : 0xFF;
        if (!(a & 0x80)) return m_tia->peek(a & 0x0F);
        return m_riot->peek(a);
    }
    void write8(uint32_t addr, uint8_t v) override {
        const uint16_t a = addr & 0x1FFF;
        ++m_access;
        m_dataBus = v;
        if (m_observer) m_observer->onMemoryWrite(this, addr & 0xFFFF, peek8(addr), v);
        if (a & 0x1000) { if (m_cart) m_cart->write(a & 0x0FFF, v); return; }
        if (m_cart) m_cart->lowWrite(a, v);
        if ((a & 0x80) && !(a & 0x200)) { m_riot->write(a, v); return; }   // RAM: at once
        m_pending.push_back({a, v});
    }

    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

private:
    void syncTo(uint64_t cycle) {
        if (cycle > m_tiaCycle) {
            m_tia->clock((uint32_t)((cycle - m_tiaCycle) * 3));
            m_tiaCycle = cycle;
        }
        m_riot->sync(cycle);
    }
    void apply(uint16_t a, uint8_t v) {
        if (!(a & 0x80)) m_tia->write(a & 0x3F, v);
        else m_riot->write(a, v);
    }

    BusConfig m_cfg;
    Tia* m_tia;
    Riot6532* m_riot;
    std::unique_ptr<Cart> m_cart;
    std::string m_label;
    uint64_t m_cycles = 0, m_tiaCycle = 0;
    uint32_t m_access = 0;
    uint8_t m_dataBus = 0;
    std::vector<std::pair<uint16_t, uint8_t>> m_pending;
};

A2600Bus* busOf(MachineDescriptor* m) {
    if (!m) return nullptr;
    for (auto& b : m->buses)
        if (auto* p = dynamic_cast<A2600Bus*>(b.bus)) return p;
    return nullptr;
}

}  // namespace

bool insertCartridge(MachineDescriptor* m, const std::vector<uint8_t>& image,
                     const std::string& label, std::string& err) {
    auto* bus = busOf(m);
    if (!bus) { err = "Not an Atari 2600."; return false; }
    std::string type = detectType(image);
    if (type.empty()) { err = "unsupported cartridge size " + std::to_string(image.size()); return false; }
    bus->setCart(std::make_unique<Cart>(image, type), label + " (" + type + ")");
    if (m->onReset) m->onReset(*m);
    return true;
}

void removeCartridge(MachineDescriptor* m) {
    auto* bus = busOf(m);
    if (!bus) return;
    bus->setCart(nullptr, "");
    if (m->onReset) m->onReset(*m);
}

std::string cartridgeInfo(MachineDescriptor* m) {
    auto* bus = busOf(m);
    return bus && bus->cart() ? bus->label() : "";
}

MachineDescriptor* create(bool pal, std::string* /*err*/) {
    auto* desc = new MachineDescriptor();
    desc->machineId = pal ? "atari2600-pal" : "atari2600";
    desc->displayName = pal ? "Atari 2600 (PAL)" : "Atari 2600";
    desc->licenseClass = "proprietary";
    desc->uiPanesSet = true;
    desc->uiPanes = {"cartridge"};
    desc->clockHz = pal ? 3546894 / 3 : 3579545 / 3;

    auto* tia = new Tia(pal ? Tia::Standard::PAL : Tia::Standard::NTSC);
    auto* riot = new Riot6532();
    auto* bus = new A2600Bus(tia, riot);
    desc->buses.push_back({"system", bus});
    auto* cpu = new MOS6502();
    cpu->setDataBus(bus);
    cpu->setCodeBus(bus);
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});

    auto* io = new IORegistry();
    desc->ioRegistry = io;
    io->registerHandler(tia);
    io->registerHandler(riot);
    desc->deleters.push_back([tia, riot]() { delete tia; delete riot; });

    // Joysticks on SWCHA (P0 the high nibble: right, left, down, up = bits
    // 7-4, 0 = closed; the same order as the active-low joystick bits),
    // fire on INPT4/5; the console switches on SWCHB (Stella Switches.cxx:
    // reset bit 0, select bit 1, colour bit 3, difficulty P0 bit 6, P1 bit 7)
    auto* joy = new uint8_t[2]{0x1F, 0x1F};
    auto* sw = new uint8_t(0x0B);                  // reset/select up, colour, both difficulties B
    desc->deleters.push_back([joy, sw]() { delete[] joy; delete sw; });
    riot->setPortA([joy]() { return (uint8_t)((joy[0] & 0x0F) << 4 | (joy[1] & 0x0F)); });
    riot->setPortB([sw]() { return *sw; });
    desc->joystickPorts = 2;
    desc->primaryJoystickPort = 0;
    desc->onJoystick = [joy, tia](int port, uint8_t bits) {
        if (port < 0 || port > 1) return;
        joy[port] = bits & 0x1F;
        tia->setButton(port, !(bits & 0x10));
    };
    desc->onKey = [sw](const std::string& name, bool down) {
        std::string n = name;
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        auto bit = [sw](uint8_t mask, bool set) { *sw = set ? (uint8_t)(*sw | mask) : (uint8_t)(*sw & ~mask); };
        if (n == "f2" || n == "game_reset") { bit(0x01, !down); return true; }
        if (n == "f1" || n == "game_select") { bit(0x02, !down); return true; }
        if (!down) return n == "f3" || n == "f4" || n == "f5" || n == "f6" || n == "f7" || n == "f8";
        if (n == "f3" || n == "color") { bit(0x08, true); return true; }
        if (n == "f4" || n == "bw") { bit(0x08, false); return true; }
        if (n == "f5") { bit(0x40, true); return true; }         // left difficulty A
        if (n == "f6") { bit(0x40, false); return true; }        //                 B
        if (n == "f7") { bit(0x80, true); return true; }
        if (n == "f8") { bit(0x80, false); return true; }
        return false;
    };
    desc->screenButtons = {
        {"RESET", {"game_reset"}, "The console's RESET switch (F2)."},
        {"SELECT", {"game_select"}, "The console's SELECT switch (F1)."}};

    desc->onReset = [bus, io, cpu](MachineDescriptor&) {
        io->resetAll();
        if (bus->cart()) bus->cart()->reset();
        bus->resetClock();
        cpu->reset();
    };
    desc->schedulerStep = [bus, cpu](MachineDescriptor&) -> int {
        bus->begin();
        int c = cpu->step();
        return c + bus->end(c);
    };
    return desc;
}

}  // namespace a2600
