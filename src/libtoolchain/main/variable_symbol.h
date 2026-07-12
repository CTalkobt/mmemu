#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>

enum class VariableType {
    UNKNOWN,
    INT8, INT16, INT32,
    UINT8, UINT16, UINT32,
    CHAR,
    POINTER,
    STRUCT,
    ARRAY,
};

struct StructField {
    std::string name;
    uint32_t offset;  // offset within struct
    uint32_t size;
    VariableType type;
};

struct VariableSymbol {
    std::string name;        // e.g., "x", "p", "__vr0"
    std::string functionName; // e.g., "_make_point" - empty if global
    uint32_t address;        // Memory address or frame offset
    uint32_t size;           // Size in bytes
    VariableType type;
    int sourceLine;          // Source line where declared (-1 if unknown)
    bool isParameter;        // True if function parameter
    bool isFrameRelative;    // True if offset relative to frame pointer
    std::string displayName; // Human-readable name from source

    // For struct types
    std::vector<StructField> fields;

    VariableSymbol()
        : address(0), size(0), type(VariableType::UNKNOWN),
          sourceLine(-1), isParameter(false), isFrameRelative(false) {}
};

// Maps variable names to their metadata, organized by function scope
class VariableSymbolTable {
public:
    // Add a variable for a specific function
    void addVariable(const std::string& functionName, const VariableSymbol& var);

    // Add a global variable (functionName empty)
    void addGlobalVariable(const VariableSymbol& var);

    // Find a variable by name within a function scope
    const VariableSymbol* findVariable(const std::string& functionName,
                                       const std::string& varName) const;

    // Find a global variable by name
    const VariableSymbol* findGlobalVariable(const std::string& varName) const;

    // Get all variables for a function (including parameters)
    std::vector<const VariableSymbol*> getVariablesInFunction(
        const std::string& functionName) const;

    // Get all global variables
    std::vector<const VariableSymbol*> getGlobalVariables() const;

    // Clear all variables
    void clear();

    // Load from debug symbols file (.debug_info format)
    bool loadDebugInfo(const std::string& path);

private:
    // Function name -> { variable name -> VariableSymbol }
    std::map<std::string, std::map<std::string, VariableSymbol>> m_functionVariables;

    // Global variables
    std::map<std::string, VariableSymbol> m_globalVariables;
};

// Helper functions for type formatting
std::string formatVariableType(VariableType type);
std::string formatVariableValue(const VariableSymbol& var, const uint8_t* memory,
                               uint32_t memorySize);
