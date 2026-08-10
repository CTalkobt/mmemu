#include "image_loader.h"
#include "machine_desc.h"
#include <fstream>
#include <iostream>
#include <algorithm>

/**
 * Default BIN loader for raw binary files (.bin).
 */
class BinLoader : public IImageLoader {
public:
    bool canLoad(const std::string& path) const override {
        size_t dot = path.find_last_of('.');
        if (dot == std::string::npos) return false;
        std::string ext = path.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return ext == "bin";
    }

    bool load(const std::string& path, IBus* bus, MachineDescriptor* machine, uint32_t addr) override {
        // If bus is not provided, try to find a system bus in the machine
        if (!bus && machine) {
            if (!machine->buses.empty()) {
                bus = machine->buses[0].bus;
            }
        }
        
        if (!bus) return false;

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return false;

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<uint8_t> buffer(size);
        if (file.read((char*)buffer.data(), size)) {
            for (size_t i = 0; i < (size_t)size; ++i) {
                bus->write8(addr + (uint32_t)i, buffer[i]);
            }
            return true;
        }
        return false;
    }

    const char* name() const override { return "Raw Binary Loader"; }
};

// ImageLoaderRegistry implementation
static ImageLoaderRegistry* s_instance = nullptr;

ImageLoaderRegistry& ImageLoaderRegistry::instance() {
    if (!s_instance) {
        s_instance = new ImageLoaderRegistry();
        // Register default BIN loader
        s_instance->registerLoader(std::make_unique<BinLoader>());
    }
    return *s_instance;
}

void ImageLoaderRegistry::setInstance(ImageLoaderRegistry* inst) {
    s_instance = inst;
}

void ImageLoaderRegistry::registerLoader(std::unique_ptr<IImageLoader> loader) {
    m_loaders.push_back(std::move(loader));
}

void ImageLoaderRegistry::registerCartridgeHandler(const std::string& extension, std::function<std::unique_ptr<ICartridgeHandler>(const std::string&)> factory) {
    std::string ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    m_cartFactories[ext] = factory;
}

IImageLoader* ImageLoaderRegistry::findLoader(const std::string& path) {
    for (auto& loader : m_loaders) {
        if (loader->canLoad(path)) return loader.get();
    }
    return nullptr;
}

std::unique_ptr<ICartridgeHandler> ImageLoaderRegistry::createCartridgeHandler(const std::string& path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return nullptr;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    auto it = m_cartFactories.find(ext);
    if (it != m_cartFactories.end()) {
        return it->second(path);
    }
    return nullptr;
}

void ImageLoaderRegistry::enumerateLoaders(std::vector<std::string>& out) {
    for (auto& loader : m_loaders) {
        out.push_back(loader->name());
    }
}

void ImageLoaderRegistry::setActiveCartridge(IBus* bus, std::unique_ptr<ICartridgeHandler> cart) {
    m_activeCarts[bus] = std::move(cart);
}

ICartridgeHandler* ImageLoaderRegistry::getActiveCartridge(IBus* bus) {
    auto it = m_activeCarts.find(bus);
    if (it != m_activeCarts.end()) {
        return it->second.get();
    }
    return nullptr;
}

void ImageLoaderRegistry::clear() {
    m_loaders.clear();
    // Re-register the default BIN loader after clearing
    m_loaders.push_back(std::make_unique<BinLoader>());
    m_cartFactories.clear();
}
