#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

class ICore;
class IBus;
class DebugContext;
class IORegistry;

/// VICE snapshot file format support
/// Enables loading/saving emulator state in VICE-compatible .vsf format
class ViceSnapshotLoader {
public:
    /// Load a VICE snapshot file and restore state
    /// Returns true on success, false on error (check getLastError())
    static bool load(const std::string& filename, ICore* cpu, IBus* bus,
                     DebugContext* dbg, IORegistry* io_registry);

    /// Check if a file is a valid VICE snapshot
    static bool isViceSnapshot(const std::string& filename);

    /// Get last error message
    static const std::string& getLastError() { return s_lastError; }

private:
    static std::string s_lastError;

    struct ModuleHeader {
        char name[16];
        uint32_t version;
        uint32_t length;
    };

    struct FileHeader {
        char magic[16];
        uint32_t version;
        char machine[16];
    };

    static bool loadCpuModule(const std::vector<uint8_t>& data, ICore* cpu);
    static bool loadRamModule(const std::vector<uint8_t>& data, IBus* bus);
    static bool loadVic2Module(const std::vector<uint8_t>& data, IORegistry* io_registry);
    static bool loadSidModule(const std::vector<uint8_t>& data, IORegistry* io_registry);
    static bool loadCia1Module(const std::vector<uint8_t>& data, IORegistry* io_registry);
    static bool loadCia2Module(const std::vector<uint8_t>& data, IORegistry* io_registry);
};

/// Save emulator state to VICE snapshot format
class ViceSnapshotSaver {
public:
    /// Save current machine state to VICE snapshot file
    /// Returns true on success, false on error (check getLastError())
    static bool save(const std::string& filename, const std::string& machine_type,
                     ICore* cpu, IBus* bus, DebugContext* dbg, IORegistry* io_registry);

    /// Get last error message
    static const std::string& getLastError() { return s_lastError; }

private:
    static std::string s_lastError;

    struct ModuleHeader {
        char name[16];
        uint32_t version;
        uint32_t length;
    };

    struct FileHeader {
        char magic[16];
        uint32_t version;
        char machine[16];
    };

    static std::vector<uint8_t> createCpuModule(ICore* cpu);
    static std::vector<uint8_t> createRamModule(IBus* bus);
    static std::vector<uint8_t> createVic2Module(IORegistry* io_registry);
    static std::vector<uint8_t> createSidModule(IORegistry* io_registry);
    static std::vector<uint8_t> createCia1Module(IORegistry* io_registry);
    static std::vector<uint8_t> createCia2Module(IORegistry* io_registry);
};
