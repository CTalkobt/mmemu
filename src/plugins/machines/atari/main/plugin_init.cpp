#include "mmemu_plugin_api.h"
#include "libcore/main/machines/machine_registry.h"
#include "machine_atari.h"
#include "libcore/main/image_loader.h"
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
    bool canLoad(const std::string& path) const override { return extensionOf(path) == "atr"; }
    bool load(const std::string& path, IBus*, MachineDescriptor* m, uint32_t) override {
        std::vector<uint8_t> data;
        std::string err;
        return readFile(path, data) && atari::bootDisk(m, std::move(data), path, err);
    }
    const char* name() const override { return "Atari ATR Disk Loader"; }
};

IImageLoader* createXexLoader() { return new XexLoader(); }
IImageLoader* createAtrLoader() { return new AtrLoader(); }
ImageLoaderPluginInfo s_loaders[] = { { createXexLoader }, { createAtrLoader } };

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
    0, nullptr
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* /*host*/) {
    auto& reg = MachineRegistry::instance();
    reg.registerMachine("atari800",
        []() { return atari::create(atari::Model::Atari800); }, "Atari 800");
    reg.registerMachine("atari800xl",
        []() { return atari::create(atari::Model::Atari800XL); }, "Atari 800XL");
    reg.registerMachine("atari65xe",
        []() { return atari::create(atari::Model::Atari65XE); }, "Atari 65XE");
    return &s_manifest;
}
