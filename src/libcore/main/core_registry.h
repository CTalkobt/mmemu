#pragma once

#include "icore.h"
#include <string>
#include <vector>
#include <map>
#include <functional>


class CoreRegistry {
public:
    using FactoryFn = std::function<ICore*()>;

    struct CoreInfo {
        std::string name;
        std::string variant;
        std::string licenseClass;
        FactoryFn   factory;
    };

    static CoreRegistry& instance();
    static void setInstance(CoreRegistry* inst);

    void registerCore(const std::string& name, const std::string& variant, const std::string& license, FactoryFn factory);
    ICore* createCore(const std::string& name);
    void enumerate(std::vector<CoreInfo>& out);
    void clear();

private:
    CoreRegistry() = default;
    std::map<std::string, CoreInfo> m_cores;
};
