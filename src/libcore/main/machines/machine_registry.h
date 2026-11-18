#pragma once

#include "libcore/main/machine_desc.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <utility>

/**
 * Global registry for machines.
 */
class MachineRegistry {
public:
    using FactoryFn = std::function<MachineDescriptor*()>;

    static MachineRegistry& instance();
    static void setInstance(MachineRegistry* inst);

    void registerMachine(const std::string& id, FactoryFn factory);
    void registerMachine(const std::string& id, FactoryFn factory, const std::string& description);
    /** With the machine's vendor (see MachineDescriptor::vendor). */
    void registerMachine(const std::string& id, FactoryFn factory, const std::string& description,
                         const std::string& vendor);
    MachineDescriptor* createMachine(const std::string& id);
    void enumerate(std::vector<std::string>& ids);
    void enumerateDetailed(std::vector<std::pair<std::string, std::string>>& out);
    /** The vendor `id` was registered with ("" if none or unknown). */
    std::string vendorOf(const std::string& id) const;
    /** The plugin registering machines now (the plugin loader sets it around
     *  a plugin's initialisation; "" otherwise): kept with each machine. */
    void setSource(const std::string& source) { m_source = source; }
    /** The plugin that registered `id` ("" if registered otherwise, e.g. by a test). */
    std::string sourceOf(const std::string& id) const;
    /** Forget machine `id` (its factory, description and vendor). */
    void unregisterMachine(const std::string& id);
    void clear();

private:
    MachineRegistry() = default;
    std::map<std::string, FactoryFn> m_factories;
    std::map<std::string, std::string> m_descriptions;
    std::map<std::string, std::string> m_vendors;
    std::map<std::string, std::string> m_sources;
    std::string m_source;
};
