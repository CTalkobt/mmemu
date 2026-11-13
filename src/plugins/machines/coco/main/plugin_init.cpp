#include "mmemu_plugin_api.h"
#include "machine_coco.h"
#include "coco_system.h"
#include "libcore/main/image_loader.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/machines/machine_registry.h"
#include "util/logging.h"
#include <algorithm>
#include <cstdio>
#include <fstream>

namespace {

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::string baseName(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/** ROM Paks (.ccc, .rom, the Dragon's .dgn): the CLI `cart` and `load`/-i,
 *  the GUI's Attach Cartridge, MCP attach_cartridge. Only a CoCo or Dragon takes them. */
class CocoCartHandler : public ICartridgeHandler {
public:
    explicit CocoCartHandler(std::string path) : m_path(std::move(path)) {}
    bool attach(IBus*, MachineDescriptor* m) override {
        if (!coco::systemOf(m)) return false;
        std::vector<uint8_t> data;
        std::string err;
        if (!readFile(m_path, data) || data.empty() || data.size() > 0x10000) return false;
        if (!coco::insertRomPak(m, data, baseName(m_path), err)) return false;
        m_machine = m;
        return true;
    }
    void eject(IBus*) override {
        if (m_machine) coco::removeRomPak(m_machine);
        m_machine = nullptr;
    }
    CartridgeMetadata metadata() const override {
        CartridgeMetadata md;
        md.type = coco::systemOf(m_machine) && coco::systemOf(m_machine)->dragon() ? "Dragon cartridge" : "CoCo ROM Pak";
        md.startAddr = 0xC000;
        md.endAddr = 0xFEFF;
        md.imagePath = m_path;
        md.displayName = baseName(m_path);
        return md;
    }
private:
    std::string m_path;
    MachineDescriptor* m_machine = nullptr;
};

ICartridgeHandler* createCartHandler(const char* path) { return new CocoCartHandler(path); }
CartridgePluginInfo s_carts[] = { { "ccc", createCartHandler }, { "rom", createCartHandler }, { "dgn", createCartHandler } };

/** .cas: into the cassette recorder with PLAY down (then CLOAD or CLOADM). */
class CasLoader : public IImageLoader {
public:
    bool canLoad(const std::string& path) const override {
        auto dot = path.rfind('.');
        std::string e = dot == std::string::npos ? "" : path.substr(dot + 1);
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        return e == "cas";
    }
    bool load(const std::string& path, IBus*, MachineDescriptor* m, uint32_t) override {
        coco::System* sys = coco::systemOf(m);
        return sys && sys->tape.mountTape(path);
    }
    const char* name() const override { return "CoCo CAS Tape Loader"; }
};

IImageLoader* createCasLoader() { return new CasLoader(); }
ImageLoaderPluginInfo s_loaders[] = { { createCasLoader } };

}  // namespace

static const char* s_deps[] = { "6809", nullptr };

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "coco",
    "TRS-80 Color Computer 1, 2 and 2B; Dragon 32 and 64",
    "1.0.0",
    s_deps,
    nullptr,
    0, nullptr,     // cores
    0, nullptr,     // toolchains
    0, nullptr,     // devices
    0, nullptr,     // machines (registered below)
    1, s_loaders,
    3, s_carts
};

static void reg(const char* id, coco::Model model, int ramKb, const char* desc, coco::Tv tv = coco::Tv::Default) {
    MachineRegistry::instance().registerMachine(id, [model, id, ramKb, tv]() -> MachineDescriptor* {
        std::string err;
        MachineDescriptor* m = coco::create(model, &err, ramKb, nullptr, tv);
        if (!m) LogRegistry::instance().getLogger("coco")->error("{}: {}", id, err);
        else m->machineId = id;     // without ROMs: made, with a warning (create() prints it)
        return m;
    }, desc);
}

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    reg("coco", coco::Model::CoCo2, 64, "TRS-80 Color Computer 2 (64 KB, Extended Color BASIC)");
    reg("coco1", coco::Model::CoCo1, 32, "TRS-80 Color Computer (32 KB, Extended Color BASIC)");
    reg("coco2", coco::Model::CoCo2, 64, "TRS-80 Color Computer 2 (64 KB, Extended Color BASIC)");
    reg("coco2b", coco::Model::CoCo2B, 64, "TRS-80 Color Computer 2B (64 KB, MC6847T1 lower case)");
    reg("cocopal", coco::Model::CoCo2, 64, "TRS-80 Color Computer 2, PAL (50 Hz, 64 KB)", coco::Tv::PAL);
    reg("dragon32", coco::Model::Dragon32, 32, "Dragon 32 (PAL, 32 KB)");
    reg("dragon64", coco::Model::Dragon64, 64, "Dragon 64 (PAL, 64 KB, 6551 RS-232)");
    reg("tano", coco::Model::Dragon64, 64, "Tano Dragon (NTSC Dragon 64)", coco::Tv::NTSC);
    return &s_manifest;
}
