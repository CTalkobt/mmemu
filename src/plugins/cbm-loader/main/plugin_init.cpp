#include "include/mmemu_plugin_api.h"
#include "libcore/main/image_loader.h"
#include "prg_loader.h"
#include "disk_loader.h"
#include "cbm_cart_handler.h"
#include <vector>

static const char* s_deps[] = { nullptr };
static const char* s_supportedMachines[] = { "vic20", "c64", "pet2001", "pet4032", "pet8032", nullptr };

static IImageLoader* createPrgLoader() {
    return new PrgLoader();
}

static IImageLoader* createDiskLoader() {
    return new DiskImageLoader();
}

// nullptr for a file that is not a C64 .crt (a Plus/4 one shares the extension)
static ICartridgeHandler* createCbmCartridge(const char* path) {
    auto* h = new CbmCartridgeHandler(path);
    if (h->valid()) return h;
    delete h;
    return nullptr;
}

static ImageLoaderPluginInfo s_loaders[] = {
    { createPrgLoader },
    { createDiskLoader }
};

static CartridgePluginInfo s_cartridges[] = {
    { "crt", createCbmCartridge }
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "cbm-loader",
    "Commodore Image Loader",
    "1.0.0",
    s_deps,
    s_supportedMachines,
    0, nullptr, // Cores
    0, nullptr, // Toolchains
    0, nullptr, // Devices
    0, nullptr, // Machines
    2, s_loaders,
    1, s_cartridges
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    (void)host;
    return &s_manifest;
}
