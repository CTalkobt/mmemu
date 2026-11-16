#include "mmemu_plugin_api.h"
#include "machine_mc10.h"
#include "mc10_system.h"
#include "libcore/main/image_loader.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/machines/machine_registry.h"
#include "util/logging.h"
#include <algorithm>
#include <fstream>

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

/** Program packs (.rom, .bin: up to 16 KB at $5000) in the expansion port:
 *  the CLI `cart` and `load`/-i, the GUI's Attach Cartridge, MCP
 *  attach_cartridge. The pack takes the port's place (the RAM pack comes
 *  out); the machine is powered on again, as a pack goes in with the power
 *  off. Ejecting puts back what was there. Only an MC-10 or Alice takes them. */
class PakHandler : public ICartridgeHandler {
public:
    explicit PakHandler(std::string path) : m_path(std::move(path)) {}
    bool attach(IBus*, MachineDescriptor* m) override {
        mc10::System* sys = mc10::systemOf(m);
        if (!sys) return false;
        std::ifstream f(m_path, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (data.empty() || data.size() > 0x4000) return false;
        m_before = sys->cart() ? sys->cart()->kind() : "";
        sys->setCart(std::unique_ptr<mc10::Cart>(new mc10::RomPak(std::move(data), baseName())));
        sys->powerOn();
        m_machine = m;
        return true;
    }
    void eject(IBus*) override {
        mc10::System* sys = mc10::systemOf(m_machine);
        if (!sys) return;
        if (m_before == "ram16k") sys->setCart(std::unique_ptr<mc10::Cart>(new mc10::RamPack()));
        else sys->setCart(nullptr);         // (an MCX-128 is put back with `expansion`)
        sys->powerOn();
        m_machine = nullptr;
    }
    CartridgeMetadata metadata() const override {
        CartridgeMetadata md;
        md.type = "MC-10 program pack";
        md.startAddr = 0x5000;
        md.endAddr = 0x8FFF;
        md.imagePath = m_path;
        md.displayName = baseName();
        return md;
    }
private:
    std::string baseName() const {
        const auto slash = m_path.find_last_of('/');
        return slash == std::string::npos ? m_path : m_path.substr(slash + 1);
    }
    std::string m_path, m_before;
    MachineDescriptor* m_machine = nullptr;
};

ICartridgeHandler* createPakHandler(const char* path) { return new PakHandler(path); }
CartridgePluginInfo s_carts[] = { { "rom", createPakHandler }, { "bin", createPakHandler } };

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
    2, s_carts      // cartridges
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
