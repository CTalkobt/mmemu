#include "mmemu_plugin_api.h"
#include "mc6847.h"

// A library plugin: the machines that use this chip link against it rather
// than compiling their own copy (#395). It registers nothing itself.

extern "C" {

static struct SimPluginManifest manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "mc6847",
    "Motorola MC6847 video display generator",
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
