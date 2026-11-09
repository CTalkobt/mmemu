#include "mmemu_plugin_api.h"
#include "machine_snes.h"
#include "libcore/main/machines/machine_registry.h"
#include "libcore/main/image_loader.h"
#include <fstream>
#include <vector>

static const char* s_deps[] = { "65816", nullptr };

namespace {

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

/** SNES cartridges (.sfc, .smc): the CLI `cart`, the GUI's Attach
 *  Cartridge, MCP attach_cartridge. */
class SnesCartHandler : public ICartridgeHandler {
public:
    explicit SnesCartHandler(std::string path) : m_path(std::move(path)) {}
    bool attach(IBus*, MachineDescriptor* m) override {
        std::vector<uint8_t> data;
        std::string err;
        if (!readFile(m_path, data) || !snes::insertCartridge(m, data, displayName(), err)) return false;
        m_machine = m;
        return true;
    }
    void eject(IBus*) override {
        if (m_machine) snes::removeCartridge(m_machine);
        m_machine = nullptr;
    }
    CartridgeMetadata metadata() const override {
        CartridgeMetadata md;
        std::string info = m_machine ? snes::cartridgeInfo(m_machine) : "";
        auto paren = info.find(" (");
        md.type = paren == std::string::npos ? "SNES cartridge" : "SNES " + info.substr(paren + 2, info.size() - paren - 3);
        md.startAddr = 0x008000;
        md.endAddr = 0xFFFFFF;
        md.imagePath = m_path;
        md.displayName = displayName();
        return md;
    }
private:
    std::string displayName() const {
        auto slash = m_path.find_last_of('/');
        return slash == std::string::npos ? m_path : m_path.substr(slash + 1);
    }
    std::string m_path;
    MachineDescriptor* m_machine = nullptr;
};

ICartridgeHandler* createHandler(const char* path) { return new SnesCartHandler(path); }
CartridgePluginInfo s_carts[] = { { "sfc", createHandler }, { "smc", createHandler } };

}  // namespace

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "snes",
    "Super Nintendo / Super Famicom",
    "1.0.0",
    s_deps,
    nullptr,
    0, nullptr,     // cores
    0, nullptr,     // toolchains
    0, nullptr,     // devices
    0, nullptr,     // machines (registered below)
    0, nullptr,     // loaders
    2, s_carts      // cartridges
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    auto& reg = MachineRegistry::instance();
    reg.registerMachine("snes", []() { return snes::create(false); }, "Super Nintendo Entertainment System (NTSC)");
    reg.registerMachine("superfamicom", []() { return snes::create(true); }, "Nintendo Super Famicom");
    return &s_manifest;
}
