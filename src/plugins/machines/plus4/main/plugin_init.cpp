#include "mmemu_plugin_api.h"
#include "machine_plus4.h"
#include "libcore/main/machines/machine_registry.h"
#include "util/logging.h"

static const char* s_deps[] = { "6502", nullptr };

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "plus4",
    "Commodore Plus/4 and C16",
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

static void reg(const char* id, plus4::Model model, bool ntsc, const char* desc) {
    MachineRegistry::instance().registerMachine(id, [model, ntsc, id]() -> MachineDescriptor* {
        std::string err;
        MachineDescriptor* m = plus4::create(model, ntsc, &err);
        if (!m) LogRegistry::instance().getLogger("plus4")->error("{}: {}", id, err);
        return m;
    }, desc);
}

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    reg("plus4", plus4::Model::Plus4, false, "Commodore Plus/4 (PAL)");
    reg("plus4-ntsc", plus4::Model::Plus4, true, "Commodore Plus/4 (NTSC)");
    reg("c16", plus4::Model::C16, false, "Commodore 16 (PAL)");
    reg("c16-ntsc", plus4::Model::C16, true, "Commodore 16 (NTSC)");
    return &s_manifest;
}
