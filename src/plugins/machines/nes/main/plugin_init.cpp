#include "mmemu_plugin_api.h"
#include "machine_nes.h"
#include "nes_cart.h"
#include "libcore/main/machines/machine_registry.h"
#include "libcore/main/image_loader.h"
#include <fstream>
#include <vector>

static const char* s_deps[] = { "6502", nullptr };

namespace {

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

/** NES cartridges (.nes, iNES / NES 2.0): the CLI `cart`, the GUI's Attach
 *  Cartridge, MCP attach_cartridge. */
class NesCartHandler : public ICartridgeHandler {
public:
    explicit NesCartHandler(std::string path) : m_path(std::move(path)) {}
    bool attach(IBus*, MachineDescriptor* m) override {
        std::vector<uint8_t> data;
        std::string err;
        if (!readFile(m_path, data) || !nes::insertCartridge(m, data, displayName(), err)) return false;
        m_machine = m;
        return true;
    }
    void eject(IBus*) override {
        if (m_machine) nes::removeCartridge(m_machine);
        m_machine = nullptr;
    }
    CartridgeMetadata metadata() const override {
        CartridgeMetadata md;
        std::string info = m_machine ? nes::cartridgeInfo(m_machine) : "";
        auto paren = info.rfind(" (");
        md.type = paren == std::string::npos ? "NES cartridge" : "NES " + info.substr(paren + 2, info.size() - paren - 3);
        md.startAddr = 0x8000;
        md.endAddr = 0xFFFF;
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

ICartridgeHandler* createHandler(const char* path) { return new NesCartHandler(path); }
CartridgePluginInfo s_carts[] = { { "nes", createHandler } };

}  // namespace

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "nes",
    "Nintendo Entertainment System / Famicom",
    "1.0.0",
    s_deps,
    nullptr,
    0, nullptr,     // cores
    0, nullptr,     // toolchains
    0, nullptr,     // devices
    0, nullptr,     // machines (registered below)
    0, nullptr,     // loaders
    1, s_carts      // cartridges
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    auto& reg = MachineRegistry::instance();
    reg.registerMachine("nes", []() { return nes::create(false); }, "Nintendo Entertainment System (NTSC)");
    reg.registerMachine("famicom", []() { return nes::create(true); }, "Nintendo Famicom");
    return &s_manifest;
}
