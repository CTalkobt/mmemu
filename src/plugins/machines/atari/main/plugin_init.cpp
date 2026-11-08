#include "mmemu_plugin_api.h"
#include "libcore/main/machines/machine_registry.h"
#include "machine_atari.h"
#include "libcore/main/image_loader.h"
#include "libdevices/main/disk_file.h"
#include <algorithm>
#include <fstream>

namespace {

std::string extensionOf(const std::string& path) {
    auto dot = path.rfind('.');
    std::string e = dot == std::string::npos ? "" : path.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

/** .xex / .com: boot the executable from a generated disk. */
class XexLoader : public IImageLoader {
public:
    bool canLoad(const std::string& path) const override {
        std::string e = extensionOf(path);
        return e == "xex" || e == "com";
    }
    bool load(const std::string& path, IBus*, MachineDescriptor* m, uint32_t) override {
        std::vector<uint8_t> data;
        std::string err;
        return readFile(path, data) && atari::bootExecutable(m, data, path, err);
    }
    const char* name() const override { return "Atari XEX Loader"; }
};

/** .atr: put the disk in drive 1 and boot it. */
class AtrLoader : public IImageLoader {
public:
    // Plain, .gz or .zip (#233)
    bool canLoad(const std::string& path) const override { return diskfile::imageExt(path) == "atr"; }
    bool load(const std::string& path, IBus*, MachineDescriptor* m, uint32_t) override {
        std::vector<uint8_t> data;
        std::string err;
        return diskfile::read(path, data) && atari::bootDisk(m, std::move(data), path, err, path);
    }
    const char* name() const override { return "Atari ATR Disk Loader"; }
};

IImageLoader* createXexLoader() { return new XexLoader(); }
IImageLoader* createAtrLoader() { return new AtrLoader(); }
ImageLoaderPluginInfo s_loaders[] = { { createXexLoader }, { createAtrLoader } };

/** .car / .rom cartridges: the CLI `cart`, the GUI's Attach Cartridge, MCP attach_cartridge. */
class AtariCartHandler : public ICartridgeHandler {
public:
    explicit AtariCartHandler(std::string path) : m_path(std::move(path)) {}
    bool attach(IBus*, MachineDescriptor* m) override {
        std::vector<uint8_t> data;
        std::string err;
        if (!readFile(m_path, data) || !atari::insertCartridge(m, data, m_path, err)) return false;
        m_machine = m;
        return true;
    }
    void eject(IBus*) override {
        if (m_machine) atari::removeCartridge(m_machine);
        m_machine = nullptr;
    }
    CartridgeMetadata metadata() const override {
        CartridgeMetadata md;
        std::string info = m_machine ? atari::cartridgeInfo(m_machine) : "";
        auto paren = info.rfind(" (");
        md.type = paren == std::string::npos ? "Atari cartridge" : info.substr(paren + 2, info.size() - paren - 3);
        md.startAddr = 0x8000;
        md.endAddr = 0xBFFF;
        md.imagePath = m_path;
        auto slash = m_path.find_last_of('/');
        md.displayName = slash == std::string::npos ? m_path : m_path.substr(slash + 1);
        return md;
    }
private:
    std::string m_path;
    MachineDescriptor* m_machine = nullptr;
};

ICartridgeHandler* createCartHandler(const char* path) { return new AtariCartHandler(path); }
CartridgePluginInfo s_carts[] = { { "car", createCartHandler }, { "rom", createCartHandler } };

}  // namespace

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "atari",
    "Atari 8-bit (800, 800XL, 65XE)",
    "1.0.0",
    nullptr,
    nullptr,
    0, nullptr,
    0, nullptr,
    0, nullptr,
    0, nullptr,
    2, s_loaders,
    2, s_carts
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    auto& reg = MachineRegistry::instance();
    reg.registerMachine("atari800",
        []() { return atari::create(atari::Model::Atari800); }, "Atari 800");
    reg.registerMachine("atari800xl",
        []() { return atari::create(atari::Model::Atari800XL); }, "Atari 800XL");
    reg.registerMachine("atari65xe",
        []() { return atari::create(atari::Model::Atari65XE); }, "Atari 65XE");
    reg.registerMachine("atari800-pal",
        []() { return atari::create(atari::Model::Atari800, nullptr, true); }, "Atari 800 (PAL)");
    reg.registerMachine("atari800xl-pal",
        []() { return atari::create(atari::Model::Atari800XL, nullptr, true); }, "Atari 800XL (PAL)");
    reg.registerMachine("atari65xe-pal",
        []() { return atari::create(atari::Model::Atari65XE, nullptr, true); }, "Atari 65XE (PAL)");
    return &s_manifest;
}
