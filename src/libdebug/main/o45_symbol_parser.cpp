#include "o45_symbol_parser.h"
#include <cstring>
#include <iostream>

bool O45SymbolParser::checkBounds(const std::vector<uint8_t>& data, size_t offset) {
    return offset < data.size();
}

bool O45SymbolParser::readString(const std::vector<uint8_t>& data, size_t& offset,
                                  std::string& outStr) {
    outStr.clear();
    while (checkBounds(data, offset)) {
        uint8_t c = data[offset++];
        if (c == 0) return true;
        outStr += (char)c;
    }
    return false;  // Unterminated string
}

uint16_t O45SymbolParser::readUint16LE(const std::vector<uint8_t>& data, size_t& offset) {
    if (!checkBounds(data, offset + 1)) return 0;
    uint16_t val = data[offset] | (data[offset + 1] << 8);
    offset += 2;
    return val;
}

int16_t O45SymbolParser::readInt16LE(const std::vector<uint8_t>& data, size_t& offset) {
    return (int16_t)readUint16LE(data, offset);
}

uint8_t O45SymbolParser::readUint8(const std::vector<uint8_t>& data, size_t& offset) {
    if (!checkBounds(data, offset)) return 0;
    return data[offset++];
}

bool O45SymbolParser::parse(const std::vector<uint8_t>& data,
                            std::map<std::string, std::vector<VariableSymbol>>& outVariables) {
    outVariables.clear();

    if (data.empty()) return false;

    size_t offset = 0;

    // Read procedure count
    uint16_t procCount = readUint16LE(data, offset);

    for (uint16_t i = 0; i < procCount; i++) {
        // Read function name
        std::string funcName;
        if (!readString(data, offset, funcName)) {
            std::cerr << "[O45SymbolParser] Failed to read function name for procedure " << i << std::endl;
            return false;
        }

        // Read parameter count
        uint8_t paramCount = readUint8(data, offset);

        std::vector<VariableSymbol> params;

        // Read each parameter
        for (uint8_t p = 0; p < paramCount; p++) {
            std::string paramName;
            if (!readString(data, offset, paramName)) {
                std::cerr << "[O45SymbolParser] Failed to read parameter name" << std::endl;
                return false;
            }

            int16_t paramOffset = readInt16LE(data, offset);

            // Create variable symbol for this parameter
            VariableSymbol var;
            var.name = paramName;
            var.functionName = funcName;
            var.displayName = paramName;
            var.address = (uint32_t)paramOffset;  // Frame-relative offset
            var.size = 2;  // Default to 16-bit (word) size
            var.type = VariableType::INT16;  // Default type
            var.isParameter = true;
            var.isFrameRelative = true;
            var.sourceLine = -1;

            params.push_back(var);
        }

        // Read terminator (0xFF)
        uint8_t terminator = readUint8(data, offset);
        if (terminator != 0xFF) {
            std::cerr << "[O45SymbolParser] Expected terminator 0xFF for procedure " << funcName
                      << " but got 0x" << std::hex << (int)terminator << std::endl;
            return false;
        }

        // Store parameters for this function
        outVariables[funcName] = params;
    }

    return true;
}

bool O45SymbolParser::populateTable(const std::vector<uint8_t>& data,
                                    VariableSymbolTable& table) {
    std::map<std::string, std::vector<VariableSymbol>> variables;

    if (!parse(data, variables)) {
        return false;
    }

    // Add all variables to the table
    for (const auto& [funcName, params] : variables) {
        for (const auto& var : params) {
            table.addVariable(funcName, var);
        }
    }

    return true;
}
