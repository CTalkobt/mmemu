#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include "io_handler.h"

/**
 * Registry for I/O device factories provided by the host or plugins.
 */
class DeviceRegistry {
public:
    using FactoryFn = std::function<IOHandler*()>;

    struct DeviceInfo {
        std::string name;
        FactoryFn   factory;
    };

    static DeviceRegistry& instance();
    static void setInstance(DeviceRegistry* inst);

    /**
     * Register a device factory.
     */
    void registerDevice(const std::string& name, FactoryFn factory);

    /**
     * Create a device instance by its type name.
     */
    IOHandler* createDevice(const std::string& typeName);

    /**
     * Enumerate all registered device types.
     */
    void enumerate(std::vector<std::string>& out);

    /**
     * Clear all registered device factories (used when unloading plugins).
     */
    void clear();

private:
    DeviceRegistry() = default;
    std::map<std::string, FactoryFn> m_factories;
};
