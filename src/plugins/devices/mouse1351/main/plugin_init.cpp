#include "mmemu_plugin_api.h"
#include "mouse1351.h"

static IOHandler* createMouse1351() {
    return new Mouse1351();
}

static DevicePluginInfo s_devices[] = {
    {"mouse1351", createMouse1351}
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "mouse1351",
    "Commodore 1351 mouse (proportional mode)",
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
