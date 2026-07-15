#include "audio_dma.h"
#include "mmemu_plugin_api.h"

static IOHandler* createAudioDma() {
    return new AudioDmaDevice(0xD710);
}

static DevicePluginInfo s_devices[] = {
    {"audio_dma", createAudioDma}
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "audio_dma_plugin",
    "Audio DMA Controller",
    "1.0.0",
    nullptr, nullptr,              // deps, supportedMachineIds
    0, nullptr,                     // coreCount, cores
    0, nullptr,                     // toolchainCount, toolchains
    1, s_devices,                   // deviceCount, devices
    0, nullptr,                     // machineCount, machines
    0, nullptr,                     // loaderCount, loaders
    0, nullptr                      // cartridgeCount, cartridges
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    (void)host;
    return &s_manifest;
}
