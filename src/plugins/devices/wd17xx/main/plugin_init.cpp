#include "mmemu_plugin_api.h"
#include "wd17xx.h"

// The WD17xx floppy controllers as stand-alone devices (#330, #331); the
// machines that carry one (the CoCo's FD-500, the TRS-80 Expansion
// Interface) build their own and wire its INTRQ/DRQ.
template <Wd17xx::Variant V, uint32_t BASE>
static IOHandler* create() { return new Wd17xx(V, BASE); }

static DevicePluginInfo s_devices[] = {
    {"fd1771", create<Wd17xx::Variant::FD1771, 0x37EC>},     // TRS-80 Model I EI
    {"fd1791", create<Wd17xx::Variant::FD1791, 0xFF48>},
    {"fd1793", create<Wd17xx::Variant::FD1793, 0xFF48>},
    {"fd1795", create<Wd17xx::Variant::FD1795, 0xFF48>},
    {"fd1797", create<Wd17xx::Variant::FD1797, 0xFF48>},
    {"wd2791", create<Wd17xx::Variant::WD2791, 0xFF48>},
    {"wd2793", create<Wd17xx::Variant::WD2793, 0xFF48>},     // CoCo FD-500
    {"wd2795", create<Wd17xx::Variant::WD2795, 0xFF48>},
    {"wd2797", create<Wd17xx::Variant::WD2797, 0xFF48>},
    {"wd1773", create<Wd17xx::Variant::WD1773, 0xFF48>},
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "wd17xx",
    "Western Digital WD17xx/WD279x floppy disk controllers (JVC, VDK, DMK, JV1, JV3 images)",
    "1.0.0",
    nullptr,        // deps
    nullptr,        // supportedMachineIds
    0, nullptr,     // cores
    0, nullptr,     // toolchains
    (int)(sizeof(s_devices) / sizeof(s_devices[0])), s_devices,
    0, nullptr,     // machines
    0, nullptr,     // loaders
    0, nullptr      // cartridges
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    (void)host;
    return &s_manifest;
}
