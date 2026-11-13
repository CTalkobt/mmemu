#include "mmemu_plugin_api.h"
#include "cpu6801.h"
#include "asm6801.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/memory_bus.h"
#include "libtoolchain/main/idisasm.h"

namespace {

/** The 6803 disassembler for the toolchain registry (Motorola mnemonics). */
class Disassembler6801 : public IDisassembler {
public:
    explicit Disassembler6801(const char* isa) : m_isa(isa) {}
    const char* isaName() const override { return m_isa; }
    int disasmOne(IBus* bus, uint32_t addr, char* buf, int bufsz) override { return m_cpu.disassembleOne(bus, addr, buf, bufsz); }
    int disasmEntry(IBus* bus, uint32_t addr, DisasmEntry& entry) override { return m_cpu.disassembleEntry(bus, addr, &entry); }
    void setSymbolTable(SymbolTable*) override {}
private:
    const char* m_isa;
    M6801 m_cpu;
};

/**
 * The raw machine's bus: 64 KB of RAM, with the CPU's own registers and
 * RAM ($00-$1F, $80-$FF) shown to the debugger where the chip decodes them.
 */
class Raw6803Bus : public FlatMemoryBus {
public:
    Raw6803Bus() : FlatMemoryBus("system", 16) {}
    void setCpu(M6801* cpu) { m_cpu = cpu; }
    uint8_t peek8(uint32_t addr) override {
        uint8_t v;
        if (m_cpu && m_cpu->peekInternal((uint16_t)addr, v)) return v;
        return FlatMemoryBus::peek8(addr);
    }
    // The debugger's writes (the CPU never sends internal addresses out)
    void write8(uint32_t addr, uint8_t v) override {
        if (m_cpu && m_cpu->isInternal((uint16_t)addr)) { m_cpu->writeInternal((uint16_t)addr, v); return; }
        FlatMemoryBus::write8(addr, v);
    }
private:
    M6801* m_cpu = nullptr;
};

}  // namespace

static ICore* createCore6803() { return new M6801(M6801::Variant::MC6803); }
static ICore* createCore6801() { return new M6801(M6801::Variant::MC6801); }
static IDisassembler* createDisasm6803() { return new Disassembler6801("6803"); }
static IDisassembler* createDisasm6801() { return new Disassembler6801("6801"); }
static IAssembler* createAsm6801() { return new Assembler6801(); }

/** An MC6803 (mode 2) with 64 KB of RAM. */
static MachineDescriptor* createMachineRaw6803() {
    auto* desc = new MachineDescriptor();
    desc->machineId = "raw6803";
    desc->displayName = "Raw 6803 System";
    auto* bus = new Raw6803Bus();
    auto* cpu = new M6801(M6801::Variant::MC6803);
    bus->setCpu(cpu);
    cpu->setDataBus(bus);
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});
    desc->buses.push_back({"system", bus});
    desc->clockHz = 1000000;
    return desc;
}

static CorePluginInfo s_cores[] = {
    {"6803", "NMOS", "open", createCore6803},
    {"6801", "NMOS", "open", createCore6801},
};
static ToolchainPluginInfo s_toolchains[] = {
    {"6803", nullptr, createDisasm6803, createAsm6801},
    {"6801", nullptr, createDisasm6801, createAsm6801},
};
static MachinePluginInfo s_machines[] = { {"raw6803", createMachineRaw6803, "Raw MC6803 with 64 KB of RAM"} };

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "6803",
    "Motorola MC6801 / MC6803",
    "1.0.0",
    nullptr,
    nullptr,
    2, s_cores,
    2, s_toolchains,
    0, nullptr,     // devices
    1, s_machines,
    0, nullptr,     // loaders
    0, nullptr      // cartridges
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    return &s_manifest;
}
