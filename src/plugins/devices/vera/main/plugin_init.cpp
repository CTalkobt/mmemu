#include "mmemu_plugin_api.h"
#include "vera.h"

extern "C" {

static IOHandler* createVera() {
    return new Vera();
}

static struct DevicePluginInfo device_info[] = {
    { "VERA", createVera }
};

static struct SimPluginManifest manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "vera",
    "Commander X16 VERA video and audio",
    "0.1.0",
    nullptr, // deps
    nullptr, // supportedMachineIds
    0, nullptr, // cores
    0, nullptr, // toolchains
    1, device_info, // devices
    0, nullptr, // machines
    0, nullptr, // loaders
    0, nullptr  // cartridges
};

SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    return &manifest;
}

} // extern "C"
