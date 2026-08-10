#pragma once

#include "libmem/main/ibus.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>

struct MachineDescriptor;

/**
 * Metadata about an attached cartridge.
 */
struct CartridgeMetadata {
    std::string type;
    int bankCount = 0;
    uint32_t startAddr = 0;
    uint32_t endAddr = 0;
    std::string imagePath;
    std::string displayName;
};

/**
 * Interface for cartridge hardware emulation (e.g. banking logic).
 */
class ICartridgeHandler {
public:
    virtual ~ICartridgeHandler() = default;

    /**
     * Attach the cartridge to the machine's bus.
     * @return true if successfully attached.
     */
    virtual bool attach(IBus* bus, MachineDescriptor* machine) = 0;

    /**
     * Remove the cartridge and restore previous bus state.
     */
    virtual void eject(IBus* bus) = 0;

    /**
     * Get metadata for display in the UI.
     */
    virtual CartridgeMetadata metadata() const = 0;
};

/**
 * Interface for loading program/memory images (.prg, .bin, .xex, etc.).
 */
class IImageLoader {
public:
    virtual ~IImageLoader() = default;

    /**
     * Check if this loader can handle the file at the given path.
     */
    virtual bool canLoad(const std::string& path) const = 0;

    /**
     * Load the image into memory.
     * @param path    Path to the image file.
     * @param bus     The bus to load into. If nullptr, uses machine's default bus.
     * @param machine The active machine descriptor.
     * @param addr    Optional target address (required for raw .bin, ignored by .prg).
     * @return true if successful.
     */
    virtual bool load(const std::string& path, IBus* bus, MachineDescriptor* machine, uint32_t addr = 0) = 0;

    /**
     * Unique name for this loader (e.g. "CBM PRG Loader").
     */
    virtual const char* name() const = 0;
};

/**
 * Global registry for image loaders and cartridge handlers.
 */
class ImageLoaderRegistry {
public:
    static ImageLoaderRegistry& instance();
    static void setInstance(ImageLoaderRegistry* inst);

    /**
     * Register a new image loader.
     */
    void registerLoader(std::unique_ptr<IImageLoader> loader);

    /**
     * Register a factory for a cartridge type (indexed by file extension).
     */
    void registerCartridgeHandler(const std::string& extension, std::function<std::unique_ptr<ICartridgeHandler>(const std::string&)> factory);

    /**
     * Find the first loader that can handle the given path.
     */
    IImageLoader* findLoader(const std::string& path);

    /**
     * Create a cartridge handler for the given file extension.
     */
    std::unique_ptr<ICartridgeHandler> createCartridgeHandler(const std::string& path);

    /**
     * List all registered loader names.
     */
    void enumerateLoaders(std::vector<std::string>& out);

    /**
     * Set the active cartridge for a specific bus.
     */
    void setActiveCartridge(IBus* bus, std::unique_ptr<ICartridgeHandler> cart);

    /**
     * Get the active cartridge for a specific bus.
     */
    ICartridgeHandler* getActiveCartridge(IBus* bus);

    /**
     * Clear all registered loaders and cartridge handlers (used when unloading plugins).
     */
    void clear();

private:
    ImageLoaderRegistry() = default;
    std::vector<std::unique_ptr<IImageLoader>> m_loaders;
    std::map<std::string, std::function<std::unique_ptr<ICartridgeHandler>(const std::string&)>> m_cartFactories;
    std::map<IBus*, std::unique_ptr<ICartridgeHandler>> m_activeCarts;
};
