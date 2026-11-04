// Commander X16 machine. Memory map, I/O decode and timing after
// x16-emulator (src/memory.c, main.c, via.c), Copyright (c) Michael Steil and
// contributors, 2-clause BSD license.

#include "machine_x16.h"
#include "x16_i2c.h"
#include "x16_hostfs.h"
#include "libcore/main/intercept.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/io_handler.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/shared_irq_manager.h"
#include "libmem/main/ibus.h"
#include "libdebug/main/execution_observer.h"
#include "plugins/6502/main/cpu65c02.h"
#include "plugins/devices/via6522/main/via6522.h"
#include "plugins/devices/vera/main/vera.h"
#include "plugins/devices/ym2151/main/ym2151.h"
#include "util/path_util.h"
#include "util/logging.h"
#include <cctype>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <vector>

namespace x16 {
namespace {

constexpr unsigned ROM_BANKS = 32;
constexpr uint32_t ROM_BANK_SIZE = 0x4000, RAM_BANK_SIZE = 0x2000;

// ---------------------------------------------------------------------------
// The bus
// ---------------------------------------------------------------------------

class X16Bus : public IBus {
public:
    explicit X16Bus(std::vector<uint8_t> rom) : m_rom(std::move(rom)) {
        m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF};
        m_rom.resize(ROM_BANKS * ROM_BANK_SIZE, 0xFF);
        m_bram.assign(RAM_BANKS * RAM_BANK_SIZE, 0);
        std::memset(m_ram, 0, sizeof(m_ram));
    }
    void setDevices(IOHandler* via1, IOHandler* via2, IOHandler* vera, IOHandler* ym) {
        m_via1 = via1; m_via2 = via2; m_vera = vera; m_ym = ym;
    }

    // The bank latches: written with $00/$01 (which are also RAM: reads
    // return the RAM byte). An interrupt's vector fetch (the 65C02's VPB pin)
    // clears the ROM bank latch only, so the KERNAL's handler can read the
    // interrupted bank back from $01 (x16-emulator vp6502()).
    uint8_t ramBank() const { return m_ramBank; }
    uint8_t romBank() const { return m_romBank; }
    void resetBanks() { m_ramBank = m_romBank = 0; m_ram[0] = m_ram[1] = 0; }
    void vectorPull() { m_romBank = 0; }
    /** Wait states from the last access (slow I/O ranges), taken by the scheduler. */
    int takeWaitCycles() { int w = m_wait; m_wait = 0; return w; }

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
        uint8_t before = m_observer ? read(addr, true) : 0;
        if (addr < 0x9F00) {
            m_ram[addr] = val;
            if (addr == 0) m_ramBank = val;
            else if (addr == 1) m_romBank = val;
        } else if (addr < 0xA000) {
            if (addr >= 0x9FA0) m_wait += 3;
            if (addr < 0x9F10) m_via1->ioWrite(this, addr, val);
            else if (addr < 0x9F20) m_via2->ioWrite(this, addr, val);
            else if (addr < 0x9F40) m_vera->ioWrite(this, addr, val);
            else if (addr < 0x9F60) { m_wait += 3; m_ym->ioWrite(this, addr, val); }   // YM2151 (slow I/O)
        } else if (addr < 0xC000) {
            if (ramBank() < RAM_BANKS) m_bram[ramBank() * RAM_BANK_SIZE + (addr - 0xA000)] = val;
        }                                             // ROM: writes ignored
        if (m_observer) m_observer->onMemoryWrite(this, addr, before, val);
    }

    // Snapshots: RAM and banked RAM (the bank registers are RAM $00/$01)
    size_t stateSize() const override { return sizeof(m_ram) + m_bram.size(); }
    void saveState(uint8_t* buf) const override {
        std::memcpy(buf, m_ram, sizeof(m_ram));
        std::memcpy(buf + sizeof(m_ram), m_bram.data(), m_bram.size());
    }
    void loadState(const uint8_t* buf) override {
        std::memcpy(m_ram, buf, sizeof(m_ram));
        std::memcpy(m_bram.data(), buf + sizeof(m_ram), m_bram.size());
        m_ramBank = m_ram[0];
        m_romBank = m_ram[1];
    }
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

private:
    uint8_t read(uint32_t addr, bool peek) {
        if (addr < 0x9F00) return m_ram[addr];
        if (addr < 0xA000) {
            if (!peek && addr >= 0x9FA0) m_wait += 3;
            uint8_t v = 0x9F;                         // open bus
            if (addr >= 0x9F40 && addr < 0x9F60 && !peek) m_wait += 3;   // YM2151: slow I/O
            IOHandler* h = addr < 0x9F10 ? m_via1 : addr < 0x9F20 ? m_via2 : addr < 0x9F40 ? m_vera
                         : addr < 0x9F60 ? m_ym : nullptr;
            if (h) {
                if (peek) h->ioPeek(this, addr, &v);
                else      h->ioRead(this, addr, &v);
                return v;
            }
            return 0x9F;
        }
        if (addr < 0xC000) {
            return ramBank() < RAM_BANKS ? m_bram[ramBank() * RAM_BANK_SIZE + (addr - 0xA000)]
                                         : (uint8_t)(addr >> 8);
        }
        return romBank() < ROM_BANKS ? m_rom[romBank() * ROM_BANK_SIZE + (addr - 0xC000)]
                                     : (uint8_t)(addr >> 8);
    }

    BusConfig m_cfg;
    uint8_t m_ram[0x10000];
    std::vector<uint8_t> m_bram, m_rom;
    IOHandler* m_via1 = nullptr;
    IOHandler* m_via2 = nullptr;
    IOHandler* m_vera = nullptr;
    IOHandler* m_ym = nullptr;
    uint8_t m_ramBank = 0, m_romBank = 0;
    int m_wait = 0;
};

// ---------------------------------------------------------------------------
// Keyboard: key names to IBM key numbers (x16-emulator keyboard.c), sent
// through the SMC's buffer; typed text waits for the buffer to drain.
// ---------------------------------------------------------------------------

const std::map<std::string, uint8_t>& keyNumbers() {
    static const std::map<std::string, uint8_t> m = [] {
        std::map<std::string, uint8_t> k = {
            {"grave", 1}, {"minus", 12}, {"equals", 13}, {"kpeq", 13}, {"delete", 15}, {"backspace", 15},
            {"tab", 16}, {"bracket_left", 27}, {"bracket_right", 28}, {"backslash", 29},
            {"caps", 30}, {"caps_lock", 30}, {"semicolon", 40}, {"squote", 41}, {"apostrophe", 41},
            {"return", 43}, {"enter", 43}, {"left_shift", 44}, {"lshift", 44}, {"comma", 53}, {"period", 54},
            {"slash", 55}, {"right_shift", 57}, {"rshift", 57}, {"control", 58}, {"ctrl", 58}, {"alt", 60},
            {"space", 61}, {"insert", 75}, {"del", 76}, {"left", 79}, {"home", 80}, {"end", 81},
            {"up", 83}, {"down", 84}, {"pageup", 85}, {"pagedown", 86}, {"right", 89},
            {"escape", 110}, {"esc", 110}, {"run_stop", 110}, {"break", 126}, {"pause", 126},
        };
        const char* row1 = "qwertyuiop";             // 17-26
        const char* row2 = "asdfghjkl";              // 31-39
        const char* row3 = "zxcvbnm";                // 46-52
        for (int i = 0; row1[i]; i++) k[std::string(1, row1[i])] = (uint8_t)(17 + i);
        for (int i = 0; row2[i]; i++) k[std::string(1, row2[i])] = (uint8_t)(31 + i);
        for (int i = 0; row3[i]; i++) k[std::string(1, row3[i])] = (uint8_t)(46 + i);
        for (int d = 1; d <= 9; d++) k[std::string(1, (char)('0' + d))] = (uint8_t)(1 + d);
        k["0"] = 11;
        for (int f = 1; f <= 12; f++) k["f" + std::to_string(f)] = (uint8_t)(111 + f);
        return k;
    }();
    return m;
}

class X16Keyboard : public IOHandler, public IKeyboardMatrix {
public:
    explicit X16Keyboard(Smc& smc) : m_smc(smc) {}
    const char* name() const override { return "Keyboard"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override { m_queue.clear(); }

    // One queued event whenever the SMC's buffer is empty: the KERNAL reads
    // the buffer from its 60 Hz interrupt
    void tick(uint64_t) override {
        if (!m_queue.empty() && m_smc.keyBufferEmpty()) {
            m_smc.keyAdd(m_queue.front());
            m_queue.pop_front();
        }
    }

    bool key(std::string name, bool down) {
        for (auto& c : name) c = (char)std::tolower((unsigned char)c);
        auto it = keyNumbers().find(name);
        if (it == keyNumbers().end()) return false;
        m_smc.keyAdd(down ? it->second : (uint8_t)(it->second | 0x80));
        return true;
    }

    // IKeyboardMatrix (no matrix: names and typed text only)
    void keyDown(int, int) override {}
    void keyUp(int, int) override {}
    void clearKeys() override { m_queue.clear(); }
    bool pressKeyByName(const std::string& n, bool down) override { return key(n, down); }
    IPortDevice* getPort(int) override { return nullptr; }

    // Text as US-layout key presses; uppercase letters are unshifted (the
    // X16 starts in the PETSCII upper case / graphics set), lowercase shifted
    void enqueueText(const std::string& text) override {
        static const std::map<char, std::pair<const char*, bool>> sym = {
            {' ', {"space", false}}, {'!', {"1", true}}, {'"', {"squote", true}}, {'#', {"3", true}},
            {'$', {"4", true}}, {'%', {"5", true}}, {'&', {"7", true}}, {'\'', {"squote", false}},
            {'(', {"9", true}}, {')', {"0", true}}, {'*', {"8", true}}, {'+', {"equals", true}},
            {',', {"comma", false}}, {'-', {"minus", false}}, {'.', {"period", false}}, {'/', {"slash", false}},
            {':', {"semicolon", true}}, {';', {"semicolon", false}}, {'<', {"comma", true}}, {'=', {"equals", false}},
            {'>', {"period", true}}, {'?', {"slash", true}}, {'@', {"2", true}}, {'[', {"bracket_left", false}},
            {']', {"bracket_right", false}}, {'^', {"6", true}}, {'_', {"minus", true}},
        };
        for (size_t i = 0; i < text.size(); i++) {
            char c = text[i];
            if (c == '\\' && i + 1 < text.size()) {
                char e = text[++i];
                if (e == 'n' || e == 'r') { queueKey("return", false); continue; }
                if (e == 't') { queueKey("space", false); continue; }
                c = e;
            }
            if (c == '\n' || c == '\r') queueKey("return", false);
            else if (c >= 'A' && c <= 'Z') queueKey(std::string(1, (char)(c - 'A' + 'a')), false);
            else if (c >= 'a' && c <= 'z') queueKey(std::string(1, c), true);
            else if (c >= '0' && c <= '9') queueKey(std::string(1, c), false);
            else if (auto it = sym.find(c); it != sym.end()) queueKey(it->second.first, it->second.second);
        }
    }

private:
    void queueKey(const std::string& name, bool shift) {
        auto it = keyNumbers().find(name);
        if (it == keyNumbers().end()) return;
        if (shift) m_queue.push_back(44);
        m_queue.push_back(it->second);
        m_queue.push_back((uint8_t)(it->second | 0x80));
        if (shift) m_queue.push_back(44 | 0x80);
    }

    Smc& m_smc;
    std::deque<uint8_t> m_queue;
};

// The I2C devices, owned by the machine; clocked for the RTC
class I2cDevices : public IOHandler {
public:
    I2cDevices() : bus(smc, rtc) {}
    const char* name() const override { return "I2C (SMC, RTC)"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override { bus.reset(); smc.reset(); m_frameAcc = 0; }
    void tick(uint64_t cycles) override {
        rtc.step(cycles, CPU_HZ);
        // The mouse state goes to the SMC once per frame, as x16-emulator does
        m_frameAcc += cycles;
        if (m_frameAcc >= CPU_HZ / 60) {
            m_frameAcc -= CPU_HZ / 60;
            smc.sendMouseState();
        }
    }

    Smc smc;
    Rtc rtc;
    I2cBus bus;

private:
    uint64_t m_frameAcc = 0;
};

// VIA1 port B: the IEC serial bus (PB3-5 out, PB6/PB7 CLK/DATA in). Nothing
// is attached: every line reads released (high).
class IecPortB : public IPortDevice {
public:
    uint8_t readPort() override { return 0xFF; }
    void writePort(uint8_t) override {}
    void setDdr(uint8_t) override {}
};

bool loadRom(std::vector<uint8_t>& out, std::string* err) {
    std::string path = PathUtil::findResource("roms/x16/rom.bin");
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "Commander X16 ROM not found: roms/x16/rom.bin (the rom.bin of an x16-rom release, e.g. R49)";
        return false;
    }
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (out.empty() || out.size() > ROM_BANKS * ROM_BANK_SIZE || out.size() % ROM_BANK_SIZE) {
        if (err) *err = "roms/x16/rom.bin: expected a multiple of 16 KB up to 512 KB, got " + std::to_string(out.size()) + " bytes";
        return false;
    }
    return true;
}

} // namespace

MachineDescriptor* create(std::string* err) {
    std::vector<uint8_t> rom;
    if (!loadRom(rom, err)) return nullptr;

    auto* desc = new MachineDescriptor();
    desc->machineId = "x16";
    desc->displayName = "Commander X16";
    desc->licenseClass = "open";

    auto* bus = new X16Bus(std::move(rom));
    desc->buses.push_back({"system", bus});
    auto* cpu = new MOS65C02();
    cpu->setDataBus(bus);
    cpu->setCodeBus(bus);
    cpu->setVectorPullHook([bus]() { bus->vectorPull(); });
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});

    auto* io = new IORegistry();
    desc->ioRegistry = io;

    auto* vera = new Vera();
    vera->setCpuClockHz(CPU_HZ);
    auto* via1 = new VIA6522("VIA1", 0x9F00);
    auto* via2 = new VIA6522("VIA2", 0x9F10);
    auto* i2c = new I2cDevices();
    auto* portA = new Via1PortA(i2c->bus);
    desc->deleters.push_back([portA]() { delete portA; });
    via1->setPortADevice(portA);
    auto* portB = new IecPortB();
    desc->deleters.push_back([portB]() { delete portB; });
    via1->setPortBDevice(portB);
    auto* keyboard = new X16Keyboard(i2c->smc);
    auto* ym = new Ym2151(0x9F40, 0x1F);
    ym->setClockHz(CPU_HZ);
    bus->setDevices(via1, via2, vera, ym);
    // VERA mixes the YM2151 into its output: (psg + pcm) * 2 + ym
    vera->setExternalAudio([ym](int32_t& l, int32_t& r) {
        int32_t yl, yr;
        ym->nextSample(Vera::AUDIO_RATE, yl, yr);
        l += yl;
        r += yr;
    });

    // IRQ: VIA1, VIA2, VERA and the YM2151 share the line
    auto* irq = new SharedIrqManager(cpu);
    desc->deleters.push_back([irq]() { delete irq; });
    via1->setIrqLine(irq->createLine());
    via2->setIrqLine(irq->createLine());
    vera->setIrqLine(irq->createLine());
    ym->setIrqLine(irq->createLine());

    MachineDescriptor* self = desc;
    i2c->smc.onReset = [self]() { if (self->onReset) self->onReset(*self); };
    i2c->smc.onNmi = [cpu]() { cpu->triggerNmi(); };

    // VERA first: the GUI and recordaudio take the first video and audio device
    io->registerOwnedHandler(vera);
    io->registerOwnedHandler(via1);
    io->registerOwnedHandler(via2);
    io->registerOwnedHandler(ym);
    io->registerOwnedHandler(i2c);
    io->registerOwnedHandler(keyboard);

    desc->onKey = [keyboard](const std::string& name, bool down) { return keyboard->key(name, down); };

    // SNES pads in the four controller ports (KERNAL joystick 1-4); a port
    // reads as connected from its first use
    desc->onGamepad = [portA](int port, uint32_t b) {
        if (port < 0 || port >= Via1PortA::NUM_PADS) return false;
        using M = MachineDescriptor;
        uint16_t p = 0;
        if (b & M::PAD_UP) p |= Via1PortA::SNES_UP;
        if (b & M::PAD_DOWN) p |= Via1PortA::SNES_DOWN;
        if (b & M::PAD_LEFT) p |= Via1PortA::SNES_LEFT;
        if (b & M::PAD_RIGHT) p |= Via1PortA::SNES_RIGHT;
        if (b & M::PAD_A) p |= Via1PortA::SNES_A;
        if (b & M::PAD_B) p |= Via1PortA::SNES_B;
        if (b & M::PAD_X) p |= Via1PortA::SNES_X;
        if (b & M::PAD_Y) p |= Via1PortA::SNES_Y;
        if (b & M::PAD_L) p |= Via1PortA::SNES_L;
        if (b & M::PAD_R) p |= Via1PortA::SNES_R;
        if (b & M::PAD_SELECT) p |= Via1PortA::SNES_SELECT;
        if (b & M::PAD_START) p |= Via1PortA::SNES_START;
        portA->setPadButtons(port, p);
        return true;
    };
    // A CBM-style joystick (active low: up, down, left, right, fire) as a pad; fire is B
    desc->onJoystick = [self](int port, uint8_t bits) {
        using M = MachineDescriptor;
        uint32_t b = 0;
        if (!(bits & 0x01)) b |= M::PAD_UP;
        if (!(bits & 0x02)) b |= M::PAD_DOWN;
        if (!(bits & 0x04)) b |= M::PAD_LEFT;
        if (!(bits & 0x08)) b |= M::PAD_RIGHT;
        if (!(bits & 0x10)) b |= M::PAD_B;
        self->onGamepad(port, b);
    };
    desc->onMouse = [i2c](int dx, int dy, uint8_t buttons, int wheel) {
        i2c->smc.mouseMove(dx, dy);
        i2c->smc.mouseButtons(buttons);
        if (wheel) i2c->smc.mouseWheel(wheel);
    };

    desc->onReset = [bus, io, cpu, i2c, portA](MachineDescriptor&) {
        io->resetAll();
        portA->reset();
        i2c->rtc.reset(true);
        bus->resetBanks();
        cpu->reset();
    };

    desc->schedulerStep = [bus, io, cpu](MachineDescriptor&) -> int {
        int c = cpu->step();
        c += bus->takeWaitCycles();
        io->tickAll((uint64_t)c);
        return c;
    };

    // An SD card image found at start-up (else `disk mount 8 <file.img>`)
    {
        std::string sd = PathUtil::findResource("roms/x16/sdcard.img");
        if (std::ifstream(sd).good()) vera->mountDisk(8, sd);
    }

    // HostFS: the current directory as IEEE unit 8 (x16-emulator's default).
    // It steps aside while an SD card is attached and it uses unit 8.
    registerX16HostFsIntercept();
    installIntercepts(*desc, nlohmann::json::parse(
        R"({"intercepts":[{"type":"x16.hostfs","unit":8,"path":"."}]})"));
    if (auto* hostfs = dynamic_cast<X16HostFs*>(findIntercept(*desc, "x16.hostfs"))) {
        hostfs->setSdCardProbe([vera]() { return vera->sdCard().attached(); });
        auto reset = desc->onReset;
        desc->onReset = [reset, hostfs](MachineDescriptor& m) {
            hostfs->reset();
            reset(m);
        };
    }

    desc->clockHz = CPU_HZ;
    desc->onReset(*desc);
    return desc;
}

} // namespace x16
