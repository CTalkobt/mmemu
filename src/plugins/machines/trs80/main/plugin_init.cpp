#include "mmemu_plugin_api.h"
#include "machine_trs80.h"
#include "trs80_system.h"
#include "libcore/main/image_loader.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/machines/machine_registry.h"
#include "libdevices/main/io_registry.h"
#include "util/logging.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>

namespace {

std::string extOf(const std::string& path) {
    const auto dot = path.rfind('.');
    std::string e = dot == std::string::npos ? "" : path.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return e;
}

/** .cas: into the machine's cassette recorder with PLAY down (then CLOAD or SYSTEM). */
class CasLoader : public IImageLoader {
public:
    bool canLoad(const std::string& path) const override { return extOf(path) == "cas"; }
    bool load(const std::string& path, IBus*, MachineDescriptor* m, uint32_t) override {
        if (!m || !m->ioRegistry) return false;
        std::vector<IOHandler*> hs;
        m->ioRegistry->enumerate(hs);
        for (auto* h : hs)
            if (h->mountTape(path)) return true;
        return false;
    }
    const char* name() const override { return "CAS Tape Loader (the machine's recorder)"; }
};

/** /CMD programs (TRS-80 DOS load modules): into memory, PC at the transfer address. */
class CmdLoader : public IImageLoader {
public:
    bool canLoad(const std::string& path) const override { return extOf(path) == "cmd"; }
    bool load(const std::string& path, IBus*, MachineDescriptor* m, uint32_t) override {
        if (!trs80::systemOf(m)) return false;
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string err;
        if (!trs80::loadCmd(m, b, err)) { std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str()); return false; }
        return true;
    }
    const char* name() const override { return "TRS-80 CMD Loader"; }
};

IImageLoader* createCasLoader() { return new CasLoader(); }
IImageLoader* createCmdLoader() { return new CmdLoader(); }
ImageLoaderPluginInfo s_loaders[] = { { createCasLoader }, { createCmdLoader } };

}  // namespace

static const char* s_deps[] = { "z80", nullptr };

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "trs80",
    "TRS-80 Model I, III, 4 and 4P",
    "1.0.0",
    s_deps,
    nullptr,
    0, nullptr,     // cores
    0, nullptr,     // toolchains
    0, nullptr,     // devices
    0, nullptr,     // machines (registered below)
    2, s_loaders,
    0, nullptr      // cartridges
};

static void reg(const char* id, trs80::Model model, const char* desc) {
    MachineRegistry::instance().registerMachine(id, [model, id]() -> MachineDescriptor* {
        std::string err;
        MachineDescriptor* m = trs80::create(model, &err);
        if (!m) LogRegistry::instance().getLogger("trs80")->error("{}: {}", id, err);
        else m->machineId = id;     // without ROMs: made, with a warning (create() prints it)
        return m;
    }, desc);
}

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    reg("trs80m1", trs80::Model::M1Level2, "TRS-80 Model I (Level II BASIC, Expansion Interface, 48 KB)");
    reg("trs80m1l1", trs80::Model::M1Level1, "TRS-80 Model I (Level I BASIC, 4 KB)");
    reg("trs80m3", trs80::Model::M3, "TRS-80 Model III (48 KB, FD1793)");
    reg("trs80m4", trs80::Model::M4, "TRS-80 Model 4 (128 KB)");
    reg("trs80m4p", trs80::Model::M4P, "TRS-80 Model 4P (128 KB, boot ROM)");
    return &s_manifest;
}
