#include "toolchain_registry.h"
#include "../main/sim_config.h"

static ToolchainRegistry* s_instance = nullptr;

ToolchainRegistry& ToolchainRegistry::instance() {
    if (!s_instance) s_instance = new ToolchainRegistry();
    return *s_instance;
}

void ToolchainRegistry::setInstance(ToolchainRegistry* inst) {
    s_instance = inst;
}

void ToolchainRegistry::registerToolchain(const std::string& isa, DisasmFactoryFn df, AsmFactoryFn af) {
    m_toolchains[isa] = {isa, df, af};
}

IDisassembler* ToolchainRegistry::createDisassembler(const std::string& isa) {
    if (m_toolchains.count(isa) && m_toolchains[isa].disasmFactory) {
        return m_toolchains[isa].disasmFactory();
    }
    return nullptr;
}

IAssembler* ToolchainRegistry::createAssembler(const std::string& isa) {
    if (m_toolchains.count(isa) && m_toolchains[isa].asmFactory) {
        return m_toolchains[isa].asmFactory();
    }
    return nullptr;
}

void ToolchainRegistry::registerAssemblerByName(const std::string& name, AsmFactoryFn fn) {
    m_assemblersByName[name] = fn;
}

IAssembler* ToolchainRegistry::createAssemblerByName(const std::string& name) {
    if (m_assemblersByName.count(name) && m_assemblersByName[name]) {
        return m_assemblersByName[name]();
    }
    return nullptr;
}

std::vector<std::string> ToolchainRegistry::getAssemblerNames() const {
    std::vector<std::string> names;
    for (const auto& kv : m_assemblersByName) {
        names.push_back(kv.first);
    }
    // Also include ISA-default assemblers (create temporarily to get their name)
    for (const auto& kv : m_toolchains) {
        if (kv.second.asmFactory) {
            IAssembler* a = kv.second.asmFactory();
            if (a) {
                std::string n = a->name();
                delete a;
                // Avoid duplicates
                bool found = false;
                for (const auto& existing : names) {
                    if (existing == n) { found = true; break; }
                }
                if (!found) names.push_back(n);
            }
        }
    }
    return names;
}

void ToolchainRegistry::clear() {
    m_toolchains.clear();
    m_assemblersByName.clear();
}

IAssembler* resolveAssembler(const std::string& isa,
                             const std::string& machinePreferred,
                             const std::string& runtimeOverride) {
    IAssembler* assembler = nullptr;
    std::string selectedName;

    // Priority 1: runtime override
    if (!runtimeOverride.empty()) {
        assembler = ToolchainRegistry::instance().createAssemblerByName(runtimeOverride);
        selectedName = runtimeOverride;
    }

    // Priority 2: machine preferred assembler
    if (!assembler && !machinePreferred.empty()) {
        assembler = ToolchainRegistry::instance().createAssemblerByName(machinePreferred);
        selectedName = machinePreferred;
    }

    // Priority 3: ISA default
    if (!assembler) {
        assembler = ToolchainRegistry::instance().createAssembler(isa);
        // For ISA defaults, try to infer the name from the assembler's name() method
        if (assembler) {
            selectedName = assembler->name();
        }
    }

    if (!assembler) {
        return nullptr;
    }

    // Apply configuration from SimConfig
    AssemblerConfig config = SimConfig::instance().assemblerConfig(selectedName);
    assembler->setConfig(config);

    return assembler;
}
