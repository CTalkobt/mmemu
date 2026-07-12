#include "debug_metadata.h"
#include <sstream>
#include <algorithm>
#include <cctype>

static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

bool DebugMetadataParser::isDebugMetadataLine(const std::string& line) {
    std::string trimmed = trim(line);
    return trimmed.find("; .debug_var:") == 0 || trimmed.find("; .debug_struct:") == 0;
}

bool DebugMetadataParser::parseAttribute(const std::string& attr, std::string& key, std::string& value) {
    size_t eq = attr.find('=');
    if (eq == std::string::npos) {
        key = attr;
        value = "";
        return true;
    }
    key = trim(attr.substr(0, eq));
    value = trim(attr.substr(eq + 1));
    return true;
}

DebugScope DebugMetadataParser::parseScopeString(const std::string& scope) {
    if (scope == "parameter") return DebugScope::PARAMETER;
    if (scope == "local") return DebugScope::LOCAL;
    if (scope == "global") return DebugScope::GLOBAL;
    return DebugScope::LOCAL;  // Default
}

bool DebugMetadataParser::parseVariableLine(const std::string& line, DebugVariable& var) {
    std::string trimmed = trim(line);

    // Remove comment prefix
    if (trimmed.substr(0, 2) == "; ") {
        trimmed = trimmed.substr(2);
    }

    // Check for .debug_var: prefix
    if (trimmed.substr(0, 11) != ".debug_var:") {
        return false;
    }

    trimmed = trimmed.substr(11);  // Remove ".debug_var:"
    trimmed = trim(trimmed);

    // Parse: function_name var_name [attributes...]
    std::istringstream iss(trimmed);
    std::string token;

    // Function name
    if (!(iss >> token)) return false;
    var.functionName = token;

    // Variable name
    if (!(iss >> token)) return false;
    var.internalName = token;

    // Parse attributes
    var.displayName = var.internalName;  // Default to internal name
    var.scope = DebugScope::LOCAL;
    var.srcLine = -1;

    while (iss >> token) {
        std::string key, value;
        if (!parseAttribute(token, key, value)) continue;

        if (key == "offset") {
            var.offset = std::stoul(value);
        } else if (key == "size") {
            var.size = std::stoul(value);
        } else if (key == "type") {
            var.type = value;
        } else if (key == "scope") {
            var.scope = parseScopeString(value);
        } else if (key == "src_line") {
            var.srcLine = std::stoi(value);
        } else if (key == "src_file") {
            var.srcFile = value;
        } else if (key == "name") {
            var.displayName = value;
        }
    }

    return true;
}

bool DebugMetadataParser::parseStructLine(const std::string& line, std::vector<DebugStructField>& fields) {
    std::string trimmed = trim(line);

    // Remove comment prefix
    if (trimmed.substr(0, 2) == "; ") {
        trimmed = trimmed.substr(2);
    }

    // Check for .debug_struct: prefix
    if (trimmed.substr(0, 14) != ".debug_struct:") {
        return false;
    }

    trimmed = trimmed.substr(14);  // Remove ".debug_struct:"
    trimmed = trim(trimmed);

    // Parse: struct_name field1=... field2=...
    std::istringstream iss(trimmed);
    std::string token;

    // Struct name
    if (!(iss >> token)) return false;
    std::string structName = token;

    // Parse field definitions
    while (iss >> token) {
        DebugStructField field;
        field.structName = structName;

        // Parse field definition: fieldname:offset:size:type
        size_t colonPos = token.find(':');
        if (colonPos == std::string::npos) continue;

        field.fieldName = token.substr(0, colonPos);
        std::string rest = token.substr(colonPos + 1);

        // Parse offset:size:type
        size_t colon2 = rest.find(':');
        if (colon2 == std::string::npos) continue;

        field.fieldOffset = std::stoul(rest.substr(0, colon2));
        rest = rest.substr(colon2 + 1);

        colon2 = rest.find(':');
        if (colon2 == std::string::npos) continue;

        field.fieldSize = std::stoul(rest.substr(0, colon2));
        field.fieldType = rest.substr(colon2 + 1);

        fields.push_back(field);
    }

    return true;
}

void DebugMetadataRegistry::addVariable(const DebugVariable& var) {
    m_variables.push_back(var);
}

void DebugMetadataRegistry::addStructFields(const std::vector<DebugStructField>& fields) {
    if (fields.empty()) return;

    std::string structName = fields[0].structName;
    for (const auto& field : fields) {
        m_structs[structName].push_back(field);
    }
}

std::vector<DebugVariable> DebugMetadataRegistry::getVariablesForFunction(const std::string& functionName) const {
    std::vector<DebugVariable> result;
    for (const auto& var : m_variables) {
        if (var.functionName == functionName) {
            result.push_back(var);
        }
    }
    return result;
}

std::vector<DebugStructField> DebugMetadataRegistry::getStructFields(const std::string& structName) const {
    auto it = m_structs.find(structName);
    if (it != m_structs.end()) {
        return it->second;
    }
    return {};
}
