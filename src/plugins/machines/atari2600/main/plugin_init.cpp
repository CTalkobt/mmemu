#include "mmemu_plugin_api.h"
#include "machine_a2600.h"
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

/** 2600 cartridges (.a26, .bin): the CLI `cart`, the GUI's Attach Cartridge,
 *  MCP attach_cartridge. Attaching fails on other machines, so a ".bin"
 *  falls through to their handlers. */
class A2600CartHandler : public ICartridgeHandler {
public:
    explicit A2600CartHandler(std::string path) : m_path(std::move(path)) {}
    bool attach(IBus*, MachineDescriptor* m) override {
        std::vector<uint8_t> data;
        std::string err;
        if (!readFile(m_path, data) || !a2600::insertCartridge(m, data, displayName(), err)) return false;
        m_machine = m;
        return true;
    }
    void eject(IBus*) override {
        if (m_machine) a2600::removeCartridge(m_machine);
        m_machine = nullptr;
    }
    CartridgeMetadata metadata() const override {
        CartridgeMetadata md;
        std::string info = m_machine ? a2600::cartridgeInfo(m_machine) : "";
        auto paren = info.rfind(" (");
        md.type = paren == std::string::npos ? "2600 cartridge" : "2600 " + info.substr(paren + 2, info.size() - paren - 3);
        md.startAddr = 0x1000;
        md.endAddr = 0x1FFF;
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

ICartridgeHandler* createHandler(const char* path) { return new A2600CartHandler(path); }
CartridgePluginInfo s_carts[] = { { "a26", createHandler }, { "bin", createHandler } };

}  // namespace

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "atari2600",
    "Atari 2600 (VCS)",
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
    reg.registerMachine("atari2600", []() { return a2600::create(false); }, "Atari 2600 (NTSC)");
    reg.registerMachine("atari2600-pal", []() { return a2600::create(true); }, "Atari 2600 (PAL)");
    return &s_manifest;
}
