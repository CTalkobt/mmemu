#include "mmemu_plugin_api.h"
#include "cpu_arm2.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/memory_bus.h"
#include "libtoolchain/main/idisasm.h"

namespace {
/** The ARM disassembler for the toolchain registry (Acorn syntax). */
class DisassemblerArm : public IDisassembler {
public:
    const char* isaName() const override { return "arm"; }
    int disasmOne(IBus* bus, uint32_t addr, char* buf, int bufsz) override { return m_cpu.disassembleOne(bus, addr, buf, bufsz); }
    int disasmEntry(IBus* bus, uint32_t addr, DisasmEntry& entry) override { return m_cpu.disassembleEntry(bus, addr, &entry); }
    void setSymbolTable(SymbolTable*) override {}
private:
    Arm2 m_cpu;
};
}  // namespace

static ICore* createCoreArm2() { return new Arm2(false); }
static ICore* createCoreArm1() { return new Arm2(true); }
static IDisassembler* createDisasmArm() { return new DisassemblerArm(); }

/** An ARM2 with 4 MB of RAM. */
static MachineDescriptor* createMachineRawArm() {
    auto* desc = new MachineDescriptor();
    desc->machineId = "rawarm";
    desc->displayName = "Raw ARM2 System";
    auto* bus = new FlatMemoryBus("system", 22);
    auto* cpu = new Arm2(false);
    cpu->setDataBus(bus);
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});
    desc->buses.push_back({"system", bus});
    desc->clockHz = 8000000;
    return desc;
}

static CorePluginInfo s_cores[] = { {"arm2", "ARM2", "open", createCoreArm2}, {"arm1", "ARM1", "open", createCoreArm1} };
static ToolchainPluginInfo s_toolchains[] = { {"arm", nullptr, createDisasmArm, nullptr} };
static MachinePluginInfo s_machines[] = { {"rawarm", createMachineRawArm, "Raw ARM2 with 4 MB of RAM"} };

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "arm",
    "Acorn ARM1/ARM2",
    "1.0.0",
    nullptr,
    nullptr,
    2, s_cores,
    1, s_toolchains,
    0, nullptr,     // devices
    1, s_machines,
    0, nullptr,     // loaders
    0, nullptr      // cartridges
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    return &s_manifest;
}
