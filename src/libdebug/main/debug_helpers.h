#pragma once

#include <string>
#include <vector>
#include <cstdint>

class DebugContext;
class IBus;

// Structured data for variable display (frontend-agnostic)
struct VariableInfo {
    std::string name;           // Variable name
    std::string displayName;    // Human-readable name
    std::string type;           // Type string (int16, uint8, etc.)
    uint32_t address;           // Memory address
    uint32_t size;              // Size in bytes
    std::vector<uint8_t> value; // Raw bytes read from memory
    int sourceLine;             // Source line (-1 if unknown)
    bool isParameter;           // True if function parameter
    bool isFrameRelative;       // True if frame-relative offset

    VariableInfo() : address(0), size(0), sourceLine(-1),
                     isParameter(false), isFrameRelative(false) {}
};

// Structured data for locals display
struct LocalVariablesInfo {
    bool hasVariables;
    std::vector<VariableInfo> variables;  // Sorted by address

    LocalVariablesInfo() : hasVariables(false) {}
};

// Structured data for frame layout
struct FrameLayoutInfo {
    std::vector<VariableInfo> variables;  // Sorted by address
    uint32_t totalFrameSize;

    FrameLayoutInfo() : totalFrameSize(0) {}
};

// Helper functions that return structured data
namespace DebugHelpers {
    /**
     * Get local variables information.
     * Returns a structure with all local variables and their current values.
     * Frontends can format this as needed (text, JSON, GUI, etc.)
     */
    LocalVariablesInfo getLocalVariablesInfo(DebugContext* dbg, IBus* bus);

    /**
     * Get frame layout information.
     * Returns structure with all variables sorted by address for layout visualization.
     */
    FrameLayoutInfo getFrameLayoutInfo(DebugContext* dbg, IBus* bus);

    /**
     * Find and get info for a single variable by name.
     * Returns empty VariableInfo if not found.
     */
    VariableInfo getVariableInfo(DebugContext* dbg, IBus* bus,
                                 const std::string& varName);

    /**
     * Format a variable value as a human-readable string.
     * Used by frontends to display values.
     */
    std::string formatVariableValue(const VariableInfo& var);

    /**
     * Get address space size from bus config.
     */
    uint32_t getMaxAddress(IBus* bus);
}
