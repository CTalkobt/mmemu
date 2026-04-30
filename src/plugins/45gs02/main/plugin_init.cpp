#include "mmemu_plugin_api.h"
#include "cpu45gs02.h"
#include "disassembler_45gs02.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/memory_bus.h"
#include "libdevices/main/io_handler.h"
#include "libdevices/main/io_registry.h"
#include "hyper_serial.h"

static const SimPluginHostAPI* g_host = nullptr;

static ICore* createCore45GS02() {
    return new MOS45GS02();
}

static IDisassembler* createDisassembler45GS02() {
    return new Disassembler45GS02();
}

static MachineDescriptor* createMachineRawMega65() {
    MachineDescriptor* desc = new MachineDescriptor();
    desc->machineId = "rawMega65";
    desc->displayName = "Raw MEGA65 (45GS02 / RAM only)";

    // 28-bit address bus for 256MB RAM
    FlatMemoryBus* bus = new FlatMemoryBus("system", 28);
    MOS45GS02* cpu = new MOS45GS02();
    cpu->setDataBus(bus);

    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 40}); // 40MHz
    desc->buses.push_back({"system", bus});

    // Add I/O devices if available
    if (g_host && g_host->createDevice) {
        if (!desc->ioRegistry) desc->ioRegistry = new IORegistry();
        IOHandler* haltDev = nullptr;

        IOHandler* hs = g_host->createDevice("hyper_serial");
        if (hs) {
            hs->setBaseAddr(0xD6C0);
            desc->ioRegistry->registerOwnedHandler(hs);
            haltDev = hs;
        }

        IOHandler* et = g_host->createDevice("exit_trap");
        if (et) {
            et->setBaseAddr(0xD6CF);
            desc->ioRegistry->registerOwnedHandler(et);
            if (!haltDev) haltDev = et;
        }

        IOHandler* math = g_host->createDevice("mega65_math");
        if (math) {
            math->setBaseAddr(0xD700);
            desc->ioRegistry->registerOwnedHandler(math);
        }

        bus->setIoHooks(
            [io = desc->ioRegistry](IBus* b, uint32_t a, uint8_t* v) { return io->dispatchRead(b, a, v); },
            [io = desc->ioRegistry](IBus* b, uint32_t a, uint8_t v) { return io->dispatchWrite(b, a, v); }
        );

        bus->setHaltCheck([io = desc->ioRegistry] {
            if (!io) return false;
            std::vector<IOHandler*> handlers;
            io->enumerate(handlers);
            for (auto* h : handlers)
                if (h->isHaltRequested()) return true;
            return false;
        });
    }

    return desc;
}

static CorePluginInfo s_cores[] = {
    {"45GS02", "MEGA65", "open", createCore45GS02}
};

static ToolchainPluginInfo s_toolchains[] = {
    {"45GS02", createDisassembler45GS02, nullptr}
};

static MachinePluginInfo s_machines[] = {
    {"rawMega65", createMachineRawMega65}
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "45gs02",
    "MEGA65 45GS02 CPU Plugin",
    "0.1.0",
    nullptr, nullptr,
    1, s_cores,
    1, s_toolchains,
    0, nullptr,
    1, s_machines,
    0, nullptr,
    0, nullptr
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    g_host = host;
    return &s_manifest;
}
