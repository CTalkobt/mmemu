#include "mmemu_plugin_api.h"
#include "machine_mc10.h"
#include "mc10_system.h"
#include "libcore/main/image_loader.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/machines/machine_registry.h"
#include "util/logging.h"
#include <algorithm>

namespace {

/** .c10: into the recorder, then CLOAD typed and PLAY pressed (an MC-10 or Alice only). */
class C10Loader : public IImageLoader {
public:
    bool canLoad(const std::string& path) const override {
        auto dot = path.rfind('.');
        std::string e = dot == std::string::npos ? "" : path.substr(dot + 1);
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        return e == "c10";
    }
    bool load(const std::string& path, IBus*, MachineDescriptor* m, uint32_t) override {
        mc10::System* sys = mc10::systemOf(m);
        if (!sys || !sys->tapeDeck.mountTape(path)) return false;
        sys->keyboard.enqueueText("CLOAD\n");
        sys->playAfterTyping();
        return true;
    }
    const char* name() const override { return "MC-10 C10 Tape Loader"; }
};

IImageLoader* createC10Loader() { return new C10Loader(); }
ImageLoaderPluginInfo s_loaders[] = { { createC10Loader } };

}  // namespace

static const char* s_deps[] = { "6803", nullptr };

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "mc10",
    "TRS-80 MC-10 Micro Color Computer; Matra Alice",
    "1.0.0",
    s_deps,
    nullptr,
    0, nullptr,     // cores
    0, nullptr,     // toolchains
    0, nullptr,     // devices
    0, nullptr,     // machines (registered below)
    1, s_loaders,
    0, nullptr      // cartridges
};

static void reg(const char* id, mc10::Model model, mc10::Expansion exp, const char* desc) {
    MachineRegistry::instance().registerMachine(id, [model, id, exp]() -> MachineDescriptor* {
        std::string err;
        MachineDescriptor* m = mc10::create(model, &err, exp);
        if (!m) LogRegistry::instance().getLogger("mc10")->error("{}: {}", id, err);
        else m->machineId = id;     // without ROMs: made, with a warning (create() prints it)
        return m;
    }, desc);
}

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    reg("mc10", mc10::Model::MC10, mc10::Expansion::RamPack16K, "TRS-80 MC-10 Micro Color Computer (20 KB with the 16K RAM pack)");
    reg("mc10-4k", mc10::Model::MC10, mc10::Expansion::None, "TRS-80 MC-10 Micro Color Computer (4 KB, unexpanded)");
    reg("alice", mc10::Model::Alice, mc10::Expansion::RamPack16K, "Matra Alice (PAL MC-10, AZERTY; 20 KB)");
    return &s_manifest;
}
