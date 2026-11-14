#include "mmemu_plugin_api.h"
#include "cpu8085.h"
#include "asm8085.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/memory_bus.h"
#include "libtoolchain/main/idisasm.h"

namespace {

/** The 8085 disassembler for the toolchain registry (Intel mnemonics). */
class Disassembler8085 : public IDisassembler {
public:
    const char* isaName() const override { return "8085"; }
    int disasmOne(IBus* bus, uint32_t addr, char* buf, int bufsz) override { return m_cpu.disassembleOne(bus, addr, buf, bufsz); }
    int disasmEntry(IBus* bus, uint32_t addr, DisasmEntry& entry) override { return m_cpu.disassembleEntry(bus, addr, &entry); }
    void setSymbolTable(SymbolTable*) override {}
private:
    I8085 m_cpu;
};

ICore* createCore8085() { return new I8085(); }
IDisassembler* createDisasm8085() { return new Disassembler8085(); }
IAssembler* createAsm8085() { return new Assembler8085(); }

/** An 8085 with 64 KB of RAM and an empty I/O space (reads $FF). */
MachineDescriptor* createMachineRaw8085() {
    auto* desc = new MachineDescriptor();
    desc->machineId = "raw8085";
    desc->displayName = "Raw 8085 System";
    auto* bus = new FlatMemoryBus("system", 16);
    auto* cpu = new I8085();
    cpu->setDataBus(bus);
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});
    desc->buses.push_back({"system", bus});
    desc->clockHz = 3072000;            // a 6.144 MHz crystal (8085A, 3 MHz grade)
    return desc;
}

CorePluginInfo s_cores[] = { {"8085", "NMOS", "open", createCore8085} };
ToolchainPluginInfo s_toolchains[] = { {"8085", nullptr, createDisasm8085, createAsm8085} };
MachinePluginInfo s_machines[] = { {"raw8085", createMachineRaw8085, "Raw Intel 8085 with 64 KB of RAM"} };

}  // namespace

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "8085",
    "Intel 8085 / Oki MSM80C85",
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
