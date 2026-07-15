#pragma once

#include "libtoolchain/main/variable_symbol.h"
#include <cstdint>
#include <vector>
#include <string>

/**
 * Parser for OPT_DEBUG_SYMBOLS metadata from .o45 object files.
 *
 * Format (binary):
 *   procCount (uint16 LE)
 *   For each procedure:
 *     - name (null-terminated string)
 *     - paramCount (uint8)
 *     - For each parameter:
 *       * name (null-terminated string)
 *       * offset (int16 LE, frame-relative)
 *     - terminator (0xFF)
 */
class O45SymbolParser {
public:
    /**
     * Parse debug symbols from raw OPT_DEBUG_SYMBOLS option data.
     * Returns parsed variables indexed by function name.
     */
    static bool parse(const std::vector<uint8_t>& data,
                      std::map<std::string, std::vector<VariableSymbol>>& outVariables);

    /**
     * Parse debug symbols and populate a VariableSymbolTable.
     * Returns true if parsing succeeded.
     */
    static bool populateTable(const std::vector<uint8_t>& data,
                              VariableSymbolTable& table);

private:
    // Helper to read null-terminated string from buffer
    static bool readString(const std::vector<uint8_t>& data, size_t& offset,
                          std::string& outStr);

    // Helper to read little-endian uint16
    static uint16_t readUint16LE(const std::vector<uint8_t>& data, size_t& offset);

    // Helper to read signed int16
    static int16_t readInt16LE(const std::vector<uint8_t>& data, size_t& offset);

    // Helper to read uint8
    static uint8_t readUint8(const std::vector<uint8_t>& data, size_t& offset);

    // Check if offset is within bounds
    static bool checkBounds(const std::vector<uint8_t>& data, size_t offset);
};
