#include "mmemu_plugin_api.h"
#include "reu.h"

static IOHandler* createReu() {
    return new Reu();
}

static DevicePluginInfo s_devices[] = {
    {"reu", createReu}
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "reu",
    "Commodore REU (1700/1764/1750, up to 16 MB)",
    "1.0.0",
    nullptr,        // deps
    nullptr,        // supportedMachineIds
    0, nullptr,     // cores
    0, nullptr,     // toolchains
    1, s_devices,   // devices
    0, nullptr,     // machines
    0, nullptr,     // loaders
    0, nullptr      // cartridges
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    (void)host;
    return &s_manifest;
}
