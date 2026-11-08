#include "mmemu_plugin_api.h"
#include "machine_bbc.h"
#include "machine_electron.h"
#include "libcore/main/machines/machine_registry.h"
#include "util/logging.h"

static const char* s_deps[] = { "6502", "z80", "6809", "arm", nullptr };

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "bbc",
    "Acorn BBC Micro and Electron",
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

static void reg(const char* id, bbc::Model model, const char* desc) {
    MachineRegistry::instance().registerMachine(id, [model, id]() -> MachineDescriptor* {
        std::string err;
        MachineDescriptor* m = bbc::create(model, &err);
        if (!m) LogRegistry::instance().getLogger("bbc")->error("{}: {}", id, err);
        return m;
    }, desc);
}

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    reg("bbca", bbc::Model::A, "BBC Micro Model A (16 KB, OS 1.20, BASIC 2)");
    reg("bbcb", bbc::Model::B, "BBC Micro Model B (32 KB, OS 1.20, DFS 0.90, BASIC 2)");
    reg("bbcb-6502", bbc::Model::BTube, "BBC Micro Model B with the 6502 Second Processor (DNFS, Tube)");
    reg("bbcb-z80", bbc::Model::BZ80, "BBC Micro Model B with the Z80 Second Processor (DNFS, Tube)");
    reg("bbcb-6809", bbc::Model::B6809, "BBC Micro Model B with the 6809 Second Processor (DNFS, Tube)");
    reg("bbcb-arm", bbc::Model::BArm, "BBC Micro Model B with the ARM Evaluation System (DNFS, Tube)");
    MachineRegistry::instance().registerMachine("electron", []() -> MachineDescriptor* {
        std::string err;
        MachineDescriptor* m = electron::create(&err);
        if (!m) LogRegistry::instance().getLogger("bbc")->error("electron: {}", err);
        return m;
    }, "Acorn Electron (OS 1.00, BASIC 2)");
    return &s_manifest;
}
