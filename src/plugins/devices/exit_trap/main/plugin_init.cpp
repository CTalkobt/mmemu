#include "mmemu_plugin_api.h"
#include "exit_trap.h"

static IOHandler* createExitTrap() {
    return new ExitTrapDevice();
}

static DevicePluginInfo s_devices[] = {
    {"exit_trap", createExitTrap}
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "exit_trap",
    "45gs02 exit trap",
    "1.0.0",
    nullptr, nullptr,
    0, nullptr,
    0, nullptr,
    1, s_devices,
    0, nullptr,
    0, nullptr,
    0, nullptr
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    (void)host;
    return &s_manifest;
}
