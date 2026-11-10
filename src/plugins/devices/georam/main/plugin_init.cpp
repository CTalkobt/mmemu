#include "mmemu_plugin_api.h"
#include "georam.h"

static IOHandler* createGeoRam() {
    return new GeoRam();
}

static DevicePluginInfo s_devices[] = {
    {"georam", createGeoRam}
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "georam",
    "GeoRAM / NeoRAM (512 KB-4 MB)",
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
