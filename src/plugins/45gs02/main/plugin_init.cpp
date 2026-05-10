#include "mmemu_plugin_api.h"
#include "cpu45gs02.h"
#include "disassembler_45gs02.h"
#include "ca45_assembler.h"

static const SimPluginHostAPI* g_host = nullptr;

static ICore* createCore45GS02() {
    return new MOS45GS02();
}

static IDisassembler* createDisassembler45GS02() {
    return new Disassembler45GS02();
}

static IAssembler* createAsmCa45() {
    return new Ca45AssemblerBackend();
}

static CorePluginInfo s_cores[] = {
    {"45GS02", "MEGA65", "open", createCore45GS02}
};

static ToolchainPluginInfo s_toolchains[] = {
    {"45GS02", "ca45", createDisassembler45GS02, createAsmCa45}
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "45gs02",
    "MEGA65 45GS02 CPU Plugin",
    "0.1.0",
    nullptr, nullptr,
    1, s_cores,
    1, s_toolchains,
    0, nullptr,
    0, nullptr,
    0, nullptr,
    0, nullptr
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    g_host = host;
    return &s_manifest;
}
