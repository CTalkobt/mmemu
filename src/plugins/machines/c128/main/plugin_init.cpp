#include "mmemu_plugin_api.h"
#include "machine_c128.h"
#include "libcore/main/machines/machine_registry.h"
#include "util/logging.h"

static const char* s_deps[] = { "6502", nullptr };

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "c128",
    "Commodore 128",
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

static void reg(const char* id, bool ntsc, const char* desc) {
    MachineRegistry::instance().registerMachine(id, [ntsc, id]() -> MachineDescriptor* {
        std::string err;
        MachineDescriptor* m = c128::create(ntsc, &err);
        if (!m) LogRegistry::instance().getLogger("c128")->error("{}: {}", id, err);
        return m;
    }, desc);
}

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    reg("c128", false, "Commodore 128 (PAL)");
    reg("c128-ntsc", true, "Commodore 128 (NTSC)");
    return &s_manifest;
}
