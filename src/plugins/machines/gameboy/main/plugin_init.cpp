#include "mmemu_plugin_api.h"
#include "machine_gameboy.h"
#include "sm83_disasm.h"
#include "libcore/main/machines/machine_registry.h"
#include "libcore/main/image_loader.h"
#include <fstream>
#include <vector>

namespace {

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

/** Game Boy cartridges (.gb, .gbc): the CLI `cart`, the GUI's Attach Cartridge,
 *  MCP attach_cartridge. (On a SNES the SNES plugin's handler puts them in the
 *  Super Game Boy; this one only attaches to a Game Boy.) */
class GbCartHandler : public ICartridgeHandler {
public:
    explicit GbCartHandler(std::string path) : m_path(std::move(path)) {}
    bool attach(IBus*, MachineDescriptor* m) override {
        if (!gb::gameboyOf(m)) return false;
        std::vector<uint8_t> data;
        std::string err;
        if (!readFile(m_path, data) || !gb::insertCartridge(m, data, displayName(), err, gb::savePathFor(m_path))) return false;
        m_machine = m;
        return true;
    }
    void eject(IBus*) override {
        if (m_machine) gb::removeCartridge(m_machine);
        m_machine = nullptr;
    }
    CartridgeMetadata metadata() const override {
        CartridgeMetadata md;
        std::string info = m_machine ? gb::cartridgeInfo(m_machine) : "";
        auto paren = info.rfind(" (");
        md.type = paren == std::string::npos ? "Game Boy cartridge" : "Game Boy " + info.substr(paren + 2, info.size() - paren - 3);
        md.startAddr = 0x0000;
        md.endAddr = 0x7FFF;
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

ICartridgeHandler* createHandler(const char* path) { return new GbCartHandler(path); }
CartridgePluginInfo s_carts[] = { { "gb", createHandler }, { "gbc", createHandler } };

IDisassembler* createDisasmSm83() { return new DisassemblerSm83(); }
ToolchainPluginInfo s_toolchains[] = { {"sm83", nullptr, createDisasmSm83, nullptr} };

}  // namespace

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "gameboy",
    "Nintendo Game Boy (DMG) and Game Boy Color",
    "1.0.0",
    nullptr,
    nullptr,
    0, nullptr,         // cores (the SM83 comes with the machine)
    1, s_toolchains,
    0, nullptr,         // devices
    0, nullptr,         // machines (registered below)
    0, nullptr,         // loaders
    2, s_carts          // cartridges
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    auto& reg = MachineRegistry::instance();
    reg.registerMachine("gameboy", []() { return gb::createMachine(); }, "Nintendo Game Boy (DMG)");
    reg.registerMachine("dmg", []() { return gb::createMachine(); }, "Nintendo Game Boy (DMG), as gameboy");
    reg.registerMachine("gameboycolor", []() { return gb::createMachine(nullptr, true); }, "Nintendo Game Boy Color (CGB)");
    reg.registerMachine("gbc", []() { return gb::createMachine(nullptr, true); }, "Nintendo Game Boy Color, as gameboycolor");
    reg.registerMachine("cgb", []() { return gb::createMachine(nullptr, true); }, "Nintendo Game Boy Color, as gameboycolor");
    return &s_manifest;
}
