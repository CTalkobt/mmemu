// NES / Famicom: the 2A03's memory map, OAM DMA, the controller ports and
// the CPU/PPU/APU timing, after Mesen2 (Core/NES/NesConsole.cpp, NesCpu.cpp,
// NesMemoryManager.cpp, Input/NesController.h), GPL-3.0, Sour and the Mesen
// contributors.

#include "machine_nes.h"
#include "nes_apu.h"
#include "nes_cart.h"
#include "nes_ppu.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdebug/main/execution_observer.h"
#include "libmem/main/ibus.h"
#include "nes_cpu.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <vector>

namespace nes {
namespace {

/** The standard controller: a 4021 shift register, A first. */
struct Controller {
    uint8_t buttons = 0;     // BTN_* held
    uint8_t shift = 0;
    uint8_t read(bool strobe) {
        if (strobe) return buttons & 1;
        const uint8_t b = shift & 1;
        shift = (uint8_t)((shift >> 1) | 0x80);    // after 8 reads: 1s
        return b;
    }
};

/**
 * The 2A03's bus, and the rest of the console as the CPU drives it: the CPU
 * (nes_cpu.cpp) makes every cycle's access through read()/write(), runs the
 * PPU by the master clock and the APU by the cycle, and samples /NMI and
 * /IRQ. The IBus side (read8/write8/peek8) is for the debugger and tools:
 * at once, with no clocking.
 */
class NesBus : public IBus, public CpuSystem {
public:
    NesBus(Ppu* ppu, Apu* apu, Cpu* cpu) : m_ppu(ppu), m_apu(apu), m_cpu(cpu) {
        m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF};
        m_ram.fill(0);
        m_apu->setDmcDma([cpu]() { cpu->startDmcDma(); }, [cpu]() { cpu->stopDmcDma(); },
                         [cpu]() { return cpu->cycleCount(); });
    }

    void setCart(std::unique_ptr<Cartridge> c, std::string label) {
        m_cart = std::move(c);
        m_label = std::move(label);
        m_ppu->setCartridge(m_cart.get());
    }
    Cartridge* cart() const { return m_cart.get(); }
    const std::string& label() const { return m_label; }
    Controller& pad(int i) { return m_pads[i & 1]; }

    /** Power on: RAM cleared (the cartridge's work RAM is kept), the PPU's clock restarted. */
    void powerOn() { m_ram.fill(0); m_dataBus = 0; m_strobe = false; m_ppuMaster = 0; }
    /** The RESET button: the PPU's master clock restarts with the CPU's. */
    void resetClock() { m_ppuMaster = 0; }

    // ------------------------------------------------------------------
    // CpuSystem
    // ------------------------------------------------------------------
    void runPpu(uint64_t mc) override {
        // At least a dot each time, as Mesen's NesPpu::Run
        do {
            m_ppu->clock(1);
            m_ppuMaster += 4;
        } while (m_ppuMaster + 4 <= mc);
    }
    void clockCycle() override { m_apu->clock(1); }
    uint8_t read(uint16_t a) override {
        const uint8_t v = busRead(a);
        if (m_observer) m_observer->onMemoryRead(this, a, v);
        return v;
    }
    void write(uint16_t a, uint8_t v) override {
        if (m_observer) m_observer->onMemoryWrite(this, a, peek8(a), v);
        m_dataBus = v;
        apply(a, v);
    }
    uint8_t openBus() const override { return m_dataBus; }
    bool nmiLine() const override { return m_ppu->nmi(); }
    bool irqLine() const override { return m_apu->irq() || (m_cart && m_cart->irq()); }
    uint16_t dmcAddress() const override { return m_apu->dmcAddress(); }
    void dmcBuffer(uint8_t v) override { m_apu->dmcSetBuffer(v); }

    // ------------------------------------------------------------------
    // IBus
    // ------------------------------------------------------------------
    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "system"; }

    uint8_t read8(uint32_t addr) override { return busRead((uint16_t)addr); }
    uint8_t peek8(uint32_t addr) override {
        const uint16_t a = (uint16_t)addr;
        if (a < 0x2000) return m_ram[a & 0x7FF];
        if (a < 0x4000) return m_ppu->peek(a & 7);
        if (a == 0x4015) return m_apu->peekStatus(m_dataBus);
        if (a < 0x4020) return m_dataBus;
        return m_cart ? m_cart->cpuPeek(a, m_dataBus) : m_dataBus;
    }
    void write8(uint32_t addr, uint8_t v) override {
        const uint16_t a = (uint16_t)addr;
        if (m_observer) m_observer->onMemoryWrite(this, a, peek8(a), v);
        apply(a, v);
    }

    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

private:
    uint8_t busRead(uint16_t a) {
        uint8_t v;
        if (a < 0x2000) v = m_ram[a & 0x7FF];
        else if (a < 0x4000) v = m_ppu->read(a & 7);
        else if (a == 0x4015) v = m_apu->readStatus(m_dataBus);     // bit 5 stays open bus, not latched
        else if (a == 0x4016 || a == 0x4017) v = (uint8_t)((m_dataBus & 0xE0) | m_pads[a & 1].read(m_strobe));
        else if (a < 0x4020) v = m_dataBus;
        else v = m_cart ? m_cart->cpuRead(a, m_dataBus) : m_dataBus;
        if (a != 0x4015) m_dataBus = v;
        return v;
    }
    void apply(uint16_t a, uint8_t v) {
        if (a < 0x2000) m_ram[a & 0x7FF] = v;
        else if (a < 0x4000) m_ppu->write(a & 7, v);
        else if (a == 0x4014) m_cpu->startOamDma(v);
        else if (a == 0x4016) {
            const bool strobe = v & 1;
            if (m_strobe && !strobe) for (auto& p : m_pads) p.shift = p.buttons;   // latched as strobe falls
            m_strobe = strobe;
            if (m_strobe) for (auto& p : m_pads) p.shift = p.buttons;
        } else if (a <= 0x4017) m_apu->write(a, v);
        else if (a >= 0x4020 && m_cart) m_cart->cpuWrite(a, v, m_cpu->cycleCount());
    }

    BusConfig m_cfg;
    Ppu* m_ppu;
    Apu* m_apu;
    Cpu* m_cpu;
    std::unique_ptr<Cartridge> m_cart;
    std::string m_label;
    std::array<uint8_t, 0x800> m_ram{};
    std::array<Controller, 2> m_pads{};
    bool m_strobe = false;
    uint64_t m_ppuMaster = 0;
    uint8_t m_dataBus = 0;
};

NesBus* busOf(MachineDescriptor* m) {
    if (!m) return nullptr;
    for (auto& b : m->buses)
        if (auto* p = dynamic_cast<NesBus*>(b.bus)) return p;
    return nullptr;
}

uint8_t padBits(uint32_t b) {
    using M = MachineDescriptor;
    uint8_t p = 0;
    if (b & (M::PAD_A | M::PAD_X)) p |= BTN_A;
    if (b & (M::PAD_B | M::PAD_Y)) p |= BTN_B;
    if (b & M::PAD_SELECT) p |= BTN_SELECT;
    if (b & M::PAD_START) p |= BTN_START;
    if (b & M::PAD_UP) p |= BTN_UP;
    if (b & M::PAD_DOWN) p |= BTN_DOWN;
    if (b & M::PAD_LEFT) p |= BTN_LEFT;
    if (b & M::PAD_RIGHT) p |= BTN_RIGHT;
    return p;
}

/** Power the console on: every chip and RAM to its power-on state (the RESET button is onReset). */
void powerOn(MachineDescriptor* m) {
    auto* bus = busOf(m);
    if (!bus) return;
    m->ioRegistry->resetAll();
    if (bus->cart()) bus->cart()->reset();
    bus->powerOn();
    static_cast<Cpu*>(m->cpus[0].cpu)->reset();
}

}  // namespace

bool insertCartridge(MachineDescriptor* m, const std::vector<uint8_t>& image, const std::string& label, std::string& err) {
    auto* bus = busOf(m);
    if (!bus) { err = "Not an NES."; return false; }
    auto cart = Cartridge::load(image, err);
    if (!cart) return false;
    const std::string desc = cart->describe();
    bus->setCart(std::move(cart), label + " (" + desc + ")");
    powerOn(m);
    return true;
}

void removeCartridge(MachineDescriptor* m) {
    auto* bus = busOf(m);
    if (!bus) return;
    bus->setCart(nullptr, "");
    powerOn(m);
}

std::string cartridgeInfo(MachineDescriptor* m) {
    auto* bus = busOf(m);
    return bus && bus->cart() ? bus->label() : "";
}

void setButtons(MachineDescriptor* m, int port, uint8_t buttons) {
    if (auto* bus = busOf(m)) bus->pad(port).buttons = buttons;
}

MachineDescriptor* create(bool famicom, std::string* /*err*/) {
    auto* desc = new MachineDescriptor();
    desc->machineId = famicom ? "famicom" : "nes";
    desc->displayName = famicom ? "Nintendo Famicom" : "Nintendo Entertainment System";
    desc->licenseClass = "proprietary";
    desc->uiPanesSet = true;
    desc->uiPanes = {"cartridge"};
    desc->clockHz = Apu::CPU_HZ;

    auto* ppu = new Ppu();
    auto* apu = new Apu();
    auto* cpu = new Cpu();
    auto* bus = new NesBus(ppu, apu, cpu);
    desc->buses.push_back({"system", bus});
    cpu->setSystem(bus);
    cpu->setFamicom(famicom);
    cpu->setDecimalDisabled(true);      // for the base core's disassembly and tools
    cpu->setDataBus(bus);
    cpu->setCodeBus(bus);
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});

    auto* io = new IORegistry();
    desc->ioRegistry = io;
    io->registerHandler(ppu);
    io->registerHandler(apu);
    desc->deleters.push_back([ppu, apu]() { delete ppu; delete apu; });

    // Controllers. The keyboard and a CBM-style joystick (fire = A) drive
    // pad 1; each source keeps its own bits so they combine.
    auto* held = new uint8_t[3]{0, 0, 0};        // keys, joystick 0, joystick 1
    desc->deleters.push_back([held]() { delete[] held; });
    auto* pads = new uint8_t[2]{0, 0};           // onGamepad
    desc->deleters.push_back([pads]() { delete[] pads; });
    auto apply = [bus, held, pads]() {
        bus->pad(0).buttons = (uint8_t)(held[0] | held[1] | pads[0]);
        bus->pad(1).buttons = (uint8_t)(held[2] | pads[1]);
    };
    desc->onGamepad = [pads, apply](int port, uint32_t b) {
        if (port < 0 || port > 1) return false;
        pads[port] = padBits(b);
        apply();
        return true;
    };
    desc->joystickPorts = 2;
    desc->primaryJoystickPort = 0;
    desc->onJoystick = [held, apply](int port, uint8_t bits) {
        if (port < 0 || port > 1) return;
        uint8_t p = 0;
        if (!(bits & 0x01)) p |= BTN_UP;
        if (!(bits & 0x02)) p |= BTN_DOWN;
        if (!(bits & 0x04)) p |= BTN_LEFT;
        if (!(bits & 0x08)) p |= BTN_RIGHT;
        if (!(bits & 0x10)) p |= BTN_A;
        held[1 + port] = p;
        apply();
    };
    desc->onKey = [held, apply](const std::string& name, bool down) {
        std::string n = name;
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        uint8_t bit = 0;
        if (n == "up") bit = BTN_UP;
        else if (n == "down") bit = BTN_DOWN;
        else if (n == "left") bit = BTN_LEFT;
        else if (n == "right") bit = BTN_RIGHT;
        else if (n == "x") bit = BTN_A;
        else if (n == "z") bit = BTN_B;
        else if (n == "return" || n == "start") bit = BTN_START;
        else if (n == "tab" || n == "left_shift" || n == "right_shift" || n == "select") bit = BTN_SELECT;
        else return false;
        held[0] = down ? (uint8_t)(held[0] | bit) : (uint8_t)(held[0] & ~bit);
        apply();
        return true;
    };
    desc->screenButtons = {
        {"START", {"start"}, "Controller 1's START button (Return)."},
        {"SELECT", {"select"}, "Controller 1's SELECT button (Tab)."}};

    // The RESET button: the PPU and APU take their reset paths, RAM and the
    // cartridge's registers stay (most mappers do not see reset), the CPU
    // keeps A/X/Y and drops S by 3
    desc->onReset = [bus, ppu, apu, cpu](MachineDescriptor&) {
        ppu->softReset();
        apu->softReset();
        bus->resetClock();
        cpu->resetButton();
    };
    desc->schedulerStep = [cpu](MachineDescriptor&) -> int { return cpu->step(); };
    powerOn(desc);
    return desc;
}

}  // namespace nes
