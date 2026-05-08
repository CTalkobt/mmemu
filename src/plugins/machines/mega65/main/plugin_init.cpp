#include "mmemu_plugin_api.h"
#include "libcore/main/machines/machine_registry.h"
#include "machine_mega65.h"

static const SimPluginHostAPI* g_host = nullptr;

static const char* s_mega65Deps[] = { "45gs02", "map_mmu", "f018b_dma", nullptr };

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "mega65",
    "MEGA65",
    "1.0.0",
    s_mega65Deps,
    nullptr,
    0, nullptr,
    0, nullptr,
    0, nullptr,
    0, nullptr,
    0, nullptr,
    0, nullptr
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    g_host = host;
    MachineRegistry::instance().registerMachine("mega65",
        []() -> MachineDescriptor* { return Mega65MachineFactory::create(); },
        "MEGA65");
    return &s_manifest;
}
