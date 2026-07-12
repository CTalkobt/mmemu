#pragma once

#include <string>
#include <utility>
#include <vector>
#include <iomanip>
#include <functional>
#include "libcore/main/machine_desc.h"
#include "libtoolchain/main/idisasm.h"
#include "libtoolchain/main/iassembler.h"
#include "libdebug/main/debug_context.h"

struct CliContext {
    MachineDescriptor* machine = nullptr;
    ICore* cpu = nullptr;
    IBus* bus = nullptr;
    IDisassembler* disasm = nullptr;
    IAssembler* assem = nullptr;
    DebugContext* dbg = nullptr;
    uint32_t lastLoadAddr = 0;
    bool quit = false;
    std::string assemblerOverride;  // Set by "config assembler <name>"
};

class CliInterpreter {
public:
    using OutputFn = std::function<void(const std::string&)>;

    CliInterpreter(CliContext& ctx, OutputFn output) : m_ctx(ctx), m_output(std::move(output)) {}

    void processLine(const std::string& line);
    [[nodiscard]] bool isAssemblyMode() const { return m_asmMode; }
    [[nodiscard]] uint32_t getAsmAddr() const { return m_asmAddr; }

private:
    void handleNormalCommand(const std::string& line);
    void handleAssemblyLine(const std::string& line);
    void printHelp(const std::string& category = "");
    void printHelpOverview();
    void printHelpCategory(const std::string& category);
    void printDebuggingGuide();
    void dumpMemory(uint32_t addr, uint32_t len);
    void saveMemory(const std::string& path, uint32_t addr, uint32_t len);
    void showRegisters();
    void showLocals();
    void showFrameLayout();
    void printVariable(const std::string& varName);
    [[nodiscard]] int addrWidth() const;

    CliContext& m_ctx;
    OutputFn m_output;
    bool m_asmMode = false;
    uint32_t m_asmAddr = 0;

    std::vector<uint8_t> m_lastSearchPattern;
    uint32_t m_lastSearchFoundAddr = 0xFFFFFFFF;
    std::string m_traceFilter = "all"; // all, calls, io
};
