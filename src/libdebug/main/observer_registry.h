#pragma once

#include "execution_observer.h"
#include <vector>

/**
 * Global registry for execution observers that should be attached to every CPU.
 */
class ObserverRegistry {
public:
    static ObserverRegistry& instance();

    void registerObserver(ExecutionObserver* obs);
    void unregisterObserver(ExecutionObserver* obs);

    const std::vector<ExecutionObserver*>& observers() const { return m_observers; }

    void clear();

private:
    ObserverRegistry() = default;
    std::vector<ExecutionObserver*> m_observers;
};
