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
    MachineDescriptor* createMachine(const std::string& id);
    void enumerate(std::vector<std::string>& ids);
    void enumerateDetailed(std::vector<std::pair<std::string, std::string>>& out);
    void clear();

private:
    MachineRegistry() = default;
    std::map<std::string, FactoryFn> m_factories;
    std::map<std::string, std::string> m_descriptions;
};
