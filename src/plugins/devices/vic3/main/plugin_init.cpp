#include "mmemu_plugin_api.h"
#include "vic3.h"
#include "libdevices/main/device_registry.h"

static IOHandler* createVIC3() {
    return new VIC3();
}

static DevicePluginInfo s_devices[] = {
    {"4567", createVIC3}
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "vic3",
    nullptr,        // displayName
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
    for (int i = 0; i < s_manifest.deviceCount; ++i) {
        DeviceRegistry::instance().registerDevice(
            s_manifest.devices[i].name,
            s_manifest.devices[i].create
        );
    }
    return &s_manifest;
}
