#include "mmemu_plugin_api.h"
#include "machine_plus4.h"
#include "libcore/main/machines/machine_registry.h"
#include "libcore/main/image_loader.h"
#include "util/logging.h"
#include <cstring>
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

/** Plus/4 / C16 cartridges: the CLI `cart`, the GUI's Attach Cartridge, MCP attach_cartridge. */
class Plus4CartHandler : public ICartridgeHandler {
public:
    explicit Plus4CartHandler(std::string path) : m_path(std::move(path)) {}
    bool attach(IBus*, MachineDescriptor* m) override {
        std::vector<uint8_t> data;
        std::string err;
        if (!readFile(m_path, data)) return false;
        if (!plus4::insertCartridge(m, data, displayName(), err)) {
            LogRegistry::instance().getLogger("plus4")->error("{}: {}", m_path, err);
            return false;
        }
        m_machine = m;
        return true;
    }
    void eject(IBus*) override {
        if (m_machine) plus4::removeCartridge(m_machine);
        m_machine = nullptr;
    }
    CartridgeMetadata metadata() const override {
        CartridgeMetadata md;
        std::string info = m_machine ? plus4::cartridgeInfo(m_machine) : "";
        auto paren = info.find(" (");
        md.type = paren == std::string::npos ? "Plus/4 cartridge" : info.substr(paren + 2, info.size() - paren - 3);
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

ICartridgeHandler* createBinHandler(const char* path) { return new Plus4CartHandler(path); }

/** .crt: only one with the Plus/4 header (the C64's share the extension). */
ICartridgeHandler* createCrtHandler(const char* path) {
    std::ifstream f(path, std::ios::binary);
    char sig[16] = {};
    if (!f.read(sig, sizeof sig) || std::memcmp(sig, "PLUS4 CARTRIDGE ", 16) != 0) return nullptr;
    return new Plus4CartHandler(path);
}

CartridgePluginInfo s_carts[] = { { "bin", createBinHandler }, { "crt", createCrtHandler } };

}  // namespace

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "plus4",
    "Commodore Plus/4 and C16",
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

static void reg(const char* id, plus4::Model model, bool ntsc, const char* desc) {
    MachineRegistry::instance().registerMachine(id, [model, ntsc, id]() -> MachineDescriptor* {
        std::string err;
        MachineDescriptor* m = plus4::create(model, ntsc, &err);
        if (!m) LogRegistry::instance().getLogger("plus4")->error("{}: {}", id, err);
        return m;
    }, desc);
}

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    reg("plus4", plus4::Model::Plus4, false, "Commodore Plus/4 (PAL)");
    reg("plus4-ntsc", plus4::Model::Plus4, true, "Commodore Plus/4 (NTSC)");
    reg("c16", plus4::Model::C16, false, "Commodore 16 (PAL)");
    reg("c16-ntsc", plus4::Model::C16, true, "Commodore 16 (NTSC)");
    reg("c116", plus4::Model::C116, false, "Commodore 116 (PAL)");
    reg("c116-ntsc", plus4::Model::C116, true, "Commodore 116 (NTSC)");
    return &s_manifest;
}
