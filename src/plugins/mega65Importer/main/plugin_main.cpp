#include "include/mmemu_plugin_api.h"
#include "rom_discovery.h"
#include "rom_importer.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>

using namespace mega65_importer;

#ifdef __cplusplus
extern "C" {
#endif

static const SimPluginHostAPI* g_host = nullptr;

// ---------------------------------------------------------------------------
// CLI command: importmega65
// ---------------------------------------------------------------------------

static int cmdImportMega65(int argc, const char* const* argv, void* ctx) {
    (void)ctx;
    bool listOnly = false;
    int sourceIdx = -1;
    std::string destDir = "roms/mega65";
    std::string xemuBinary;
    bool overwrite = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--list") listOnly = true;
        else if (arg == "--source" && i + 1 < argc) sourceIdx = std::stoi(argv[++i]);
        else if (arg == "--dest" && i + 1 < argc) destDir = argv[++i];
        else if (arg == "--xemu" && i + 1 < argc) xemuBinary = argv[++i];
        else if (arg == "--overwrite") overwrite = true;
    }

    auto sources = discoverSources("mega65");
    std::string xemu = xemuBinary.empty() ? findXemuBinary() : xemuBinary;

    if (listOnly) {
        std::cout << "Found " << sources.size() << " MEGA65 ROM source(s):\n";
        for (size_t i = 0; i < sources.size(); ++i) {
            std::cout << "  [" << i << "] " << sources[i].label << " (" << sources[i].basePath << ")\n";
        }
        std::cout << "HYPPO (HICKUP.M65) source: " << (xemu.empty() ? "none (xemu-xmega65 not found)" : xemu) << "\n";
        return 0;
    }

    int rc = 0;
    std::string sourcePath;
    if (sources.empty()) {
        std::cout << "No MEGA65 ROMs found in standard locations.\n";
        rc = 1;
    } else {
        if (sourceIdx < 0) {
            if (sources.size() == 1) {
                sourceIdx = 0;
            } else {
                std::cout << "Multiple sources found. Please specify one with --source <n>:\n";
                for (size_t i = 0; i < sources.size(); ++i) {
                    std::cout << "  [" << i << "] " << sources[i].label << "\n";
                }
                return 1;
            }
        }

        if (sourceIdx >= (int)sources.size()) {
            std::cout << "Invalid source index: " << sourceIdx << "\n";
            return 1;
        }

        sourcePath = sources[sourceIdx].basePath;
        std::cout << "Importing MEGA65 ROM from: " << sources[sourceIdx].label << " to " << destDir << "...\n";
        auto result = importRoms("mega65", sourcePath, destDir, overwrite);
        if (result.success) {
            std::cout << "Successfully imported MEGA65 ROM.\n";
        } else {
            std::cerr << "Import failed: " << result.message << "\n";
            rc = 1;
        }
    }

    auto hyppo = importHickup(sourcePath, xemu, destDir, overwrite);
    if (hyppo.success) {
        std::cout << hyppo.message << "\n";
    } else {
        std::cerr << "HYPPO import failed: " << hyppo.message << "\n";
        rc = 1;
    }

    if (rc == 0) std::cout << "Please reset the machine to load the new ROMs.\n";
    return rc;
}

// ---------------------------------------------------------------------------
// MCP tool: import_mega65_roms
// ---------------------------------------------------------------------------

static void mcpImportMega65Roms(const char* paramsJson, char** resultJson, void* ctx) {
    (void)ctx;
    std::string params = paramsJson ? paramsJson : "{}";

    bool overwrite = false;
    size_t owPos = params.find("\"overwrite\"");
    if (owPos != std::string::npos) {
        size_t valPos = params.find_first_not_of(" \t\r\n:", owPos + 11);
        if (valPos != std::string::npos && params.compare(valPos, 4, "true") == 0)
            overwrite = true;
    }

    int sourceIndex = 0;
    size_t siPos = params.find("\"sourceIndex\"");
    if (siPos != std::string::npos) {
        size_t colonPos = params.find(':', siPos);
        if (colonPos != std::string::npos) {
            size_t numPos = params.find_first_of("0123456789", colonPos);
            if (numPos != std::string::npos)
                sourceIndex = std::stoi(params.substr(numPos));
        }
    }

    auto escape = [](std::string msg) {
        for (size_t i = 0; i < msg.size(); ++i) {
            if (msg[i] == '"' || msg[i] == '\\') { msg.insert(i, "\\"); ++i; }
        }
        return msg;
    };

    auto sources = discoverSources("mega65");
    if (sourceIndex < 0 || (!sources.empty() && sourceIndex >= (int)sources.size())) {
        *resultJson = strdup("{\"success\":false,\"error\":\"Invalid sourceIndex\"}");
        return;
    }

    std::string sourcePath;
    std::string romError;
    if (sources.empty()) {
        romError = "No MEGA65 ROMs found";
    } else {
        sourcePath = sources[sourceIndex].basePath;
        auto res = importRoms("mega65", sourcePath, "roms/mega65", overwrite);
        if (!res.success) romError = res.message;
    }

    auto hyppo = importHickup(sourcePath, "", "roms/mega65", overwrite);

    std::string json = "{\"success\":";
    json += (romError.empty() && hyppo.success) ? "true" : "false";
    json += ",\"rom\":\"" + (romError.empty() ? std::string("ok") : escape(romError)) + "\"";
    json += ",\"hyppo\":\"" + escape(hyppo.message) + "\"}";
    *resultJson = strdup(json.c_str());
}

static void mcpFreeString(char* s) {
    free(s);
}

// ---------------------------------------------------------------------------
// GUI pane factory (implemented in rom_import_pane.cpp)
// ---------------------------------------------------------------------------

void* createMega65RomImportPane(void* parent, void* ctx);

// ---------------------------------------------------------------------------
// Plugin entry point
// ---------------------------------------------------------------------------

SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    g_host = host;

    // CLI command
    static struct PluginCommandInfo cmdInfo;
    cmdInfo.name    = "importmega65";
    cmdInfo.usage   = "[--list] [--source <n>] [--dest <path>] [--xemu <binary>] [--overwrite]";
    cmdInfo.execute = cmdImportMega65;
    cmdInfo.ctx     = nullptr;
    host->registerCommand(&cmdInfo);

    // GUI pane
    static const char* paneMatchIds[] = { "mega65", nullptr };
    static struct PluginPaneInfo paneInfo;
    paneInfo.paneId      = "mega65-importer.main";
    paneInfo.displayName = "MEGA65 ROM Importer";
    paneInfo.menuSection = "Tools";
    paneInfo.machineIds  = paneMatchIds;
    paneInfo.createPane  = createMega65RomImportPane;
    paneInfo.destroyPane = nullptr;  // wxWidgets manages lifetime
    paneInfo.refreshPane = nullptr;
    paneInfo.ctx         = nullptr;
    host->registerPane(&paneInfo);

    // MCP tool
    static struct PluginMcpToolInfo mcpInfo;
    mcpInfo.toolName   = "import_mega65_roms";
    mcpInfo.schemaJson =
        "{"
        "\"type\":\"object\","
        "\"properties\":{"
        "\"sourceIndex\":{\"type\":\"integer\",\"description\":\"Index into discovered sources\"},"
        "\"overwrite\":{\"type\":\"boolean\",\"description\":\"Overwrite existing ROM files\"}"
        "}"
        "}";
    mcpInfo.handle     = mcpImportMega65Roms;
    mcpInfo.freeString = mcpFreeString;
    mcpInfo.ctx        = nullptr;
    host->registerMcpTool(&mcpInfo);

    // Manifest
    static const char* supportedMachines[] = { "mega65", nullptr };

    static SimPluginManifest manifest;
    std::memset(&manifest, 0, sizeof(manifest));
    manifest.apiVersion          = MMEMU_PLUGIN_API_VERSION;
    manifest.pluginId            = "mega65-importer";
    manifest.displayName         = "MEGA65 ROM Importer";
    manifest.version             = "0.1.0";
    manifest.deps                = nullptr;
    manifest.supportedMachineIds = supportedMachines;

    return &manifest;
}

#ifdef __cplusplus
}
#endif
