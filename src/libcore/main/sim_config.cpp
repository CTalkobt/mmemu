#include "sim_config.h"
#include "util/path_util.h"
#include <fstream>
#include <spdlog/spdlog.h>

SimConfig& SimConfig::instance() {
    static SimConfig s_instance;
    return s_instance;
}

void SimConfig::load() {
    std::string configPath = findConfigFile();
    if (configPath.empty()) {
        spdlog::debug("No config.json found; using defaults");
        m_root = nlohmann::json::object();
        return;
    }

    try {
        std::ifstream file(configPath);
        if (!file.is_open()) {
            spdlog::warn("Could not open config file: {}", configPath);
            m_root = nlohmann::json::object();
            return;
        }
        m_root = nlohmann::json::parse(file);
        spdlog::info("Loaded config from: {}", configPath);
    } catch (const std::exception& e) {
        spdlog::warn("Failed to parse config.json: {}", e.what());
        m_root = nlohmann::json::object();
    }
}

AssemblerConfig SimConfig::assemblerConfig(const std::string& name) const {
    AssemblerConfig config;

    // Look for tools.assemblers.<name>
    if (m_root.contains("tools") && m_root["tools"].is_object()) {
        auto tools = m_root["tools"];
        if (tools.contains("assemblers") && tools["assemblers"].is_object()) {
            auto assemblers = tools["assemblers"];
            if (assemblers.contains(name) && assemblers[name].is_object()) {
                auto asmConfig = assemblers[name];
                if (asmConfig.contains("command") && asmConfig["command"].is_string()) {
                    config.binaryPath = asmConfig["command"].get<std::string>();
                }
                if (asmConfig.contains("extraOptions") && asmConfig["extraOptions"].is_string()) {
                    config.extraOptions = asmConfig["extraOptions"].get<std::string>();
                }
                if (asmConfig.contains("workingDir") && asmConfig["workingDir"].is_string()) {
                    config.workingDir = asmConfig["workingDir"].get<std::string>();
                }
                // Optionally parse env vars if needed
            }
        }
    }

    return config;
}

std::string SimConfig::findConfigFile() const {
    // Search PathUtil data paths for config.json
    std::vector<std::string> searchPaths = PathUtil::getDataSearchPaths();
    searchPaths.insert(searchPaths.begin(), ".");  // Add current directory first

    for (const auto& path : searchPaths) {
        std::string fullPath = path + "/config.json";
        std::ifstream f(fullPath);
        if (f.good()) {
            return fullPath;
        }
    }

    return "";  // Not found
}
