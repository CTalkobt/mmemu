#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "libtoolchain/main/iassembler.h"

/**
 * Global simulator configuration.
 * Loads from ./config.json or ~/.local/share/mmsim/config.json.
 * Provides tool configuration (assemblers, etc.).
 */
class SimConfig {
public:
    static SimConfig& instance();

    /** Load configuration from disk. Called at startup from CLI/MCP main(). */
    void load();

    /** Get assembler configuration by name (e.g., "ca45", "kickAssembler"). */
    AssemblerConfig assemblerConfig(const std::string& name) const;

private:
    SimConfig() = default;
    nlohmann::json m_root;

    /** Search for config.json in PathUtil data paths. */
    std::string findConfigFile() const;
};
