#include "mmemu_plugin_api.h"
#include "machine_bbc.h"
#include "libcore/main/machines/machine_registry.h"
#include "util/logging.h"

static const char* s_deps[] = { "6502", nullptr };

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "bbc",
    "Acorn BBC Micro",
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

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    MachineRegistry::instance().registerMachine("bbcb", []() -> MachineDescriptor* {
        std::string err;
        MachineDescriptor* m = bbc::create(bbc::Model::B, &err);
        if (!m) LogRegistry::instance().getLogger("bbc")->error("bbcb: {}", err);
        return m;
    }, "BBC Micro Model B (OS 1.20, BASIC 2)");
    return &s_manifest;
}
