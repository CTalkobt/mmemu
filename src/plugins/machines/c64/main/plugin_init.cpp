#include "mmemu_plugin_api.h"
#include "libcore/main/json_machine_loader.h"
#include "libdevices/main/device_registry.h"
#include "kbd_c64.h"

static const SimPluginHostAPI* g_host = nullptr;

static const char* s_c64Deps[] = { "6502", "c64-pla", "cia6526", "vic2", "sid6581", "cbm-loader", "reu", "georam", nullptr };

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "c64",
    "Commodore 64",
    "1.0.0",
    s_c64Deps,
    nullptr,
    0, nullptr,
    0, nullptr,
    0, nullptr,
    0, nullptr,
    0, nullptr,
    0, nullptr
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    g_host = host;
    DeviceRegistry::instance().registerDevice("kbd_c64",
        []() -> IOHandler* { return new KbdC64(); });
    JsonMachineLoader loader;
    loader.loadFile("machines/c64.json");
    return &s_manifest;
}
