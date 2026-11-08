#include "mmemu_plugin_api.h"
#include "cpu65816.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/memory_bus.h"
#include "libtoolchain/main/idisasm.h"

namespace {
/** The 6809 disassembler for the toolchain registry (WDC mnemonics). */
class Disassembler65816 : public IDisassembler {
public:
    const char* isaName() const override { return "65816"; }
    int disasmOne(IBus* bus, uint32_t addr, char* buf, int bufsz) override { return m_cpu.disassembleOne(bus, addr, buf, bufsz); }
    int disasmEntry(IBus* bus, uint32_t addr, DisasmEntry& entry) override { return m_cpu.disassembleEntry(bus, addr, &entry); }
    void setSymbolTable(SymbolTable*) override {}
private:
    W65C816 m_cpu;
};
}  // namespace

static ICore* createCore65816() { return new W65C816(); }
static IDisassembler* createDisasm65816() { return new Disassembler65816(); }

/** A 65C816 with 16 MB of RAM. */
static MachineDescriptor* createMachineRaw65816() {
    auto* desc = new MachineDescriptor();
    desc->machineId = "raw65816";
    desc->displayName = "Raw 65816 System";
    auto* bus = new FlatMemoryBus("system", 24);
    auto* cpu = new W65C816();
    cpu->setDataBus(bus);
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});
    desc->buses.push_back({"system", bus});
    desc->clockHz = 1000000;
    return desc;
}

static CorePluginInfo s_cores[] = { {"65816", "CMOS", "open", createCore65816} };
static ToolchainPluginInfo s_toolchains[] = { {"65816", nullptr, createDisasm65816, nullptr} };
static MachinePluginInfo s_machines[] = { {"raw65816", createMachineRaw65816, "Raw 65C816 with 16 MB of RAM"} };

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "65816",
    "WDC 65C816",
    "1.0.0",
    nullptr,
    nullptr,
    1, s_cores,
    1, s_toolchains,
    0, nullptr,     // devices
    1, s_machines,
    0, nullptr,     // loaders
    0, nullptr      // cartridges
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    return &s_manifest;
}
