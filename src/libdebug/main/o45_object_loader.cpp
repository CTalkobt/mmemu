#include "o45_object_loader.h"
#include <fstream>
#include <cstring>
#include <iostream>

bool O45ObjectLoader::checkBounds(const std::vector<uint8_t>& data, size_t offset, size_t needed) {
    return offset + needed <= data.size();
}

bool O45ObjectLoader::readOption(const std::vector<uint8_t>& fileData, size_t& offset,
                                 uint8_t& outType, std::vector<uint8_t>& outData) {
    outData.clear();

    // Read option type
    if (!checkBounds(fileData, offset, 1)) {
        return false;
    }
    outType = fileData[offset++];

    // OPT_END marks end of options
    if (outType == OPT_END) {
        return true;
    }

    // Read payload length (variable-length encoding)
    if (!checkBounds(fileData, offset, 1)) {
        return false;
    }
    uint8_t len = fileData[offset++];

    // Read payload
    if (!checkBounds(fileData, offset, len)) {
        return false;
    }
    outData.insert(outData.end(), fileData.begin() + offset, fileData.begin() + offset + len);
    offset += len;

    return true;
}

bool O45ObjectLoader::loadDebugSymbols(const std::string& path,
                                       std::vector<uint8_t>& outDebugData) {
    outDebugData.clear();

    // Read entire file
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        std::cerr << "[O45ObjectLoader] Failed to open " << path << std::endl;
        return false;
    }

    // Read file into memory
    f.seekg(0, std::ios::end);
    size_t fileSize = f.tellg();
    f.seekg(0, std::ios::beg);

    std::vector<uint8_t> fileData(fileSize);
    f.read((char*)fileData.data(), fileSize);
    f.close();

    // Verify .o45 file header
    // Format: marker1(1) marker2(1) magic(3) version(1) ...
    size_t offset = 0;
    if (!checkBounds(fileData, offset, 5)) {
        std::cerr << "[O45ObjectLoader] File too small for .o45 header" << std::endl;
        return false;
    }

    if (fileData[offset] != O45_MARKER1 || fileData[offset + 1] != O45_MARKER2) {
        std::cerr << "[O45ObjectLoader] Invalid .o45 markers" << std::endl;
        return false;
    }
    offset += 2;

    if (fileData[offset] != 'o' || fileData[offset + 1] != '4' || fileData[offset + 2] != '5') {
        std::cerr << "[O45ObjectLoader] Invalid .o45 magic number" << std::endl;
        return false;
    }
    offset += 3;

    // Skip version byte
    offset++;

    // Skip mode word (2 bytes)
    if (!checkBounds(fileData, offset, 2)) {
        std::cerr << "[O45ObjectLoader] File too small for mode word" << std::endl;
        return false;
    }
    offset += 2;

    // Skip most of the fixed header (we need to get to options section)
    // .o45 fixed header is 41 bytes total
    // So far we've read 8 bytes (marker1, marker2, magic, version, mode)
    // We need to skip 33 more bytes to get to options
    if (!checkBounds(fileData, offset, 33)) {
        std::cerr << "[O45ObjectLoader] File too small for complete header" << std::endl;
        return false;
    }
    offset += 33;

    // Now we're at the options section
    // Read options until we find OPT_DEBUG_SYMBOLS or OPT_END
    while (offset < fileData.size()) {
        uint8_t optType = 0;
        std::vector<uint8_t> optData;

        if (!readOption(fileData, offset, optType, optData)) {
            std::cerr << "[O45ObjectLoader] Error reading option at offset " << offset << std::endl;
            return false;
        }

        // Check for end of options
        if (optType == OPT_END) {
            // No debug symbols found
            return false;
        }

        // Check if this is the debug symbols option
        if (optType == OPT_DEBUG_SYMBOLS) {
            outDebugData = optData;
            return true;
        }
    }

    // No debug symbols option found
    return false;
}
