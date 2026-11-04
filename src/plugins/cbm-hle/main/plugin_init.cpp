#include "mmemu_plugin_api.h"
#include "kernal_hle.h"

// The cbm-hle plugin provides the "cbm.kernal.fileio" intercept type. Machines
// install it from their configuration (machines/*.json, on the KERNAL ROM);
// the CLI's `intercept` (and `hle`) commands control the installed instances.

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "cbm-hle",
    "CBM KERNAL HLE",
    "1.0.0",
    nullptr,        // deps
    nullptr,        // supportedMachineIds
    0, nullptr,     // cores
    0, nullptr,     // toolchains
    0, nullptr,     // devices
    0, nullptr,     // machines
    0, nullptr,     // loaders
    0, nullptr      // cartridges
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    registerKernalHleIntercept();
    return &s_manifest;
}
