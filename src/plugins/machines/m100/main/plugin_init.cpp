#include "mmemu_plugin_api.h"
#include "machine_m100.h"
#include "m100_system.h"
#include "libcore/main/image_loader.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/machines/machine_registry.h"
#include "util/logging.h"
#include <fstream>

namespace {

std::string baseName(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/** Option ROMs (.rom, .bin up to 32 KB) into the socket: the CLI `cart` and
 *  `load`/-i (.rom), the GUI's Attach Cartridge. Only a Model 100 or a sibling takes them. */
class OptRomHandler : public ICartridgeHandler {
public:
    explicit OptRomHandler(std::string path) : m_path(std::move(path)) {}
    bool attach(IBus*, MachineDescriptor* m) override {
        if (!m100::systemOf(m)) return false;
        std::ifstream f(m_path, std::ios::binary);
        if (!f) return false;
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string err;
        if (data.empty() || !m100::insertOptionRom(m, data, baseName(m_path), err)) return false;
        m_machine = m;
        return true;
    }
    void eject(IBus*) override {
        std::string err;
        if (m_machine) m100::insertOptionRom(m_machine, {}, "", err);
        m_machine = nullptr;
    }
    CartridgeMetadata metadata() const override {
        CartridgeMetadata md;
        md.type = "Model 100/102/200 option ROM";
        md.startAddr = 0x0000;
        md.endAddr = 0x7FFF;
        md.imagePath = m_path;
        md.displayName = baseName(m_path);
        return md;
    }
private:
    std::string m_path;
    MachineDescriptor* m_machine = nullptr;
};

ICartridgeHandler* createOptRom(const char* path) { return new OptRomHandler(path); }
CartridgePluginInfo s_carts[] = { { "rom", createOptRom }, { "bin", createOptRom } };

}  // namespace

static const char* s_deps[] = { "8085", nullptr };

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "m100",
    "Tandy Model 100, 102 and 200; NEC PC-8201A; Olivetti M10; Kyotronic 85",
    "1.0.0",
    s_deps,
    nullptr,
    0, nullptr,     // cores
    0, nullptr,     // toolchains
    0, nullptr,     // devices
    0, nullptr,     // machines (registered below)
    0, nullptr,     // loaders
    2, s_carts
};

static void reg(const char* id, m100::Model model, const char* desc) {
    MachineRegistry::instance().registerMachine(id, [model, id]() -> MachineDescriptor* {
        std::string err;
        MachineDescriptor* m = m100::create(model, &err);
        if (!m) LogRegistry::instance().getLogger("m100")->error("{}: {}", id, err);
        else m->machineId = id;     // without ROMs: made, with a warning (create() prints it)
        return m;
    }, desc);
}

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    reg("model100", m100::Model::M100, "Tandy Model 100 (32 KB, option ROM socket)");
    reg("m100", m100::Model::M100, "Tandy Model 100 (alias of model100)");
    reg("model102", m100::Model::M102, "Tandy Model 102 (32 KB)");
    reg("model200", m100::Model::M200, "Tandy 200 (72 KB in three banks)");
    reg("pc8201a", m100::Model::PC8201, "NEC PC-8201A (up to 96 KB in banks)");
    reg("pc8201", m100::Model::PC8201, "NEC PC-8201A (alias of pc8201a)");
    reg("m10", m100::Model::M10, "Olivetti M10 (32 KB, European keyboard)");
    reg("kc85", m100::Model::KC85, "Kyocera Kyotronic 85 (32 KB)");
    return &s_manifest;
}
