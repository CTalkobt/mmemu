#include "mmemu_plugin_api.h"
#include "ieee488_bus.h"
#include "libdevices/main/device_registry.h"

static IOHandler* createIeee488Bus() { return new Ieee488Bus(); }

static DevicePluginInfo s_devices[] = {
    {"ieee488_bus", createIeee488Bus}
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "ieee488",
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
