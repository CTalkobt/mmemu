#include "map_mmu.h"
#include "include/mmemu_plugin_api.h"

// Note: MapMmu is not registered as a device plugin here because it requires
// a SparseMemoryBus reference in its constructor, which is provided at machine setup time.
// The machine factory wires MapMmu directly as the CPU's IBus.

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    (void)host;

    static SimPluginManifest s_manifest = {
        MMEMU_PLUGIN_API_VERSION,
        "map-mmu",
        "MEGA65 Memory Address Mapper (MAP)",
        "1.0.0",
        nullptr, nullptr,
        0, nullptr,
        0, nullptr,
        0, nullptr,
        0, nullptr,
        0, nullptr,
        0, nullptr
    };

    return &s_manifest;
}
