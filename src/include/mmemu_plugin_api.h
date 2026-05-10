#pragma once

#ifdef __cplusplus
#include <cstdint>
extern "C" {
#else
#include <stdint.h>
#include <stdbool.h>
#endif

// mmemu public plugin ABI version — upper 16 bits: major, lower 16 bits: minor.
// Major version changes are breaking; minor version changes are additive.
#define MMEMU_PLUGIN_API_VERSION  0x00010000u
#define MMEMU_PLUGIN_API_MAJOR(v) ((v) >> 16)
#define MMEMU_PLUGIN_API_MINOR(v) ((v) & 0xFFFFu)

#ifdef __cplusplus
class ICore;
class IDisassembler;
class IAssembler;
class IOHandler;
class IImageLoader;
class ICartridgeHandler;
class ExecutionObserver;
struct MachineDescriptor;
#else
typedef struct ICore ICore;
typedef struct IDisassembler IDisassembler;
typedef struct IAssembler IAssembler;
typedef struct IOHandler IOHandler;
typedef struct IImageLoader IImageLoader;
typedef struct ICartridgeHandler ICartridgeHandler;
typedef struct ExecutionObserver ExecutionObserver;
typedef struct MachineDescriptor MachineDescriptor;
#endif

/**
 * Descriptor for a GUI pane provided by a plugin.
 */
struct PluginPaneInfo {
    const char* paneId;
    const char* displayName;
    const char* menuSection;        // e.g. "Tools", "View", or nullptr
    const char* const* machineIds;  // Null-terminated list, or nullptr for all
    void* (*createPane)(void* parentHandle, void* ctx);
    void  (*destroyPane)(void* paneHandle, void* ctx);
    void  (*refreshPane)(void* paneHandle, uint64_t cycles, void* ctx);
    /** Called after createPane when a matching machine is loaded. Nullable. */
    void  (*onMachineLoad)(void* paneHandle, MachineDescriptor* desc, void* ctx);
    void* ctx;
};

/**
 * Descriptor for a CLI command provided by a plugin.
 */
struct PluginCommandInfo {
    const char* name;
    const char* usage;
    int (*execute)(int argc, const char* const* argv, void* ctx);
    void* ctx;
};

/**
 * Descriptor for an MCP tool provided by a plugin.
 */
struct PluginMcpToolInfo {
    const char* toolName;
    const char* schemaJson;
    void (*handle)(const char* paramsJson, char** resultJson, void* ctx);
    void (*freeString)(char* s);
    void* ctx;
};

/**
 * Log levels.
 */
typedef enum {
    SIM_LOG_TRACE = 0,
    SIM_LOG_DEBUG = 1,
    SIM_LOG_INFO  = 2,
    SIM_LOG_WARN  = 3,
    SIM_LOG_ERROR = 4,
    SIM_LOG_CRITICAL = 5,
    SIM_LOG_OFF = 6
} SimLogLevel;

/**
 * Host services provided to plugins.
 */
struct SimPluginHostAPI {
    void (*log)(int level, const char* msg);
    /** Get or create a named logger. Handle should be used with logNamed. */
    void* (*getLogger)(const char* name);
    void (*logNamed)(void* logger, int level, const char* msg);
    
    // Registry access
    ICore*             (*createCore)(const char* name);
    MachineDescriptor* (*createMachine)(const char* machineId);
    IOHandler*         (*createDevice)(const char* name);
    IDisassembler*     (*createDisassembler)(const char* isa);
    IAssembler*        (*createAssembler)(const char* isa);
    IImageLoader*      (*findImageLoader)(const char* path);
    ICartridgeHandler* (*createCartridgeHandler)(const char* path);

    // Extension points
    void (*registerPane)(const struct PluginPaneInfo* info);
    void (*registerCommand)(const struct PluginCommandInfo* info);
    void (*registerMcpTool)(const struct PluginMcpToolInfo* info);
    void (*registerObserver)(ExecutionObserver* obs);
};

/**
 * Descriptor for a CPU core provided by a plugin.
 */
struct CorePluginInfo {
    const char* name;           // e.g. "6502", "Z80"
    const char* variant;        // e.g. "NMOS", "CMOS"
    const char* licenseClass;   // "open", "commercial"
    ICore* (*create)(void);
};

/**
 * Descriptor for a toolchain provided by a plugin.
 */
struct ToolchainPluginInfo {
    const char* isa;            // e.g. "6502"
    const char* assemblerName;  // e.g. "ca45", "kickAssembler"; nullptr to skip name-based registration
    IDisassembler* (*createDisassembler)(void);
    IAssembler*    (*createAssembler)(void);
};

/**
 * Descriptor for an I/O device provided by a plugin.
 */
struct DevicePluginInfo {
    const char* name;
    IOHandler* (*create)(void);
};

/**
 * Descriptor for a machine preset provided by a plugin.
 */
struct MachinePluginInfo {
    const char* machineId;
    MachineDescriptor* (*create)(void);
    const char* description;  // Human-readable description (optional, may be nullptr)
};

/**
 * Descriptor for an image loader provided by a plugin.
 */
struct ImageLoaderPluginInfo {
    IImageLoader* (*create)(void);
};

/**
 * Descriptor for a cartridge handler factory provided by a plugin.
 */
struct CartridgePluginInfo {
    const char* extension;      // e.g. "crt", "car"
    ICartridgeHandler* (*create)(const char* path);
};

/**
 * Top-level manifest returned by the plugin entry point.
 */
struct SimPluginManifest {
    uint32_t apiVersion;
    const char* pluginId;
    const char* displayName;        // Human-readable name (e.g. "VICE ROM Importer")
    const char* version;

    const char* const* deps;                 // Null-terminated list of required plugin IDs
    const char* const* supportedMachineIds;  // Null-terminated list; nullptr = all machines

    int coreCount;
    struct CorePluginInfo* cores;

    int toolchainCount;
    struct ToolchainPluginInfo* toolchains;

    int deviceCount;
    struct DevicePluginInfo* devices;

    int machineCount;
    struct MachinePluginInfo* machines;

    int loaderCount;
    struct ImageLoaderPluginInfo* loaders;

    int cartridgeCount;
    struct CartridgePluginInfo* cartridges;
};


/**
 * Every plugin .so must export:
 *   extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host);
 */
typedef struct SimPluginManifest* (*MmemuPluginInitFn)(const struct SimPluginHostAPI* host);

#ifdef __cplusplus
}
#endif
