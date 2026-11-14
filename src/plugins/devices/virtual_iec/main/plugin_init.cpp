#include "mmemu_plugin_api.h"
#include "virtual_iec.h"
#include "plugins/devices/iec_drive/main/iec_drive.h"

static const SimPluginHostAPI* g_host = nullptr;

static IOHandler* createVirtualIEC() {
    auto* iec = new VirtualIECBus(8);
    if (g_host && g_host->getLogger && g_host->logNamed) {
        iec->setLogger(g_host->getLogger("VirtualIEC"), g_host->logNamed);
    }
    return iec;
}

// A 1541 at the line level, as the C128's and Plus/4's (the C64's drive 8)
static IOHandler* createIecDrive() { return new IecDrive(8); }

static DevicePluginInfo s_devices[] = {
    {"virtual_iec", createVirtualIEC},
    {"iec_drive", createIecDrive}
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "virtual_iec",
    "Virtual IEC Device",
    "1.0.0",
    nullptr,        // deps
    nullptr,        // supportedMachineIds
    0, nullptr,
    0, nullptr,
    2, s_devices,
    0, nullptr,
    0, nullptr,
    0, nullptr
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    g_host = host;
    return &s_manifest;
}
