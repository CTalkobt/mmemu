#include "observer_registry.h"
#include <algorithm>

ObserverRegistry& ObserverRegistry::instance() {
    static ObserverRegistry s_instance;
    return s_instance;
}

void ObserverRegistry::registerObserver(ExecutionObserver* obs) {
    if (std::find(m_observers.begin(), m_observers.end(), obs) == m_observers.end()) {
        m_observers.push_back(obs);
    }
}

void ObserverRegistry::unregisterObserver(ExecutionObserver* obs) {
    auto it = std::find(m_observers.begin(), m_observers.end(), obs);
    if (it != m_observers.end()) {
        m_observers.erase(it);
    }
}

void ObserverRegistry::clear() {
    m_observers.clear();
}
