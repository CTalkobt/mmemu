#include "mmemu_plugin_api.h"
#include "machine_c128.h"
#include "libcore/main/machines/machine_registry.h"
#include "libcore/main/image_loader.h"
#include "util/logging.h"
#include <cstring>
#include <fstream>
#include <vector>

static const char* s_deps[] = { "6502", "z80", nullptr };

namespace {

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

/** C128 cartridges (external function ROM): the CLI `cart`, the GUI's Attach
 *  Cartridge, MCP attach_cartridge. Attaching fails on other machines, so a
 *  ".bin" falls through to the Plus/4's handler there. */
class C128CartHandler : public ICartridgeHandler {
public:
    explicit C128CartHandler(std::string path) : m_path(std::move(path)) {}
    bool attach(IBus*, MachineDescriptor* m) override {
        std::vector<uint8_t> data;
        std::string err;
        if (!readFile(m_path, data)) return false;
        if (!c128::insertCartridge(m, data, displayName(), err)) return false;
        m_machine = m;
        return true;
    }
    void eject(IBus*) override {
        if (m_machine) c128::removeCartridge(m_machine);
        m_machine = nullptr;
    }
    CartridgeMetadata metadata() const override {
        CartridgeMetadata md;
        std::string info = m_machine ? c128::cartridgeInfo(m_machine) : "";
        auto paren = info.find(" (");
        md.type = paren == std::string::npos ? "C128 cartridge" : info.substr(paren + 2, info.size() - paren - 3);
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

ICartridgeHandler* createBinHandler(const char* path) { return new C128CartHandler(path); }

/** .crt: only one with the C128 header (the C64's and Plus/4's share it). */
ICartridgeHandler* createCrtHandler(const char* path) {
    std::ifstream f(path, std::ios::binary);
    char sig[16] = {};
    if (!f.read(sig, sizeof sig) || std::memcmp(sig, "C128 CARTRIDGE  ", 16) != 0) return nullptr;
    return new C128CartHandler(path);
}

CartridgePluginInfo s_carts[] = { { "bin", createBinHandler }, { "crt", createCrtHandler } };

}  // namespace

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "c128",
    "Commodore 128",
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

static void reg(const char* id, bool ntsc, const char* desc) {
    MachineRegistry::instance().registerMachine(id, [ntsc, id]() -> MachineDescriptor* {
        std::string err;
        MachineDescriptor* m = c128::create(ntsc, &err);
        if (!m) LogRegistry::instance().getLogger("c128")->error("{}: {}", id, err);
        return m;
    }, desc);
}

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    reg("c128", false, "Commodore 128 (PAL)");
    reg("c128-ntsc", true, "Commodore 128 (NTSC)");
    return &s_manifest;
}
