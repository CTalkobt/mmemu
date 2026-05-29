#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct BitField {
    std::string name;
    uint8_t     startBit;  // LSB position (0-7)
    uint8_t     width;     // number of bits
};

struct DeviceRegister {
    std::string name;
    uint32_t    offset;
    uint8_t     value;
    std::string description;
    std::vector<BitField> bitFields;
};

struct DeviceBitmap {
    std::string name;
    int width;
    int height;
    std::vector<uint32_t> pixels; // RGBA8888
};

struct DeviceInfo {
    std::string name;
    uint32_t    baseAddr;
    uint32_t    addrMask;
    std::vector<DeviceRegister> registers;
    std::vector<std::pair<std::string, std::string>> state;
    std::vector<std::pair<std::string, std::string>> dependencies;
    std::vector<DeviceBitmap> bitmaps;
};
