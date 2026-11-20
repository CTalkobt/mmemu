#include "mmemu_plugin_api.h"
#include "sn76489.h"

// A library plugin: the machines that use this chip link against it rather
// than compiling their own copy (#395). It registers nothing itself.

extern "C" {

static struct SimPluginManifest manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "sn76489",
    "TI SN76489 sound chip (BBC Micro, CoCo Game Master Cartridge)",
    "0.1.0",
    nullptr, // deps
    nullptr, // supportedMachineIds
    0, nullptr, // cores
    0, nullptr, // toolchains
    0, nullptr, // devices
    0, nullptr, // machines
    0, nullptr, // loaders
    0, nullptr  // cartridges
};

SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    return &manifest;
}

} // extern "C"
