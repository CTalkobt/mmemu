#include "vice_snapshot.h"
#include "libcore/main/icore.h"
#include "libmem/main/ibus.h"
#include "libdebug/main/debug_context.h"
#include "libdevices/main/io_registry.h"
#include <fstream>
#include <cstring>
#include <spdlog/spdlog.h>

std::string ViceSnapshotLoader::s_lastError;
std::string ViceSnapshotSaver::s_lastError;

// Utility to read little-endian integers
static uint16_t readU16LE(const uint8_t* data) {
    return data[0] | (data[1] << 8);
}

static uint32_t readU32LE(const uint8_t* data) {
    return data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
}

// Utility to write little-endian integers
static void writeU16LE(uint8_t* data, uint16_t val) {
    data[0] = val & 0xFF;
    data[1] = (val >> 8) & 0xFF;
}

static void writeU32LE(uint8_t* data, uint32_t val) {
    data[0] = val & 0xFF;
    data[1] = (val >> 8) & 0xFF;
    data[2] = (val >> 16) & 0xFF;
    data[3] = (val >> 24) & 0xFF;
}

bool ViceSnapshotLoader::isViceSnapshot(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        return false;
    }

    char magic[16] = {0};
    file.read(magic, 16);
    return std::string(magic) == "VICE Snapshot";
}

bool ViceSnapshotLoader::load(const std::string& filename, ICore* cpu, IBus* bus,
                               DebugContext* dbg, IORegistry* io_registry) {
    if (!cpu || !bus) {
        s_lastError = "CPU or bus not available";
        return false;
    }

    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) {
        s_lastError = "Cannot open file: " + filename;
        return false;
    }

    size_t fileSize = file.tellg();
    file.seekg(0);

    if (fileSize < 40) {
        s_lastError = "File too small for VICE snapshot header";
        return false;
    }

    // Read file header (40 bytes)
    FileHeader header;
    char headerBuf[40];
    file.read(headerBuf, 40);

    std::memcpy(header.magic, headerBuf, 16);
    header.version = readU32LE(reinterpret_cast<uint8_t*>(headerBuf + 16));
    std::memcpy(header.machine, headerBuf + 20, 16);

    if (std::string(header.magic) != "VICE Snapshot") {
        s_lastError = "Invalid VICE snapshot magic";
        return false;
    }

    spdlog::info("[VICE Loader] Format version: {}, Machine: {}",
                 header.version, header.machine);

    // Read modules
    while (file.tellg() < static_cast<std::streampos>(fileSize)) {
        ModuleHeader mod;
        char modBuf[24];
        file.read(modBuf, 24);

        if (file.gcount() < 24) {
            break; // End of file or incomplete module header
        }

        std::memcpy(mod.name, modBuf, 16);
        mod.version = readU32LE(reinterpret_cast<uint8_t*>(modBuf + 16));
        mod.length = readU32LE(reinterpret_cast<uint8_t*>(modBuf + 20));

        if (file.tellg() + static_cast<std::streampos>(mod.length) > static_cast<std::streampos>(fileSize)) {
            s_lastError = "Module data exceeds file size";
            return false;
        }

        std::vector<uint8_t> modData(mod.length);
        file.read(reinterpret_cast<char*>(modData.data()), mod.length);

        std::string modName(mod.name);
        spdlog::info("[VICE Loader] Found module: {} (v{}, {} bytes)",
                     modName, mod.version, mod.length);

        if (modName == "CPU") {
            if (!loadCpuModule(modData, cpu)) {
                return false;
            }
        } else if (modName == "RAM") {
            if (!loadRamModule(modData, bus)) {
                return false;
            }
        } else if (modName == "VIC2" && io_registry) {
            loadVic2Module(modData, io_registry);
        } else if (modName == "SID" && io_registry) {
            loadSidModule(modData, io_registry);
        } else if (modName == "CIA1" && io_registry) {
            loadCia1Module(modData, io_registry);
        } else if (modName == "CIA2" && io_registry) {
            loadCia2Module(modData, io_registry);
        }
    }

    return true;
}

bool ViceSnapshotLoader::loadCpuModule(const std::vector<uint8_t>& data, ICore* cpu) {
    if (data.size() < 7) {
        s_lastError = "CPU module too small";
        return false;
    }

    uint16_t pc = readU16LE(data.data());
    uint8_t a = data[2];
    uint8_t x = data[3];
    uint8_t y = data[4];
    uint8_t sp = data[5];
    uint8_t p = data[6];

    cpu->setPc(pc);

    // Set registers using the register API
    int aIdx = cpu->regIndexByName("A");
    int xIdx = cpu->regIndexByName("X");
    int yIdx = cpu->regIndexByName("Y");
    int pIdx = cpu->regIndexByName("P");

    if (aIdx >= 0) cpu->regWrite(aIdx, a);
    if (xIdx >= 0) cpu->regWrite(xIdx, x);
    if (yIdx >= 0) cpu->regWrite(yIdx, y);
    if (pIdx >= 0) cpu->regWrite(pIdx, p);

    // SP handling (typically register SP)
    int spIdx = cpu->regIndexByName("SP");
    if (spIdx >= 0) {
        cpu->regWrite(spIdx, sp);
    }

    spdlog::info("[VICE Loader] Restored CPU: PC=${:04X} A=${:02X} X=${:02X} Y=${:02X} SP=${:02X} P=${:02X}",
                 pc, a, x, y, sp, p);
    return true;
}

bool ViceSnapshotLoader::loadRamModule(const std::vector<uint8_t>& data, IBus* bus) {
    if (data.size() < 4) {
        s_lastError = "RAM module header incomplete";
        return false;
    }

    uint32_t ramSize = readU32LE(data.data());
    size_t dataSize = data.size() - 4;

    if (dataSize < ramSize) {
        spdlog::warn("[VICE Loader] RAM module incomplete: expected {}, got {}",
                     ramSize, dataSize);
    }

    // Write RAM contents
    for (size_t i = 0; i < std::min(static_cast<size_t>(ramSize), dataSize); i++) {
        bus->write8(i, data[4 + i]);
    }

    spdlog::info("[VICE Loader] Restored {} bytes of RAM", std::min(static_cast<size_t>(ramSize), dataSize));
    return true;
}

bool ViceSnapshotLoader::loadVic2Module(const std::vector<uint8_t>& data, IORegistry* io_registry) {
    spdlog::info("[VICE Loader] VIC2 module ({} registers) - device support pending", data.size());
    return true;
}

bool ViceSnapshotLoader::loadSidModule(const std::vector<uint8_t>& data, IORegistry* io_registry) {
    spdlog::info("[VICE Loader] SID module ({} registers) - device support pending", data.size());
    return true;
}

bool ViceSnapshotLoader::loadCia1Module(const std::vector<uint8_t>& data, IORegistry* io_registry) {
    spdlog::info("[VICE Loader] CIA1 module ({} registers) - device support pending", data.size());
    return true;
}

bool ViceSnapshotLoader::loadCia2Module(const std::vector<uint8_t>& data, IORegistry* io_registry) {
    spdlog::info("[VICE Loader] CIA2 module ({} registers) - device support pending", data.size());
    return true;
}

// SAVER IMPLEMENTATION

bool ViceSnapshotSaver::save(const std::string& filename, const std::string& machine_type,
                             ICore* cpu, IBus* bus, DebugContext* dbg, IORegistry* io_registry) {
    if (!cpu || !bus) {
        s_lastError = "CPU or bus not available";
        return false;
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file) {
        s_lastError = "Cannot create file: " + filename;
        return false;
    }

    // Write file header
    FileHeader header;
    std::memset(&header, 0, sizeof(header));
    std::strncpy(header.magic, "VICE Snapshot", 15);
    header.version = 1;
    std::strncpy(header.machine, machine_type.c_str(), 15);

    char headerBuf[40];
    std::memset(headerBuf, 0, 40);
    std::memcpy(headerBuf, header.magic, 16);
    writeU32LE(reinterpret_cast<uint8_t*>(headerBuf + 16), header.version);
    std::memcpy(headerBuf + 20, header.machine, 16);
    file.write(headerBuf, 40);

    // Create and write CPU module
    auto cpuMod = createCpuModule(cpu);
    {
        ModuleHeader mod;
        std::memset(&mod, 0, sizeof(mod));
        std::strncpy(mod.name, "CPU", 15);
        mod.version = 1;
        mod.length = cpuMod.size();

        char modBuf[24];
        std::memset(modBuf, 0, 24);
        std::memcpy(modBuf, mod.name, 16);
        writeU32LE(reinterpret_cast<uint8_t*>(modBuf + 16), mod.version);
        writeU32LE(reinterpret_cast<uint8_t*>(modBuf + 20), mod.length);
        file.write(modBuf, 24);
        file.write(reinterpret_cast<char*>(cpuMod.data()), cpuMod.size());
    }

    // Create and write RAM module
    auto ramMod = createRamModule(bus);
    {
        ModuleHeader mod;
        std::memset(&mod, 0, sizeof(mod));
        std::strncpy(mod.name, "RAM", 15);
        mod.version = 1;
        mod.length = ramMod.size();

        char modBuf[24];
        std::memset(modBuf, 0, 24);
        std::memcpy(modBuf, mod.name, 16);
        writeU32LE(reinterpret_cast<uint8_t*>(modBuf + 16), mod.version);
        writeU32LE(reinterpret_cast<uint8_t*>(modBuf + 20), mod.length);
        file.write(modBuf, 24);
        file.write(reinterpret_cast<char*>(ramMod.data()), ramMod.size());
    }

    size_t totalSize = 40 + (24 + cpuMod.size()) + (24 + ramMod.size());
    spdlog::info("[VICE Saver] Snapshot saved to {} ({} bytes)", filename, totalSize);
    return true;
}

std::vector<uint8_t> ViceSnapshotSaver::createCpuModule(ICore* cpu) {
    std::vector<uint8_t> module(7);

    uint16_t pc = cpu->pc();
    writeU16LE(module.data(), pc);

    // Get registers by name
    int aIdx = cpu->regIndexByName("A");
    int xIdx = cpu->regIndexByName("X");
    int yIdx = cpu->regIndexByName("Y");
    int spIdx = cpu->regIndexByName("SP");
    int pIdx = cpu->regIndexByName("P");

    module[2] = (aIdx >= 0) ? cpu->regRead(aIdx) : 0;
    module[3] = (xIdx >= 0) ? cpu->regRead(xIdx) : 0;
    module[4] = (yIdx >= 0) ? cpu->regRead(yIdx) : 0;
    module[5] = (spIdx >= 0) ? cpu->regRead(spIdx) : 0;
    module[6] = (pIdx >= 0) ? cpu->regRead(pIdx) : 0;

    return module;
}

std::vector<uint8_t> ViceSnapshotSaver::createRamModule(IBus* bus) {
    // 64K RAM + 4-byte header
    std::vector<uint8_t> module(65540);

    // Header: RAM size (64K)
    writeU32LE(module.data(), 65536);

    // Read entire address space
    for (uint32_t i = 0; i < 65536; i++) {
        module[4 + i] = bus->peek8(i);
    }

    return module;
}

std::vector<uint8_t> ViceSnapshotSaver::createVic2Module(IORegistry* io_registry) {
    std::vector<uint8_t> module;
    // Placeholder for VIC2 state
    return module;
}

std::vector<uint8_t> ViceSnapshotSaver::createSidModule(IORegistry* io_registry) {
    std::vector<uint8_t> module;
    // Placeholder for SID state
    return module;
}

std::vector<uint8_t> ViceSnapshotSaver::createCia1Module(IORegistry* io_registry) {
    std::vector<uint8_t> module;
    // Placeholder for CIA1 state
    return module;
}

std::vector<uint8_t> ViceSnapshotSaver::createCia2Module(IORegistry* io_registry) {
    std::vector<uint8_t> module;
    // Placeholder for CIA2 state
    return module;
}
