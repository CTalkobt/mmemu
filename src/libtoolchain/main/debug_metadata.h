#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>

/**
 * Debug metadata format specification for mmemu
 *
 * Format: Emitted as assembly comments from cc45 compiler
 * ; .debug_var: function_name var_name offset=N size=N type=TYPE scope=SCOPE [src_line=N] [src_file=FILE] [name=DISPLAY_NAME]
 *
 * Example:
 * ; .debug_var: _make_point @_p_x offset=10 size=2 type=int scope=parameter src_line=12 name=p.x
 * ; .debug_var: _make_point __vr0 offset=4 size=2 type=I16 scope=local name=x_value
 * ; .debug_struct: Point x offset=0 size=2 type=int y offset=2 size=2 type=int
 */

enum class DebugScope {
    PARAMETER,
    LOCAL,
    GLOBAL
};

struct DebugVariable {
    std::string functionName;      // Function this variable belongs to
    std::string internalName;      // Internal/compiler name (@_p_x, __vr0)
    std::string displayName;       // User-friendly name (x, y, x_value)
    uint32_t offset;               // Frame offset in bytes
    uint32_t size;                 // Size in bytes
    std::string type;              // Type string (int, I16, struct_Point)
    DebugScope scope;              // Parameter, local, or global
    int srcLine;                   // Source line number (-1 if unknown)
    std::string srcFile;           // Source file name

    DebugVariable()
        : offset(0), size(0), scope(DebugScope::LOCAL), srcLine(-1) {}
};

struct DebugStructField {
    std::string structName;        // Name of struct type
    std::string fieldName;         // Field name
    uint32_t fieldOffset;          // Offset within struct
    uint32_t fieldSize;            // Field size in bytes
    std::string fieldType;         // Field type

    DebugStructField()
        : fieldOffset(0), fieldSize(0) {}
};

/**
 * Parser for debug metadata emitted by cc45 compiler
 */
class DebugMetadataParser {
public:
    /**
     * Parse a debug variable line from assembly comment
     * Format: ; .debug_var: function_name var_name offset=N size=N type=TYPE scope=SCOPE [src_line=N] [src_file=FILE] [name=DISPLAY_NAME]
     */
    static bool parseVariableLine(const std::string& line, DebugVariable& var);

    /**
     * Parse a debug struct definition line
     * Format: ; .debug_struct: struct_name field1=... field2=...
     */
    static bool parseStructLine(const std::string& line, std::vector<DebugStructField>& fields);

    /**
     * Check if a line is a debug metadata line
     */
    static bool isDebugMetadataLine(const std::string& line);

private:
    static bool parseAttribute(const std::string& attr, std::string& key, std::string& value);
    static DebugScope parseScopeString(const std::string& scope);
};

/**
 * Registry for collected debug metadata from all sources
 */
class DebugMetadataRegistry {
public:
    void addVariable(const DebugVariable& var);
    void addStructFields(const std::vector<DebugStructField>& fields);

    const std::vector<DebugVariable>& getVariables() const { return m_variables; }
    const std::map<std::string, std::vector<DebugStructField>>& getStructs() const { return m_structs; }

    std::vector<DebugVariable> getVariablesForFunction(const std::string& functionName) const;
    std::vector<DebugStructField> getStructFields(const std::string& structName) const;

    void clear() {
        m_variables.clear();
        m_structs.clear();
    }

private:
    std::vector<DebugVariable> m_variables;
    std::map<std::string, std::vector<DebugStructField>> m_structs;
};
