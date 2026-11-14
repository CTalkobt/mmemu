#include "mmemu_plugin_api.h"
#include "cpu6809.h"
#include "cpu6309.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/memory_bus.h"
#include "libtoolchain/main/idisasm.h"

namespace {
/** The 6809 disassembler for the toolchain registry (Motorola mnemonics). */
class Disassembler6809 : public IDisassembler {
public:
    const char* isaName() const override { return "6809"; }
    int disasmOne(IBus* bus, uint32_t addr, char* buf, int bufsz) override { return m_cpu.disassembleOne(bus, addr, buf, bufsz); }
    int disasmEntry(IBus* bus, uint32_t addr, DisasmEntry& entry) override { return m_cpu.disassembleEntry(bus, addr, &entry); }
    void setSymbolTable(SymbolTable*) override {}
private:
    M6809 m_cpu;
};

/** The HD6309 disassembler: the 6809's plus the 6309's opcodes and indexed modes. */
class Disassembler6309 : public IDisassembler {
public:
    const char* isaName() const override { return "6309"; }
    int disasmOne(IBus* bus, uint32_t addr, char* buf, int bufsz) override { return m_cpu.disassembleOne(bus, addr, buf, bufsz); }
    int disasmEntry(IBus* bus, uint32_t addr, DisasmEntry& entry) override { return m_cpu.disassembleEntry(bus, addr, &entry); }
    void setSymbolTable(SymbolTable*) override {}
private:
    HD6309 m_cpu;
};
}  // namespace

static ICore* createCore6809() { return new M6809(); }
static IDisassembler* createDisasm6809() { return new Disassembler6809(); }
static ICore* createCore6309() { return new HD6309(); }
static IDisassembler* createDisasm6309() { return new Disassembler6309(); }

/** A 6809 with 64 KB of RAM. */
static MachineDescriptor* createMachineRaw6809() {
    auto* desc = new MachineDescriptor();
    desc->machineId = "raw6809";
    desc->displayName = "Raw 6809 System";
    auto* bus = new FlatMemoryBus("system", 16);
    auto* cpu = new M6809();
    cpu->setDataBus(bus);
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});
    desc->buses.push_back({"system", bus});
    desc->clockHz = 1000000;
    return desc;
}

/** An HD6309 with 64 KB of RAM (starts in emulation mode, as after /RESET). */
static MachineDescriptor* createMachineRaw6309() {
    auto* desc = new MachineDescriptor();
    desc->machineId = "raw6309";
    desc->displayName = "Raw 6309 System";
    auto* bus = new FlatMemoryBus("system", 16);
    auto* cpu = new HD6309();
    cpu->setDataBus(bus);
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});
    desc->buses.push_back({"system", bus});
    desc->clockHz = 1000000;
    return desc;
}

static CorePluginInfo s_cores[] = {
    {"6809", "NMOS", "open", createCore6809},
    {"6309", "CMOS", "open", createCore6309},
};
static ToolchainPluginInfo s_toolchains[] = {
    {"6809", nullptr, createDisasm6809, nullptr},
    {"6309", nullptr, createDisasm6309, nullptr},
};
static MachinePluginInfo s_machines[] = {
    {"raw6809", createMachineRaw6809, "Raw 6809 with 64 KB of RAM"},
    {"raw6309", createMachineRaw6309, "Raw HD6309 with 64 KB of RAM"},
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "6809",
    "Motorola 6809 / Hitachi HD6309",
    "1.1.0",
    nullptr,
    nullptr,
    2, s_cores,
    2, s_toolchains,
    0, nullptr,     // devices
    2, s_machines,
    0, nullptr,     // loaders
    0, nullptr      // cartridges
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    return &s_manifest;
}
