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
#include "plugins/6502/main/cpu6502.h"

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
 * The 2A03's bus. The PPU and APU are brought up to the cycle of each read;
 * writes to them and to the cartridge are held until the instruction ends
 * and applied on its last cycles (where the 6502 writes).
 */
class NesBus : public IBus {
public:
    NesBus(Ppu* ppu, Apu* apu) : m_ppu(ppu), m_apu(apu) {
        m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF};
        m_ram.fill(0);
        m_apu->setDmaRead([this](uint16_t a) { return busRead(a); });
    }

    void setCart(std::unique_ptr<Cartridge> c, std::string label) {
        m_cart = std::move(c);
        m_label = std::move(label);
        m_ppu->setCartridge(m_cart.get());
    }
    Cartridge* cart() const { return m_cart.get(); }
    const std::string& label() const { return m_label; }
    Controller& pad(int i) { return m_pads[i & 1]; }

    uint64_t cycles() const { return m_cycles; }
    void resetClock() {
        m_cycles = m_synced = 0;
        m_polledNmi = m_polledIrq = false;
        m_pending.clear();
        m_strobe = false;
        m_dmaPage = -1;
        // The 6502's reset sequence: 7 cycles before the first instruction
        syncTo(RESET_CYCLES);
        m_cycles = RESET_CYCLES;
    }
    static constexpr uint64_t RESET_CYCLES = 7;

    void begin() { m_access = 0; }
    /**
     * After an instruction of `c` cycles: the held writes, OAM DMA, DMC
     * stalls; returns the stall. The interrupt lines are sampled where the
     * 6502 polls them, before the instruction's last cycle (so an NMI or IRQ
     * raised in that cycle waits an instruction more): polledNmi()/polledIrq().
     */
    int end(int c) {
        const uint64_t base = m_cycles;
        const uint64_t poll = base + (uint64_t)(c > 0 ? c - 1 : 0);
        bool polled = false;
        auto sample = [&]() {
            syncTo(poll);
            m_polledNmi = m_ppu->nmi();
            m_polledIrq = irq();
            polled = true;
        };
        const size_t n = m_pending.size();
        for (size_t i = 0; i < n; ++i) {
            const uint64_t at = base + (uint64_t)c - (n - i);
            if (!polled && at >= poll) sample();
            syncTo(at);
            apply(m_pending[i].first, m_pending[i].second, at);
        }
        m_pending.clear();
        if (!polled) sample();
        uint64_t now = base + (uint64_t)c;
        int stall = 0;
        if (m_dmaPage >= 0) {
            // OAM DMA: a halt cycle, an alignment cycle on odd cycles, then
            // 256 read/write pairs (513 or 514 cycles)
            const int align = (now & 1) ? 2 : 1;
            for (int i = 0; i < 256; ++i) {
                const uint64_t at = now + (uint64_t)align + (uint64_t)(i * 2);
                syncTo(at);
                const uint8_t v = busRead((uint16_t)((m_dmaPage << 8) | i));
                syncTo(at + 1);
                m_ppu->oamDmaWrite(v);
            }
            stall += 512 + align;
            m_dmaPage = -1;
        }
        syncTo(now + (uint64_t)stall);
        stall += m_apu->takeStall();
        m_cycles = now + (uint64_t)stall;
        if (stall) {
            // The CPU was halted: it polls again on its way out
            syncTo(m_cycles - 1);
            m_polledNmi = m_polledNmi || m_ppu->nmi();
            m_polledIrq = irq();
        }
        syncTo(m_cycles);
        return stall;
    }

    bool polledNmi() const { return m_polledNmi; }
    bool polledIrq() const { return m_polledIrq; }
    /** Power on: RAM cleared (the cartridge's work RAM is battery-backed or not, but kept here). */
    void powerOn() { m_ram.fill(0); m_dataBus = 0; resetClock(); }
    bool nmi() const { return m_ppu->nmi(); }
    bool irq() const { return m_apu->irq() || (m_cart && m_cart->irq()); }

    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "system"; }

    uint8_t read8(uint32_t addr) override {
        const uint16_t a = (uint16_t)addr;
        const uint64_t at = m_cycles + m_access++;
        if (a >= 0x2000 && a < 0x4020) syncTo(at);
        const uint8_t v = busRead(a);
        if (m_observer) m_observer->onMemoryRead(this, a, v);
        return v;
    }
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
        ++m_access;
        if (m_observer) m_observer->onMemoryWrite(this, a, peek8(a), v);
        m_dataBus = v;
        if (a < 0x2000) { m_ram[a & 0x7FF] = v; return; }   // RAM: at once
        m_pending.push_back({a, v});
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
    void syncTo(uint64_t cycle) {
        if (cycle > m_synced) {
            const uint64_t n = cycle - m_synced;
            m_ppu->clock((uint32_t)(n * 3));
            m_apu->clock((uint32_t)n);
            m_synced = cycle;
        }
    }
    void apply(uint16_t a, uint8_t v, uint64_t at) {
        if (a < 0x4000) m_ppu->write(a & 7, v);
        else if (a == 0x4014) m_dmaPage = v;
        else if (a == 0x4016) {
            const bool strobe = v & 1;
            if (m_strobe && !strobe) for (auto& p : m_pads) p.shift = p.buttons;   // latched as strobe falls
            m_strobe = strobe;
            if (m_strobe) for (auto& p : m_pads) p.shift = p.buttons;
        } else if (a <= 0x4017) m_apu->write(a, v);
        else if (a >= 0x4020 && m_cart) m_cart->cpuWrite(a, v, at);
    }

    BusConfig m_cfg;
    Ppu* m_ppu;
    Apu* m_apu;
    std::unique_ptr<Cartridge> m_cart;
    std::string m_label;
    std::array<uint8_t, 0x800> m_ram{};
    std::array<Controller, 2> m_pads{};
    bool m_strobe = false;
    int m_dmaPage = -1;
    uint64_t m_cycles = 0, m_synced = 0;
    uint32_t m_access = 0;
    uint8_t m_dataBus = 0;
    bool m_polledNmi = false, m_polledIrq = false;
    std::vector<std::pair<uint16_t, uint8_t>> m_pending;
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
    auto* cpu = static_cast<MOS6502*>(m->cpus[0].cpu);
    cpu->reset();
    cpu->setNmiLine(false);
    cpu->setIrqLine(false);
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
    auto* bus = new NesBus(ppu, apu);
    desc->buses.push_back({"system", bus});
    auto* cpu = new MOS6502();
    cpu->setDecimalDisabled(true);
    cpu->setUnstableMagic(0xFF);
    cpu->setIrqPollLatency(true);
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
        cpu->softReset();
        cpu->setNmiLine(false);
        cpu->setIrqLine(false);
    };
    desc->schedulerStep = [bus, cpu](MachineDescriptor&) -> int {
        bus->begin();
        const int c = cpu->step();
        const int total = c + bus->end(c);
        cpu->setNmiLine(bus->polledNmi());
        cpu->setIrqLine(bus->polledIrq());
        return total;
    };
    powerOn(desc);
    return desc;
}

}  // namespace nes
