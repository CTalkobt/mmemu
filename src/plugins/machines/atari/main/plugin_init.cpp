#include "mmemu_plugin_api.h"
#include "libcore/main/machines/machine_registry.h"
#include "machine_atari.h"

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "atari",
    "Atari 8-bit (800, 800XL, 65XE)",
    "1.0.0",
    nullptr,
    nullptr,
    0, nullptr,
    0, nullptr,
    0, nullptr,
    0, nullptr,
    0, nullptr,
    0, nullptr
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    auto& reg = MachineRegistry::instance();
    reg.registerMachine("atari800",
        []() { return atari::create(atari::Model::Atari800); }, "Atari 800");
    reg.registerMachine("atari800xl",
        []() { return atari::create(atari::Model::Atari800XL); }, "Atari 800XL");
    reg.registerMachine("atari65xe",
        []() { return atari::create(atari::Model::Atari65XE); }, "Atari 65XE");
    return &s_manifest;
}
