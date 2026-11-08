#include "mmemu_plugin_api.h"
#include "machine_spectrum.h"
#include "libcore/main/machines/machine_registry.h"
#include "util/logging.h"

static const char* s_deps[] = { "z80", nullptr };

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "spectrum",
    "Sinclair ZX Spectrum",
    "1.0.0",
    s_deps,
    nullptr,
    0, nullptr,     // cores
    0, nullptr,     // toolchains
    0, nullptr,     // devices
    0, nullptr,     // machines (registered below)
    0, nullptr,     // loaders
    0, nullptr      // cartridges
};

static void reg(const char* id, spectrum::Model model, const char* desc) {
    MachineRegistry::instance().registerMachine(id, [model, id]() -> MachineDescriptor* {
        std::string err;
        MachineDescriptor* m = spectrum::create(model, &err);
        if (!m) LogRegistry::instance().getLogger("spectrum")->error("{}: {}", id, err);
        return m;
    }, desc);
}

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    reg("spectrum48", spectrum::Model::S48, "Sinclair ZX Spectrum 48K");
    reg("spectrum128", spectrum::Model::S128, "Sinclair ZX Spectrum 128K");
    reg("spectrumplus2", spectrum::Model::Plus2, "Sinclair ZX Spectrum +2 (Amstrad)");
    return &s_manifest;
}
