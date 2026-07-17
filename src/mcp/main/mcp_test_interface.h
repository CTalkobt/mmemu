#pragma once

#include "minijson.h"
#include <string>
#include <cstdint>

/// Interface for invoking MCP tools from tests
/// These functions bridge between the test harness and the MCP tool system

namespace MCPTest {
    /// Invoke an MCP tool with given arguments
    /// @param toolName Name of the tool (e.g., "step_cpu", "read_memory")
    /// @param arguments JSON object with tool arguments
    /// @return JSON response with "text" field for success or "error" field for errors
    Json invokeTool(const std::string& toolName, const Json& arguments);

    /// Create a machine instance for testing
    /// @param machineType Type of machine (e.g., "c64", "vic20")
    /// @param instanceId Optional instance ID; if empty, generates one
    /// @return Instance ID if successful, empty string on error
    std::string createTestMachine(const std::string& machineType, const std::string& instanceId = "");

    /// Destroy a machine instance
    /// @param instanceId Instance ID to destroy
    /// @return true if successful
    bool destroyTestMachine(const std::string& instanceId);

    /// Read memory from a test machine
    /// @param instanceId Machine instance ID
    /// @param addr Address to read from
    /// @return Byte value at address
    uint8_t readMemory(const std::string& instanceId, uint32_t addr);

    /// Write memory on a test machine
    /// @param instanceId Machine instance ID
    /// @param addr Address to write to
    /// @param value Byte value to write
    void writeMemory(const std::string& instanceId, uint32_t addr, uint8_t value);

    /// Read a CPU register
    /// @param instanceId Machine instance ID
    /// @param regName Register name (e.g., "A", "X", "Y", "PC")
    /// @return Register value
    uint32_t readRegister(const std::string& instanceId, const std::string& regName);

    /// Write a CPU register
    /// @param instanceId Machine instance ID
    /// @param regName Register name (e.g., "A", "X", "Y", "PC")
    /// @param value Value to write
    void writeRegister(const std::string& instanceId, const std::string& regName, uint32_t value);

    /// Get current CPU cycle count
    /// @param instanceId Machine instance ID
    /// @return Cycle count
    uint64_t getCycles(const std::string& instanceId);

    /// List all active machine instances
    /// @return Array of instance IDs
    std::vector<std::string> listTestMachines();

    /// Clean up all test machines
    void cleanupAllTestMachines();
}
