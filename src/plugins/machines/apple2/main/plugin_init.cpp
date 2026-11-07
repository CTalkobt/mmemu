#include "mmemu_plugin_api.h"
#include "machine_apple2.h"
#include "libcore/main/machines/machine_registry.h"
#include "util/logging.h"

static const char* s_deps[] = { "6502", nullptr };

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "apple2",
    "Apple II family",
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

static void reg(const char* id, apple2::Model model, const char* desc) {
    MachineRegistry::instance().registerMachine(id, [model, id]() -> MachineDescriptor* {
        std::string err;
        MachineDescriptor* m = apple2::create(model, &err);
        if (!m) LogRegistry::instance().getLogger("apple2")->error("{}: {}", id, err);
        return m;
    }, desc);
}

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    reg("apple2", apple2::Model::II, "Apple ][ (Integer BASIC)");
    reg("apple2plus", apple2::Model::IIPlus, "Apple ][+ (Applesoft)");
    reg("apple2e", apple2::Model::IIe, "Apple //e (128 KB, 80 columns)");
    reg("apple2e-enhanced", apple2::Model::IIeEnhanced, "Apple //e enhanced (65C02, MouseText)");
    return &s_manifest;
}
