#include "mmemu_plugin_api.h"
#include "cpu6502.h"
#include "cpu6510.h"
#include "disassembler_6502.h"
#include "assembler_6502.h"
#include "kickassembler.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/memory_bus.h"

static const SimPluginHostAPI* g_host = nullptr;

static ICore* createCore6502() {
    return new MOS6502();
}

static ICore* createCore6510() {
    auto* core = new MOS6510();
    if (g_host && g_host->getLogger && g_host->logNamed) {
        core->setLogger(g_host->getLogger("6510"), g_host->logNamed);
    }
    return core;
}

static IDisassembler* createDisasm6502() {
    return new Disassembler6502();
}

static IAssembler* createAsm6502() {
    return new Assembler6502();
}

static IAssembler* createAsmKick() {
    return new KickAssemblerBackend();
}

static MachineDescriptor* createMachineRaw6502() {
    auto* desc = new MachineDescriptor();
    desc->machineId = "raw6502";
    desc->displayName = "Raw 6502 System (Plugin)";

    auto* bus = new FlatMemoryBus("system", 16);
    auto* cpu = new MOS6502();
    cpu->setDataBus(bus);

    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});
    desc->buses.push_back({"system", bus});

    return desc;
}

static MachineDescriptor* createMachineRaw6510() {
    auto* desc = new MachineDescriptor();
    desc->machineId = "raw6510";
    desc->displayName = "Raw 6510 System (Plugin)";

    auto* bus = new FlatMemoryBus("system", 16);
    auto* cpu = new MOS6510();
    cpu->setDataBus(bus);

    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});
    desc->buses.push_back({"system", bus});

    return desc;
}

static CorePluginInfo s_cores[] = {
    {"6502", "NMOS", "open", createCore6502},
    {"6510", "NMOS", "open", createCore6510}
};

static ToolchainPluginInfo s_toolchains[] = {
    {"6502", nullptr, createDisasm6502, createAsm6502},           // ISA default, no name
    {nullptr, "kickAssembler", nullptr, createAsmKick}            // Name-only registration
};

static MachinePluginInfo s_machines[] = {
    {"raw6502", createMachineRaw6502},
    {"raw6510", createMachineRaw6510}
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "6502",
    nullptr,        // displayName
    "1.0.0",
    nullptr,        // deps
    nullptr,        // supportedMachineIds
    2, s_cores,
    2, s_toolchains,
    0, nullptr,     // devices
    2, s_machines,  // machines
    0, nullptr,     // loaders
    0, nullptr      // cartridges
};

#include "libcore/main/core_registry.h"
#include "libtoolchain/main/toolchain_registry.h"

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    g_host = host;
    return &s_manifest;
}
