#include "mmemu_plugin_api.h"
#include "drive1541.h"

// The true-drive 1541 (#341): device type "drive1541"
static IOHandler* createDrive1541() { return new Drive1541(8); }

static DevicePluginInfo s_devices[] = {
    {"drive1541", createDrive1541}
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "drive1541",
    "Commodore 1541 (true drive)",
    "1.0.0",
    nullptr,        // deps
    nullptr,        // supportedMachineIds
    0, nullptr,
    0, nullptr,
    1, s_devices,
    0, nullptr,
    0, nullptr,
    0, nullptr
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI*) {
    return &s_manifest;
}
