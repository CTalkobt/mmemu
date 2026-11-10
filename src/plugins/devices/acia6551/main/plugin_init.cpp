#include "mmemu_plugin_api.h"
#include "acia6551.h"

// The C64/C128 RS-232 cartridges built on the 6551 (VICE c64acia1.c): at
// $DE00 by default, rates doubled by their 3.6864 MHz crystal
static IOHandler* createSwiftLink() {
    auto* a = new Acia6551(0xDE00);
    a->setName("SwiftLink");
    a->setMode(Acia6551::Mode::SwiftLink);
    return a;
}
static IOHandler* createTurbo232() {
    auto* a = new Acia6551(0xDE00);
    a->setName("Turbo232");
    a->setMode(Acia6551::Mode::Turbo232);
    return a;
}

static DevicePluginInfo s_devices[] = {
    {"swiftlink", createSwiftLink},
    {"turbo232", createTurbo232},
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "acia6551",
    "6551 ACIA RS-232 cartridges (SwiftLink, Turbo232)",
    "1.0.0",
    nullptr,        // deps
    nullptr,        // supportedMachineIds
    0, nullptr,     // cores
    0, nullptr,     // toolchains
    2, s_devices,   // devices
    0, nullptr,     // machines
    0, nullptr,     // loaders
    0, nullptr      // cartridges
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    (void)host;
    return &s_manifest;
}
