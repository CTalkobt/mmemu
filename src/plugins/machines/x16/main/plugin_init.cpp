#include "mmemu_plugin_api.h"
#include "machine_x16.h"
#include "x16_hostfs.h"
#include "libcore/main/machines/machine_registry.h"
#include "util/logging.h"

static const char* s_deps[] = { "6502", nullptr };

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "x16",
    "Commander X16",
    "1.0.0",
    s_deps,
    nullptr,
    0, nullptr,     // cores
    0, nullptr,     // toolchains
    0, nullptr,     // devices
    0, nullptr,     // machines (registered below)
    0, nullptr,     // loaders
    0, nullptr      // cartridges
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    registerX16HostFsIntercept();     // "x16.hostfs": a host folder as an IEEE unit
    MachineRegistry::instance().registerMachine("x16", []() -> MachineDescriptor* {
        std::string err;
        MachineDescriptor* m = x16::create(&err);
        if (!m) LogRegistry::instance().getLogger("x16")->error("{}", err);
        return m;
    }, "Commander X16");
    return &s_manifest;
}
