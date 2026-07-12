#include "variable_symbol.h"
#include "debug_metadata.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iomanip>

void VariableSymbolTable::addVariable(const std::string& functionName,
                                      const VariableSymbol& var) {
    if (functionName.empty()) {
        addGlobalVariable(var);
        return;
    }
    m_functionVariables[functionName][var.name] = var;
}

void VariableSymbolTable::addGlobalVariable(const VariableSymbol& var) {
    m_globalVariables[var.name] = var;
}

const VariableSymbol* VariableSymbolTable::findVariable(
    const std::string& functionName, const std::string& varName) const {
    auto funcIt = m_functionVariables.find(functionName);
    if (funcIt != m_functionVariables.end()) {
        auto varIt = funcIt->second.find(varName);
        if (varIt != funcIt->second.end()) {
            return &varIt->second;
        }
    }
    return nullptr;
}

const VariableSymbol* VariableSymbolTable::findGlobalVariable(
    const std::string& varName) const {
    auto it = m_globalVariables.find(varName);
    if (it != m_globalVariables.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<const VariableSymbol*> VariableSymbolTable::getVariablesInFunction(
    const std::string& functionName) const {
    std::vector<const VariableSymbol*> result;
    auto funcIt = m_functionVariables.find(functionName);
    if (funcIt != m_functionVariables.end()) {
        for (const auto& pair : funcIt->second) {
            result.push_back(&pair.second);
        }
    }
    return result;
}

std::vector<const VariableSymbol*> VariableSymbolTable::getGlobalVariables() const {
    std::vector<const VariableSymbol*> result;
    for (const auto& pair : m_globalVariables) {
        result.push_back(&pair.second);
    }
    return result;
}

void VariableSymbolTable::clear() {
    m_functionVariables.clear();
    m_globalVariables.clear();
}

bool VariableSymbolTable::loadDebugInfo(const std::string& path) {
    std::ifstream file(path);
    if (!file.good()) {
        return false;
    }

    std::string line;
    std::string currentFunction;

    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        // Trim whitespace
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);

        // Function scope marker: .func functionname
        if (line.find(".func ") == 0) {
            currentFunction = line.substr(6);
            continue;
        }

        // Variable entry: name address size type [isParam] [displayName]
        // Example: @_p_x 10 2 int16 1 "x"
        if (line[0] == '@' || std::isalpha(line[0]) || line[0] == '_') {
            std::istringstream iss(line);
            std::string name, typeStr;
            uint32_t address, size;
            int isParam = 0;
            std::string displayName;

            if (!(iss >> name >> std::hex >> address >> std::dec >> size >> typeStr)) {
                continue;
            }

            iss >> isParam;  // Optional: whether it's a parameter (0 or 1)

            // Optional: display name in quotes
            std::string rest;
            if (std::getline(iss, rest)) {
                size_t startQuote = rest.find('"');
                size_t endQuote = rest.rfind('"');
                if (startQuote != std::string::npos && endQuote != std::string::npos &&
                    startQuote < endQuote) {
                    displayName = rest.substr(startQuote + 1, endQuote - startQuote - 1);
                }
            }

            VariableSymbol var;
            var.name = name;
            var.address = address;
            var.size = size;
            var.isParameter = (isParam != 0);
            var.displayName = displayName.empty() ? name : displayName;
            var.isFrameRelative = (name[0] == '@');  // @ prefix = frame relative

            // Parse type
            if (typeStr == "int8") var.type = VariableType::INT8;
            else if (typeStr == "int16") var.type = VariableType::INT16;
            else if (typeStr == "int32") var.type = VariableType::INT32;
            else if (typeStr == "uint8") var.type = VariableType::UINT8;
            else if (typeStr == "uint16") var.type = VariableType::UINT16;
            else if (typeStr == "uint32") var.type = VariableType::UINT32;
            else if (typeStr == "char") var.type = VariableType::CHAR;
            else if (typeStr == "ptr") var.type = VariableType::POINTER;
            else var.type = VariableType::UNKNOWN;

            var.functionName = currentFunction;
            addVariable(currentFunction, var);
        }
    }

    return true;
}

static VariableType parseVariableType(const std::string& typeStr) {
    if (typeStr == "int8" || typeStr == "I8") return VariableType::INT8;
    if (typeStr == "int16" || typeStr == "I16") return VariableType::INT16;
    if (typeStr == "int32" || typeStr == "I32") return VariableType::INT32;
    if (typeStr == "uint8" || typeStr == "U8") return VariableType::UINT8;
    if (typeStr == "uint16" || typeStr == "U16") return VariableType::UINT16;
    if (typeStr == "uint32" || typeStr == "U32") return VariableType::UINT32;
    if (typeStr == "char") return VariableType::CHAR;
    if (typeStr == "ptr" || typeStr.find("*") != std::string::npos) return VariableType::POINTER;
    if (typeStr.find("[") != std::string::npos) return VariableType::ARRAY;
    if (typeStr.find("struct") != std::string::npos) return VariableType::STRUCT;
    return VariableType::UNKNOWN;
}

bool VariableSymbolTable::loadFromDebugMetadata(const std::string& path) {
    std::ifstream file(path);
    if (!file.good()) {
        return false;
    }

    std::string line;
    DebugMetadataRegistry registry;

    while (std::getline(file, line)) {
        if (!DebugMetadataParser::isDebugMetadataLine(line)) {
            continue;
        }

        // Try to parse as variable
        DebugVariable var;
        if (DebugMetadataParser::parseVariableLine(line, var)) {
            VariableSymbol sym;
            sym.name = var.internalName;
            sym.displayName = var.displayName;
            sym.address = var.offset;
            sym.size = var.size;
            sym.type = parseVariableType(var.type);
            sym.sourceLine = var.srcLine;
            sym.functionName = var.functionName;
            sym.isParameter = (var.scope == DebugScope::PARAMETER);
            sym.isFrameRelative = true;  // Debug metadata offsets are frame-relative

            addVariable(var.functionName, sym);
        }

        // Try to parse as struct definition
        std::vector<DebugStructField> fields;
        if (DebugMetadataParser::parseStructLine(line, fields)) {
            registry.addStructFields(fields);
        }
    }

    return true;
}

std::string formatVariableType(VariableType type) {
    switch (type) {
        case VariableType::INT8:    return "int8";
        case VariableType::INT16:   return "int16";
        case VariableType::INT32:   return "int32";
        case VariableType::UINT8:   return "uint8";
        case VariableType::UINT16:  return "uint16";
        case VariableType::UINT32:  return "uint32";
        case VariableType::CHAR:    return "char";
        case VariableType::POINTER: return "ptr";
        case VariableType::STRUCT:  return "struct";
        case VariableType::ARRAY:   return "array";
        default:                    return "unknown";
    }
}

std::string formatVariableValue(const VariableSymbol& var, const uint8_t* memory,
                               uint32_t memorySize) {
    if (var.address >= memorySize) {
        return "[address out of range]";
    }

    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');

    switch (var.type) {
        case VariableType::INT8:
        case VariableType::UINT8:
            if (var.address + 1 <= memorySize) {
                oss << "0x" << std::setw(2) << (int)memory[var.address];
            }
            break;

        case VariableType::INT16:
        case VariableType::UINT16:
            if (var.address + 2 <= memorySize) {
                uint16_t val = memory[var.address] | (memory[var.address + 1] << 8);
                oss << "0x" << std::setw(4) << val;
            }
            break;

        case VariableType::INT32:
        case VariableType::UINT32:
            if (var.address + 4 <= memorySize) {
                uint32_t val = memory[var.address] |
                              (memory[var.address + 1] << 8) |
                              (memory[var.address + 2] << 16) |
                              (memory[var.address + 3] << 24);
                oss << "0x" << std::setw(8) << val;
            }
            break;

        case VariableType::POINTER:
            if (var.address + 2 <= memorySize) {
                uint16_t val = memory[var.address] | (memory[var.address + 1] << 8);
                oss << "0x" << std::setw(4) << val;
            }
            break;

        default:
            oss << "[hex: ";
            for (uint32_t i = 0; i < var.size && var.address + i < memorySize; i++) {
                oss << std::setw(2) << (int)memory[var.address + i] << " ";
            }
            oss << "]";
            break;
    }

    return oss.str();
}
