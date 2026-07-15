#pragma once

#include <cstdint>
#include <vector>
#include <string>

/**
 * Minimal .o45 object file reader for extracting debug metadata.
 *
 * Reads just enough of the .o45 format to find and extract the OPT_DEBUG_SYMBOLS option.
 * Does not perform full object file parsing or linking.
 */
class O45ObjectLoader {
public:
    /**
     * Load debug symbols from an .o45 file.
     * Extracts OPT_DEBUG_SYMBOLS option data if present.
     *
     * @param path File path to .o45 object file
     * @param outDebugData Output buffer for debug symbol metadata
     * @return true if file was read and debug data extracted successfully
     */
    static bool loadDebugSymbols(const std::string& path,
                                 std::vector<uint8_t>& outDebugData);

private:
    // .o45 file format constants
    static constexpr uint8_t O45_MARKER1 = 0x01;
    static constexpr uint8_t O45_MARKER2 = 0x00;
    static constexpr uint8_t O45_MAGIC[3] = {0x6F, 0x34, 0x35};  // "o45"
    static constexpr uint8_t OPT_END = 0x00;
    static constexpr uint8_t OPT_DEBUG_SYMBOLS = 0x12;

    // Helper: Read option record from file
    static bool readOption(const std::vector<uint8_t>& fileData, size_t& offset,
                          uint8_t& outType, std::vector<uint8_t>& outData);

    // Helper: Check file bounds
    static bool checkBounds(const std::vector<uint8_t>& data, size_t offset, size_t needed);
};
