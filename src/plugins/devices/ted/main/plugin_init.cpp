#include "mmemu_plugin_api.h"
#include "ted.h"
#include "libdevices/main/device_registry.h"

// Device type "ted": the MOS 7360/8360 TED of the Plus/4 and C16 (PAL).
// The machine factory wires FetchFn, KeyboardFn and RomSelectFn.

static IOHandler* createTed() {
    return new Ted(Ted::Standard::PAL);
}

static DevicePluginInfo s_devices[] = {
    {"ted", createTed}
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "ted",
    nullptr,        // displayName
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

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    return &s_manifest;
}
