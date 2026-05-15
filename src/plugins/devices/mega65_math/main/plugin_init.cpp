#include "mmemu_plugin_api.h"
#include "mega65_math.h"

static IOHandler* createMega65Math() {
    return new Mega65MathDevice();
}

static DevicePluginInfo s_devices[] = {
    {"mega65_math", createMega65Math}
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "mega65_math",
    "MEGA65 Math Accelerator",
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
