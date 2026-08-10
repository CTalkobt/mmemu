#include "device_registry.h"

static DeviceRegistry* s_instance = nullptr;

DeviceRegistry& DeviceRegistry::instance() {
    if (!s_instance) {
        s_instance = new DeviceRegistry();
    }
    return *s_instance;
}

void DeviceRegistry::setInstance(DeviceRegistry* inst) {
    s_instance = inst;
}

void DeviceRegistry::registerDevice(const std::string& typeName, FactoryFn factory) {
    m_factories[typeName] = factory;
}

IOHandler* DeviceRegistry::createDevice(const std::string& typeName) {
    auto it = m_factories.find(typeName);
    if (it != m_factories.end()) {
        return it->second();
    }
    return nullptr;
}

void DeviceRegistry::enumerate(std::vector<std::string>& out) {
    for (auto const& [name, factory] : m_factories) {
        out.push_back(name);
    }
}

void DeviceRegistry::clear() {
    m_factories.clear();
}
