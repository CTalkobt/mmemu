#include "mmemu_plugin_api.h"
#include "cpu_z80.h"
#include "disasm_z80.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/memory_bus.h"

static ICore* createCoreZ80() { return new Z80(); }
static IDisassembler* createDisasmZ80() { return new DisassemblerZ80(); }

/** A Z80 with 64 KB of RAM and an empty I/O space (reads $FF). */
static MachineDescriptor* createMachineRawZ80() {
    auto* desc = new MachineDescriptor();
    desc->machineId = "rawz80";
    desc->displayName = "Raw Z80 System";
    auto* bus = new FlatMemoryBus("system", 16);
    auto* cpu = new Z80();
    cpu->setDataBus(bus);
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});
    desc->buses.push_back({"system", bus});
    desc->clockHz = 4000000;
    return desc;
}

static CorePluginInfo s_cores[] = { {"z80", "NMOS", "open", createCoreZ80} };
static ToolchainPluginInfo s_toolchains[] = { {"z80", nullptr, createDisasmZ80, nullptr} };
static MachinePluginInfo s_machines[] = { {"rawz80", createMachineRawZ80} };

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "z80",
    "Zilog Z80",
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
