#include "mmemu_plugin_api.h"
#include "f018b_dma.h"

static IOHandler* createF018bDma() {
    return new F018bDmaDevice();
}

static DevicePluginInfo s_devices[] = {
    {"f018b_dma", createF018bDma}
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "f018b_dma_plugin",
    "F018B DMA Controller",
    "1.0.0",
    nullptr, nullptr,           // deps, supportedMachineIds
    0, nullptr,                 // coreCount, cores
    0, nullptr,                 // toolchainCount, toolchains
    1, s_devices,               // deviceCount, devices
    0, nullptr,                 // machineCount, machines
    0, nullptr,                 // loaderCount, loaders
    0, nullptr                  // cartridgeCount, cartridges
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    (void)host;
    return &s_manifest;
}
