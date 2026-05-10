#pragma once

#include "idisasm.h"
#include "iassembler.h"
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>

class ToolchainRegistry {
public:
    using DisasmFactoryFn = std::function<IDisassembler*()>;
    using AsmFactoryFn    = std::function<IAssembler*()>;

    struct ToolchainInfo {
        std::string     isa;
        DisasmFactoryFn disasmFactory;
        AsmFactoryFn    asmFactory;
    };

    static ToolchainRegistry& instance();
    static void setInstance(ToolchainRegistry* inst);

    void registerToolchain(const std::string& isa, DisasmFactoryFn df, AsmFactoryFn af);
    IDisassembler* createDisassembler(const std::string& isa);
    IAssembler*    createAssembler(const std::string& isa);

    // Name-based assembler registry (for explicit assembler selection)
    void registerAssemblerByName(const std::string& name, AsmFactoryFn fn);
    IAssembler* createAssemblerByName(const std::string& name);

private:
    ToolchainRegistry() = default;
    std::map<std::string, ToolchainInfo> m_toolchains;
    std::map<std::string, AsmFactoryFn> m_assemblersByName;
};

/**
 * Resolve an assembler with 3-level precedence:
 * 1. runtimeOverride (CLI session / MCP per-machine override)
 * 2. machinePreferred (from machine JSON "assembler" key)
 * 3. isaDefault (ISA-based default from ToolchainRegistry)
 *
 * Returns nullptr if no assembler is available.
 * The returned assembler is configured with SimConfig if a matching config entry exists.
 */
IAssembler* resolveAssembler(const std::string& isa,
                             const std::string& machinePreferred,
                             const std::string& runtimeOverride);
