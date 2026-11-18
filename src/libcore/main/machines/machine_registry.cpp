#include "machine_registry.h"
#include "util/logging.h"

static MachineRegistry* s_instance = nullptr;

MachineRegistry& MachineRegistry::instance() {
    if (!s_instance) s_instance = new MachineRegistry();
    return *s_instance;
}

void MachineRegistry::setInstance(MachineRegistry* inst) {
    s_instance = inst;
}

void MachineRegistry::registerMachine(const std::string& id, FactoryFn factory) {
    m_factories[id] = factory;
    if (m_source.empty()) m_sources.erase(id);
    else m_sources[id] = m_source;
}

void MachineRegistry::registerMachine(const std::string& id, FactoryFn factory, const std::string& description) {
    registerMachine(id, std::move(factory));
    m_descriptions[id] = description;
}

void MachineRegistry::registerMachine(const std::string& id, FactoryFn factory, const std::string& description,
                                      const std::string& vendor) {
    registerMachine(id, std::move(factory), description);
    if (vendor.empty()) m_vendors.erase(id);
    else m_vendors[id] = vendor;
}

std::string MachineRegistry::vendorOf(const std::string& id) const {
    const auto it = m_vendors.find(id);
    return it == m_vendors.end() ? "" : it->second;
}

std::string MachineRegistry::sourceOf(const std::string& id) const {
    const auto it = m_sources.find(id);
    return it == m_sources.end() ? "" : it->second;
}

MachineDescriptor* MachineRegistry::createMachine(const std::string& id) {
    auto log = LogRegistry::instance().getLogger("system");
    auto it = m_factories.find(id);
    if (it == m_factories.end()) {
        log->debug("createMachine: no factory for '{}' ({} registered)", id, m_factories.size());
        return nullptr;
    }
    auto* result = it->second();
    if (result && result->vendor.empty()) result->vendor = vendorOf(id);
    if (result)
        log->debug("createMachine: '{}' has {} CPUs and {} buses", id, result->cpus.size(), result->buses.size());
    else
        log->debug("createMachine: factory for '{}' returned null", id);
    return result;
}

void MachineRegistry::enumerate(std::vector<std::string>& ids) {
    for (const auto& pair : m_factories) {
        ids.push_back(pair.first);
    }
}

void MachineRegistry::enumerateDetailed(std::vector<std::pair<std::string, std::string>>& out) {
    for (const auto& pair : m_factories) {
        auto it = m_descriptions.find(pair.first);
        std::string desc = (it != m_descriptions.end()) ? it->second : "";
        out.push_back({pair.first, desc});
    }
}

void MachineRegistry::unregisterMachine(const std::string& id) {
    m_factories.erase(id);
    m_descriptions.erase(id);
    m_vendors.erase(id);
    m_sources.erase(id);
}

void MachineRegistry::clear() {
    m_factories.clear();
    m_descriptions.clear();
    m_vendors.clear();
    m_sources.clear();
}
