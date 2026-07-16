#include "cli_interpreter.h"
#include <csignal>
#include "include/util/logging.h"
#include "vice_snapshot.h"
#include "lua_engine.h"

// Global interrupt flag — set by SIGINT handler, checked by run loops.
// Defined here (not in main.cpp) so the test binary can link it too.
volatile sig_atomic_t g_interrupted = 0;
#include "libcore/main/machines/machine_registry.h"
#include "libtoolchain/main/toolchain_registry.h"
#include "libcore/main/image_loader.h"
#include "libcore/main/sim_config.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/io_registry.h"
#include "plugin_command_registry.h"
#include "libdebug/main/expression_evaluator.h"
#include "libdebug/main/debug_helpers.h"
#include "libdebug/main/source_location_formatter.h"
#include "libdebug/main/frame_analyzer.h"
#include "imap_controller.h"
#include "plugins/devices/map_mmu/main/map_mmu.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <set>
#include <map>
#include <unordered_map>
#include <cstring>
#include "libdevices/main/device_info.h"

static std::string toHex(uint32_t v, int width = 4) {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0') << std::setw(width) << v;
    return ss.str();
}

int CliInterpreter::addrWidth() const {
    if (!m_ctx.bus) return 4;
    uint32_t bits = m_ctx.bus->config().addrBits;
    return (bits + 3) / 4;  // round up to nearest hex digit
}

void CliInterpreter::processLine(const std::string& line) {
    if (m_asmMode) {
        handleAssemblyLine(line);
    } else {
        handleNormalCommand(line);
    }
}

void CliInterpreter::handleNormalCommand(const std::string& line) {
    if (line.empty()) return;

    auto parseAddr = [&](const std::string& expr, uint32_t& addr) -> bool {
        return ExpressionEvaluator::evaluate(expr, m_ctx.dbg, addr);
    };

    if (line[0] == '.') {
        if (!m_ctx.cpu) {
            m_output("No machine created.\n");
            return;
        }
        std::string instr = line.substr(1);
        uint8_t opcodes[16];
        int sz = -1;
        // Try primary assembler first
        if (m_ctx.assem)
            sz = m_ctx.assem->assembleLine(instr, opcodes, sizeof(opcodes), m_ctx.cpu->pc());
        // Fallback: try ISA default, then base "6502" assembler (handles LDA/STA/etc.)
        if (sz <= 0) {
            const char* fallbackIsas[] = { m_ctx.cpu->isaName(), "6502", nullptr };
            for (int fi = 0; sz <= 0 && fallbackIsas[fi]; fi++) {
                IAssembler* fallback = ToolchainRegistry::instance().createAssembler(fallbackIsas[fi]);
                if (fallback) {
                    sz = fallback->assembleLine(instr, opcodes, sizeof(opcodes), m_ctx.cpu->pc());
                    delete fallback;
                }
            }
        }
        if (sz > 0) {
            uint32_t scratch = 0x0200;
            for (int i = 0; i < sz; ++i) m_ctx.bus->write8(scratch + i, opcodes[i]);
            uint32_t oldPc = m_ctx.cpu->pc();
            m_ctx.cpu->setPc(scratch);
            m_ctx.cpu->step();
            m_ctx.cpu->setPc(oldPc);
            showRegisters();
        } else {
            m_output("Assembly failed: " + instr + "\n");
        }
        return;
    }

    std::stringstream ss(line);
    std::string cmd;
    ss >> cmd;

    if (cmd == "help" || cmd == "?") {
        std::string category;
        ss >> category;
        printHelp(category);
    } else if (cmd == "list") {
        std::vector<std::string> ids;
        MachineRegistry::instance().enumerate(ids);
        m_output("Available machines:\n");
        for (const auto& id : ids) m_output("  " + id + "\n");
    } else if (cmd == "log") {
        std::string sub;
        if (ss >> sub) {
            if (sub == "list") {
                auto names = LogRegistry::instance().getLoggerNames();
                m_output("Registered loggers:\n");
                for (const auto& n : names) {
                    auto l = LogRegistry::instance().getLogger(n);
                    std::string lvl = spdlog::level::to_string_view(l->level()).data();
                    m_output("  " + n + " [" + lvl + "]\n");
                }
            } else if (sub == "level") {
                std::string target, levelStr;
                if (ss >> target >> levelStr) {
                    spdlog::level::level_enum lvl = spdlog::level::from_str(levelStr);
                    if (target == "all") {
                        LogRegistry::instance().setGlobalLevel(lvl);
                        m_output("Set all loggers to " + levelStr + "\n");
                    } else {
                        auto l = LogRegistry::instance().getLogger(target);
                        l->set_level(lvl);
                        m_output("Set logger '" + target + "' to " + levelStr + "\n");
                    }
                } else {
                    m_output("Usage: log level <name|all|kernal> <trace|debug|info|warn|error|off>\n");
                }
            }
        } else {
            m_output("Usage: log <list|level>\n");
        }
    } else if (cmd == "save") {
        std::string path;
        std::string addrStr, lenStr;
        if (ss >> path >> addrStr >> lenStr) {
            uint32_t addr, len;
            if (ExpressionEvaluator::evaluate(addrStr, m_ctx.dbg, addr) &&
                ExpressionEvaluator::evaluate(lenStr, m_ctx.dbg, len)) {
                saveMemory(path, addr, len);
            } else {
                m_output("Invalid address or length.\n");
            }
        } else {
            m_output("Usage: save <path> <addr> <len>\n");
        }
    } else if (cmd == "create") {
        std::string id;
        if (ss >> id) {
            if (MachineDescriptor* md = MachineRegistry::instance().createMachine(id)) {
                if (md->cpus.empty() || md->buses.empty()) {
                    m_output("Error: Machine '" + id + "' is incomplete (missing CPU or Bus).\n");
                    delete md;
                    return;
                }
                if (m_ctx.machine) {
                    if (m_ctx.cpu) m_ctx.cpu->setObserver(nullptr);
                    if (m_ctx.bus) m_ctx.bus->setObserver(nullptr);
                    delete m_ctx.machine;
                    m_ctx.machine = nullptr;
                }
                m_ctx.machine = md;
                m_ctx.cpu = md->cpus[0].cpu;
                // Use CPU data bus if available, otherwise fallback to machine bus
                m_ctx.bus = m_ctx.cpu->getDataBus() ? m_ctx.cpu->getDataBus() : md->buses[0].bus;
                
                if (m_ctx.disasm) { delete m_ctx.disasm; m_ctx.disasm = nullptr; }
                if (m_ctx.assem)  { delete m_ctx.assem;  m_ctx.assem = nullptr; }
                m_ctx.disasm = ToolchainRegistry::instance().createDisassembler(m_ctx.cpu->isaName());
                m_ctx.assem = resolveAssembler(m_ctx.cpu->isaName(), md->preferredAssembler, "");
                
                if (m_ctx.dbg) { delete m_ctx.dbg; m_ctx.dbg = nullptr; }
                m_ctx.dbg = new DebugContext(m_ctx.cpu, m_ctx.bus);
                if (md->ioRegistry) m_ctx.dbg->setIoRegistry(md->ioRegistry);
                m_ctx.cpu->setObserver(m_ctx.dbg);
                m_ctx.bus->setObserver(m_ctx.dbg);
                m_ctx.dbg->onMachineLoad(md);

                for (const auto& path : md->symbolFiles) {
                    m_ctx.dbg->symbols().loadSym(path);
                }

                if (m_ctx.disasm) {
                    m_ctx.disasm->setSymbolTable(&m_ctx.dbg->symbols());
                }

                m_output("Created machine: " + md->displayName + "\n");
                
                if (m_ctx.machine->onReset) {
                    m_ctx.machine->onReset(*m_ctx.machine);
                }
                showRegisters();
            } else {
                m_output("Unknown machine type: " + id + "\n");
            }
        }
    } else if (cmd == "reset") {
        if (!m_ctx.machine) { m_output("No machine created.\n"); return; }
        if (m_ctx.machine->onReset) {
            m_ctx.machine->onReset(*m_ctx.machine);
            m_output("Machine reset.\n");
        }
        // Refresh bus reference in case it changed during reset
        m_ctx.bus = m_ctx.cpu->getDataBus() ? m_ctx.cpu->getDataBus() : m_ctx.machine->buses[0].bus;
        showRegisters();
    } else if (cmd == "step") {
        if (!m_ctx.cpu) { m_output("No machine created.\n"); return; }

        // Check if argument is provided
        std::string arg;
        bool hasArg = !!(ss >> arg);

        if (hasArg) {
            // Old behavior: execute N CPU instructions
            int n = 1;
            try { n = std::stoi(arg); } catch (...) { n = 1; }
            if (m_ctx.dbg) m_ctx.dbg->resume();
            for (int i = 0; i < n; ++i) {
                if (m_ctx.machine && m_ctx.machine->schedulerStep) {
                    m_ctx.machine->schedulerStep(*m_ctx.machine);
                } else {
                    m_ctx.cpu->step();
                }
                if (m_ctx.dbg && m_ctx.dbg->isPaused()) break;
                if (m_ctx.cpu->isProgramEnd(m_ctx.bus)) break;
            }
            showRegisters();
        } else if (m_ctx.dbg) {
            // New behavior: source-level step (Phase 4 - step-into)
            // Step to next source line, following function calls
            uint32_t startPC = m_ctx.cpu->pc();
            auto startLoc = m_ctx.dbg->sourceMap().addrToSource(startPC);

            if (startLoc.file.empty()) {
                // No source mapping - fall back to one instruction
                m_output("No source mapping for current PC. Executing one instruction.\n");
                if (m_ctx.machine && m_ctx.machine->schedulerStep) {
                    m_ctx.machine->schedulerStep(*m_ctx.machine);
                } else {
                    m_ctx.cpu->step();
                }
            } else {
                // Source-level step: execute until we reach a different source line
                m_output("Stepping to next source line (following function calls)...\n");
                m_ctx.dbg->resume();

                int steps = 0;
                const int MAX_STEPS = 10000000;

                // Execute instructions until we reach a different source line
                while (steps < MAX_STEPS && !g_interrupted) {
                    if (m_ctx.machine && m_ctx.machine->schedulerStep) {
                        m_ctx.machine->schedulerStep(*m_ctx.machine);
                    } else {
                        m_ctx.cpu->step();
                    }
                    ++steps;

                    // Check if we hit a breakpoint
                    if (m_ctx.dbg->isPaused()) {
                        m_output("Breakpoint hit.\n");
                        break;
                    }

                    // Check if program ended
                    if (m_ctx.cpu->isProgramEnd(m_ctx.bus)) {
                        m_output("Program end reached.\n");
                        break;
                    }

                    // Check if we reached a new source line
                    uint32_t currentPC = m_ctx.cpu->pc();
                    auto currentLoc = m_ctx.dbg->sourceMap().addrToSource(currentPC);

                    // Stop if we're on a different line (in any file)
                    if (!currentLoc.file.empty() &&
                        (currentLoc.file != startLoc.file || currentLoc.line != startLoc.line)) {
                        m_output("Reached source line " + std::to_string(currentLoc.line) + " in " + currentLoc.file + "\n");
                        break;
                    }
                }

                if (steps >= MAX_STEPS) {
                    m_output("Warning: Step limit reached (possible infinite loop)\n");
                }
                if (g_interrupted) {
                    m_output("Interrupted.\n");
                    g_interrupted = 0;
                }
            }
            showRegisters();
        } else {
            // No source mapping available, just execute one instruction
            if (m_ctx.machine && m_ctx.machine->schedulerStep) {
                m_ctx.machine->schedulerStep(*m_ctx.machine);
            } else {
                m_ctx.cpu->step();
            }
            showRegisters();
        }
    } else if (cmd == "backstep" || cmd == "bs") {
        if (!m_ctx.cpu) { m_output("No machine created.\n"); return; }
        if (!m_ctx.dbg) { m_output("No debug context.\n"); return; }
        int n = 1;
        if (ss >> n) {} else { n = 1; }
        int reversed = 0;
        for (int i = 0; i < n; ++i) {
            if (!m_ctx.dbg->reverseStep()) break;
            ++reversed;
        }
        if (reversed == 0) {
            m_output("No undo history available.\n");
        } else {
            m_output("Reversed " + std::to_string(reversed) + " instruction(s).\n");
            showRegisters();
        }
    } else if (cmd == "next") {
        if (!m_ctx.cpu || !m_ctx.dbg) { m_output("No machine created.\n"); return; }

        // Get current source line
        uint32_t startPC = m_ctx.cpu->pc();
        auto startLoc = m_ctx.dbg->sourceMap().addrToSource(startPC);

        if (startLoc.file.empty()) {
            m_output("No source mapping for current PC. Load source (.loc directives) to use 'next'.\n");
            return;
        }

        m_output("Stepping to next source line...\n");
        m_ctx.dbg->resume();

        int steps = 0;
        const int MAX_STEPS = 10000000;  // Safety limit to prevent infinite loops

        // Execute instructions until we reach a different source line
        while (steps < MAX_STEPS && !g_interrupted) {
            if (m_ctx.machine && m_ctx.machine->schedulerStep) {
                m_ctx.machine->schedulerStep(*m_ctx.machine);
            } else {
                m_ctx.cpu->step();
            }
            ++steps;

            // Check if we hit a breakpoint
            if (m_ctx.dbg->isPaused()) {
                m_output("Breakpoint hit.\n");
                break;
            }

            // Check if program ended
            if (m_ctx.cpu->isProgramEnd(m_ctx.bus)) {
                m_output("Program end reached.\n");
                break;
            }

            // Check if we reached a new source line
            uint32_t currentPC = m_ctx.cpu->pc();
            auto currentLoc = m_ctx.dbg->sourceMap().addrToSource(currentPC);

            // Stop if we're on a different line in the same file, or different file entirely
            if (!currentLoc.file.empty() &&
                (currentLoc.file != startLoc.file || currentLoc.line != startLoc.line)) {
                m_output("Reached source line " + std::to_string(currentLoc.line) + " in " + currentLoc.file + "\n");
                break;
            }
        }

        if (steps >= MAX_STEPS) {
            m_output("Warning: Step limit reached (possible infinite loop)\n");
        }
        if (g_interrupted) {
            m_output("Interrupted.\n");
            g_interrupted = 0;
        }

        showRegisters();
    } else if (cmd == "finish") {
        if (!m_ctx.cpu || !m_ctx.dbg) { m_output("No machine created.\n"); return; }

        int startDepth = m_ctx.dbg->stackTrace().depth();

        if (startDepth == 0) {
            m_output("Not inside a function call (stack depth is 0).\n");
            return;
        }

        m_output("Running until function returns...\n");
        m_ctx.dbg->resume();

        int steps = 0;
        const int MAX_STEPS = 10000000;  // Safety limit

        // Execute instructions until stack depth decreases (return from function)
        while (steps < MAX_STEPS && !g_interrupted) {
            if (m_ctx.machine && m_ctx.machine->schedulerStep) {
                m_ctx.machine->schedulerStep(*m_ctx.machine);
            } else {
                m_ctx.cpu->step();
            }
            ++steps;

            // Check if we hit a breakpoint
            if (m_ctx.dbg->isPaused()) {
                m_output("Breakpoint hit.\n");
                break;
            }

            // Check if program ended
            if (m_ctx.cpu->isProgramEnd(m_ctx.bus)) {
                m_output("Program end reached.\n");
                break;
            }

            // Check if we've returned from the function (stack depth decreased)
            int currentDepth = m_ctx.dbg->stackTrace().depth();
            if (currentDepth < startDepth) {
                uint32_t returnPC = m_ctx.cpu->pc();
                auto loc = m_ctx.dbg->sourceMap().addrToSource(returnPC);
                if (!loc.file.empty()) {
                    m_output("Function returned at $" + toHex(returnPC) +
                             " (" + loc.file + ":" + std::to_string(loc.line) + ")\n");
                } else {
                    m_output("Function returned at $" + toHex(returnPC) + "\n");
                }
                break;
            }
        }

        if (steps >= MAX_STEPS) {
            m_output("Warning: Step limit reached (possible infinite loop)\n");
        }
        if (g_interrupted) {
            m_output("Interrupted.\n");
            g_interrupted = 0;
        }

        showRegisters();
    } else if (cmd == "until") {
        if (!m_ctx.cpu || !m_ctx.dbg) { m_output("No machine created.\n"); return; }

        // Parse: until <line_number> or until <filename>:<line_number>
        std::string lineSpec;
        if (!(ss >> lineSpec)) {
            m_output("Usage: until <line> or until <filename>:<line>\n");
            return;
        }

        // Determine target file and line
        std::string targetFile;
        int targetLine = -1;

        size_t colonPos = lineSpec.find(':');
        if (colonPos != std::string::npos) {
            // Format: filename:line
            targetFile = lineSpec.substr(0, colonPos);
            try {
                targetLine = std::stoi(lineSpec.substr(colonPos + 1));
            } catch (...) {
                m_output("Invalid line number in '" + lineSpec + "'\n");
                return;
            }
        } else {
            // Format: just line number - use current file
            uint32_t pc = m_ctx.cpu->pc();
            auto srcLoc = m_ctx.dbg->sourceMap().addrToSource(pc);
            if (srcLoc.file.empty()) {
                m_output("Cannot determine current source file. Use 'until <filename>:<line>'\n");
                return;
            }
            targetFile = srcLoc.file;
            try {
                targetLine = std::stoi(lineSpec);
            } catch (...) {
                m_output("Invalid line number: " + lineSpec + "\n");
                return;
            }
        }

        if (targetLine <= 0) {
            m_output("Line number must be > 0\n");
            return;
        }

        m_output("Continuing until " + targetFile + ":" + std::to_string(targetLine) + "\n");
        m_ctx.dbg->resume();

        int steps = 0;
        const int MAX_STEPS = 10000000;

        // Execute until we reach the target line
        while (steps < MAX_STEPS && !g_interrupted) {
            if (m_ctx.machine && m_ctx.machine->schedulerStep) {
                m_ctx.machine->schedulerStep(*m_ctx.machine);
            } else {
                m_ctx.cpu->step();
            }
            ++steps;

            // Check if we hit a breakpoint
            if (m_ctx.dbg->isPaused()) {
                m_output("Breakpoint hit.\n");
                break;
            }

            // Check if program ended
            if (m_ctx.cpu->isProgramEnd(m_ctx.bus)) {
                m_output("Program end reached.\n");
                break;
            }

            // Check if we've reached the target line
            uint32_t currentPC = m_ctx.cpu->pc();
            auto currentLoc = m_ctx.dbg->sourceMap().addrToSource(currentPC);

            if (!currentLoc.file.empty() && currentLoc.file == targetFile && currentLoc.line == targetLine) {
                m_output("Reached " + targetFile + ":" + std::to_string(targetLine) + "\n");
                break;
            }
        }

        if (steps >= MAX_STEPS) {
            m_output("Warning: Step limit reached (target line not found)\n");
        }
        if (g_interrupted) {
            m_output("Interrupted.\n");
            g_interrupted = 0;
        }

        showRegisters();
    } else if (cmd == "undoinfo") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        auto& tb = m_ctx.dbg->trace();
        if (tb.size() == 0) {
            m_output("No undo history available.\n");
        } else {
            const auto& entry = tb.at(tb.size() - 1);
            std::ostringstream os;
            os << "Last instruction: $" << toHex(entry.addr, addrWidth());
            if (!entry.mnemonic.empty()) os << "  " << entry.mnemonic;
            os << "\n  Registers: ";
            for (const auto& [name, val] : entry.regs) {
                int w = (name == "SP" || name == "PC") ? 4 : 2;
                os << name << "=$" << toHex(val, w) << " ";
            }
            os << "\n  Memory writes: " << entry.memWrites.size();
            for (const auto& mw : entry.memWrites) {
                os << "\n    $" << toHex(mw.addr, addrWidth())
                   << ": was $" << toHex(mw.before, 2);
            }
            os << "\n";
            m_output(os.str());
        }
    } else if (cmd == "runto") {
        if (!m_ctx.cpu) { m_output("No machine created.\n"); return; }
        if (!m_ctx.dbg) { m_output("No debug context.\n"); return; }
        std::string rest;
        std::getline(ss, rest);
        rest = rest.substr(rest.find_first_not_of(" \t") != std::string::npos ? rest.find_first_not_of(" \t") : 0);
        if (rest.empty()) {
            m_output("Syntax: runto <condition_expression> [max_steps]\n");
            m_output("  Example: runto A == $42\n");
            m_output("  Example: runto *$D012 > 100\n");
        } else {
            // Check for trailing max_steps number
            int maxSteps = 10000000;
            m_ctx.dbg->resume();
            g_interrupted = 0;
            int steps = 0;
            bool condMet = false;
            while (!g_interrupted && steps < maxSteps) {
                if (m_ctx.machine && m_ctx.machine->schedulerStep)
                    m_ctx.machine->schedulerStep(*m_ctx.machine);
                else
                    m_ctx.cpu->step();
                ++steps;
                if (m_ctx.dbg->isPaused()) break;
                if (ExpressionEvaluator::evaluateCondition(rest, m_ctx.dbg)) {
                    condMet = true;
                    break;
                }
            }
            if (condMet)
                m_output("Condition met after " + std::to_string(steps) + " steps.\n");
            else if (g_interrupted)
                m_output("Interrupted after " + std::to_string(steps) + " steps.\n");
            else if (m_ctx.dbg->isPaused())
                m_output("Breakpoint hit after " + std::to_string(steps) + " steps.\n");
            else
                m_output("Max steps reached (" + std::to_string(steps) + ").\n");
            g_interrupted = 0;
            showRegisters();
        }
    } else if (cmd == "run") {
        if (!m_ctx.cpu) { m_output("No machine created.\n"); return; }
        std::string expr;
        bool breakpointOnly = false;
        int maxSteps = 0; // 0 = unlimited
        if (ss >> expr) {
            if (expr == "breakpoint") {
                breakpointOnly = true;
            } else {
                // Try as decimal cycle count first (e.g. "run 5000000")
                bool isDecimal = true;
                for (char c : expr) { if (!isdigit(c)) { isDecimal = false; break; } }
                if (isDecimal && expr.size() > 0) {
                    maxSteps = std::stoi(expr);
                } else {
                    uint32_t addr;
                    if (parseAddr(expr, addr)) {
                        m_ctx.cpu->setPc(addr);
                    } else {
                        m_output("Error: Invalid address or step count '" + expr + "'\n");
                        return;
                    }
                }
            }
        } else if (m_ctx.lastLoadAddr != 0) {
            m_ctx.cpu->setPc(m_ctx.lastLoadAddr);
        }
        if (maxSteps > 0)
            m_output("Running " + std::to_string(maxSteps) + " steps... (Ctrl-C to stop)\n");
        else
            m_output("Running... (Ctrl-C to stop)\n");
        g_interrupted = 0;
        m_ctx.dbg->resume();
        int steps = 0;
        const int STATUS_INTERVAL = 100000; // Report status every 100k steps
        int nextStatusAt = STATUS_INTERVAL;
        while (!m_ctx.dbg->isPaused() && !g_interrupted) {
            if (m_ctx.machine && m_ctx.machine->schedulerStep) {
                m_ctx.machine->schedulerStep(*m_ctx.machine);
            } else {
                m_ctx.cpu->step();
            }
            ++steps;
            if (!breakpointOnly && m_ctx.cpu->isProgramEnd(m_ctx.bus)) break;
            if (maxSteps > 0 && steps >= maxSteps) break;

            // Periodic status reporting to show responsiveness
            if (steps >= nextStatusAt) {
                uint32_t pc = m_ctx.cpu->pc();
                m_output("  " + std::to_string(steps) + " steps, PC=$" +
                        toHex(pc >> 20, 2) + ":" + toHex(pc & 0xFFFFF, 5) + "\n");
                nextStatusAt += STATUS_INTERVAL;
            }
        }
        if (g_interrupted) {
            m_output("Interrupted.\n");
            g_interrupted = 0;
        } else if (m_ctx.dbg->isPaused()) {
            m_output("Breakpoint hit.\n");
        } else if (maxSteps > 0 && steps >= maxSteps) {
            m_output("Stopped after " + std::to_string(steps) + " steps.\n");
        }
        showRegisters();
    } else if (cmd == "load") {
        if (!m_ctx.bus) { m_output("No machine created.\n"); return; }
        std::string path, expr;
        if (ss >> path) {
            uint32_t addr = 0;
            bool hasAddr = false;
            if (ss >> expr) {
                if (parseAddr(expr, addr)) {
                    hasAddr = true;
                } else {
                    m_output("Error: Invalid address '" + expr + "'\n");
                    return;
                }
            }
            // Use the physical bus for loading so data goes to the literal
            // address regardless of MAP state (#79).  For machines without
            // address translation (C64, VIC-20) this is the same as the
            // CPU's dataBus.
            IBus* loadBus = m_ctx.machine->buses[0].bus;
            if (auto* loader = ImageLoaderRegistry::instance().findLoader(path)) {
                if (loader->load(path, loadBus, m_ctx.machine, addr)) {
                    m_output("Loaded '" + path + "' using " + loader->name() + "\n");
                    // Extract load address if not provided (for .prg)
                    if (!hasAddr && std::string(loader->name()).find("PRG") != std::string::npos) {
                        std::ifstream f(path, std::ios::binary);
                        uint8_t h[2];
                        f.read((char*)h, 2);
                        m_ctx.lastLoadAddr = h[0] | (h[1] << 8);
                    } else if (hasAddr) {
                        m_ctx.lastLoadAddr = addr;
                    }
                } else {
                    m_output("Failed to load '" + path + "'\n");
                }
            } else {
                m_output("No loader found for '" + path + "'\n");
            }
        } else {
            m_output("Syntax: load <path> [address]\n");
        }
    } else if (cmd == "load-vice") {
        if (!m_ctx.bus) { m_output("No machine created.\n"); return; }
        std::string path;
        if (ss >> path) {
            if (ViceSnapshotLoader::load(path, m_ctx.cpu, m_ctx.bus, m_ctx.dbg,
                                        m_ctx.machine ? m_ctx.machine->ioRegistry : nullptr)) {
                m_output("Loaded VICE snapshot: " + path + "\n");
                showRegisters();
            } else {
                m_output("Failed to load VICE snapshot: " + ViceSnapshotLoader::getLastError() + "\n");
            }
        } else {
            m_output("Syntax: load-vice <path>\n");
        }
    } else if (cmd == "save-vice") {
        if (!m_ctx.bus) { m_output("No machine created.\n"); return; }
        std::string path;
        if (ss >> path) {
            // Determine machine type from current machine
            std::string machineType = "C64";
            if (m_ctx.machine && m_ctx.machine->displayName.find("VIC-20") != std::string::npos) {
                machineType = "VIC20";
            } else if (m_ctx.machine && m_ctx.machine->displayName.find("MEGA65") != std::string::npos) {
                machineType = "MEGA65";
            } else if (m_ctx.machine && m_ctx.machine->displayName.find("PET") != std::string::npos) {
                machineType = "PET";
            }
            if (ViceSnapshotSaver::save(path, machineType, m_ctx.cpu, m_ctx.bus, m_ctx.dbg,
                                       m_ctx.machine ? m_ctx.machine->ioRegistry : nullptr)) {
                m_output("Saved VICE snapshot: " + path + "\n");
            } else {
                m_output("Failed to save VICE snapshot: " + ViceSnapshotSaver::getLastError() + "\n");
            }
        } else {
            m_output("Syntax: save-vice <path>\n");
        }
    } else if (cmd == "script") {
        if (!m_ctx.cpu) { m_output("No machine created.\n"); return; }
        std::string subcmd;
        if (ss >> subcmd) {
            if (subcmd == "run") {
                std::string path;
                if (ss >> path) {
                    LuaEngine engine(m_ctx.cpu, m_ctx.bus, m_ctx.dbg);
                    if (engine.executeFile(path)) {
                        m_output("Script executed: " + path + "\n");
                    } else {
                        m_output("Script error: " + engine.getLastError() + "\n");
                    }
                } else {
                    m_output("Syntax: script run <path>\n");
                }
            } else if (subcmd == "eval") {
                std::string code;
                std::getline(ss, code);
                if (!code.empty()) {
                    LuaEngine engine(m_ctx.cpu, m_ctx.bus, m_ctx.dbg);
                    if (engine.executeString(code)) {
                        m_output("Code executed.\n");
                    } else {
                        m_output("Error: " + engine.getLastError() + "\n");
                    }
                } else {
                    m_output("Syntax: script eval <lua code>\n");
                }
            } else {
                m_output("Usage: script <run|eval>\n");
            }
        } else {
            m_output("Usage: script <run|eval>\n");
        }
    } else if (cmd == "info") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        std::string sub;
        ss >> sub;
        if (sub == "breaks") {
            const auto& breaks = m_ctx.dbg->breakpoints().breakpoints();
            if (breaks.empty()) {
                m_output("No breakpoints set.\n");
            } else {
                m_output("Num     Type        Enb Location              Hits  Condition/Size\n");
                for (const auto& bp : breaks) {
                    std::stringstream row;
                    row << std::left << std::setw(8) << bp.id;
                    std::string type;
                    switch (bp.type) {
                        case BreakpointType::EXEC: type = "exec"; break;
                        case BreakpointType::READ_WATCH: type = "read"; break;
                        case BreakpointType::WRITE_WATCH: type = "write"; break;
                        case BreakpointType::VALUE_WATCH: type = "value"; break;
                    }
                    if (bp.physical) type += " (phys)";
                    row << std::left << std::setw(12) << type;
                    row << (bp.enabled ? "y" : "n") << "   ";

                    // Try to show source location, fall back to address
                    SourceLocation srcLoc = m_ctx.dbg->sourceMap().addrToSource(bp.addr);
                    if (!srcLoc.file.empty() && srcLoc.line > 0) {
                        // Show source location
                        std::string srcDisplay = srcLoc.file + ":" + std::to_string(srcLoc.line);
                        row << std::left << std::setw(23) << srcDisplay;
                    } else {
                        // Fall back to address
                        std::stringstream addrStr;
                        addrStr << "$" << std::hex << std::uppercase << std::setfill('0')
                                << std::setw(bp.physical ? 7 : 4) << bp.addr;
                        row << std::left << std::setw(23) << addrStr.str();
                    }

                    row << std::dec << std::setfill(' ') << std::setw(4) << bp.hitCount << "  ";

                    if (bp.type == BreakpointType::VALUE_WATCH) {
                        row << bp.watchSize << " bytes";
                    } else if (!bp.condition.empty()) {
                        row << bp.condition;
                        if (bp.hitCountLimit > 0) {
                            row << " (limit: " << bp.hitCountLimit << ")";
                        }
                    } else if (bp.hitCountLimit > 0) {
                        row << "(hit limit: " << bp.hitCountLimit << ")";
                    }

                    // Show Lua action if present (Issue #24)
                    if (!bp.luaAction.empty()) {
                        row << " [Lua: " << bp.luaAction.substr(0, 30);
                        if (bp.luaAction.length() > 30) row << "...";
                        row << "]";
                    }

                    m_output(row.str() + "\n");
                }
            }
        } else if (sub == "locals") {
            showLocals();
        } else if (sub == "frame") {
            showFrameLayout();
        } else {
            m_output("Usage: info <breaks|locals|frame>\n");
        }
    } else if (cmd == "break") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        std::string expr;
        if (ss >> expr) {
            // Check for "at" prefix for source-level breakpoints (#95)
            if (expr == "at") {
                // Parse: break at line N [count N]
                std::string lineExpr;
                if (!(ss >> lineExpr)) {
                    m_output("Syntax: break at line <N> [count N] | break at <file>:<N>\n");
                    return;
                }

                // Parse line number, optionally with filename (file:line format)
                std::string filename;
                int lineNum = -1;

                size_t colonPos = lineExpr.find(':');
                if (colonPos != std::string::npos) {
                    // file:line format
                    filename = lineExpr.substr(0, colonPos);
                    std::stringstream ss2(lineExpr.substr(colonPos + 1));
                    ss2 >> lineNum;
                } else {
                    // Just line number
                    if (lineExpr == "line") {
                        if (!(ss >> lineNum)) {
                            m_output("Error: Expected line number after 'line'\n");
                            return;
                        }
                    } else {
                        std::stringstream ss2(lineExpr);
                        ss2 >> lineNum;
                    }
                }

                if (lineNum <= 0) {
                    m_output("Error: Invalid line number\n");
                    return;
                }

                // Check for "count" modifier
                int hitCountLimit = 0;
                std::string countStr;
                if (ss >> countStr && countStr == "count") {
                    if (!(ss >> hitCountLimit)) {
                        m_output("Error: count requires a number\n");
                        return;
                    }
                }

                // Convert source line to address via SourceMap
                uint32_t addr;
                if (!filename.empty()) {
                    addr = m_ctx.dbg->sourceMap().sourceToAddr(filename, lineNum);
                } else {
                    // If no filename specified, use the first file in source map
                    auto files = m_ctx.dbg->sourceMap().getSourceFiles();
                    if (files.empty()) {
                        m_output("Error: No source map loaded. Load an assembly file with .loc directives.\n");
                        return;
                    }
                    addr = m_ctx.dbg->sourceMap().sourceToAddr(files[0], lineNum);
                }

                if (addr == 0xFFFFFFFF) {
                    m_output("Error: Source line " + std::to_string(lineNum));
                    if (!filename.empty()) m_output(" in " + filename);
                    m_output(" not found in source map\n");
                    return;
                }

                int id = m_ctx.dbg->breakpoints().add(addr, BreakpointType::EXEC);
                if (hitCountLimit > 0) {
                    m_ctx.dbg->breakpoints().setHitCountLimit(id, hitCountLimit);
                }

                m_output("Breakpoint " + std::to_string(id) + " at ");
                if (!filename.empty()) {
                    m_output(filename + ":");
                }
                m_output(std::to_string(lineNum) + " (address $" + toHex(addr, addrWidth()) + ")");
                if (hitCountLimit > 0) {
                    m_output(" (stops at hit " + std::to_string(hitCountLimit) + ")");
                }
                m_output("\n");
                return;
            }

            // Check for "when" prefix for conditional breakpoints (#97)
            if (expr == "when") {
                // Parse: break when <condition> [count N]
                std::string rest;
                std::getline(ss, rest);

                // Check for "count" modifier
                int hitCountLimit = 0;
                size_t countPos = rest.find(" count ");
                if (countPos != std::string::npos) {
                    std::string limitStr = rest.substr(countPos + 7);
                    hitCountLimit = std::stoi(limitStr);
                    rest = rest.substr(0, countPos);
                }

                // Create a temporary breakpoint at address 0 for condition testing
                int id = m_ctx.dbg->breakpoints().add(0, BreakpointType::EXEC);
                m_ctx.dbg->breakpoints().setCondition(id, rest);
                if (hitCountLimit > 0) {
                    m_ctx.dbg->breakpoints().setHitCountLimit(id, hitCountLimit);
                }
                m_output("Conditional breakpoint " + std::to_string(id) + " set: when " + rest);
                if (hitCountLimit > 0) {
                    m_output(" (stops at hit " + std::to_string(hitCountLimit) + ")");
                }
                m_output("\n");
                return;
            }

            // Check for "phys" prefix for physical-address breakpoints (#73)
            bool physical = false;
            if (expr == "phys") {
                physical = true;
                if (!(ss >> expr)) {
                    m_output("Syntax: break phys <physical_address>\n");
                    return;
                }
            }

            uint32_t addr;
            if (parseAddr(expr, addr)) {
                // Check for optional modifiers: count N, action "code"
                int hitCountLimit = 0;
                std::string luaAction;
                std::string modifier;

                while (ss >> modifier) {
                    if (modifier == "count") {
                        if (!(ss >> hitCountLimit)) {
                            m_output("Error: count requires a number\n");
                            return;
                        }
                    } else if (modifier == "action") {
                        // Read rest of line as Lua action
                        std::getline(ss, luaAction);
                        // Trim leading whitespace
                        size_t start = luaAction.find_first_not_of(" \t");
                        if (start != std::string::npos) {
                            luaAction = luaAction.substr(start);
                        }
                        // Trim quotes if present
                        if (!luaAction.empty() && luaAction[0] == '"') {
                            luaAction = luaAction.substr(1);
                        }
                        if (!luaAction.empty() && luaAction.back() == '"') {
                            luaAction.pop_back();
                        }
                        break; // action consumes rest of line
                    } else {
                        m_output("Unknown modifier: " + modifier + "\n");
                        return;
                    }
                }

                int id = m_ctx.dbg->breakpoints().add(addr, BreakpointType::EXEC, physical);
                if (hitCountLimit > 0) {
                    m_ctx.dbg->breakpoints().setHitCountLimit(id, hitCountLimit);
                }
                if (!luaAction.empty()) {
                    m_ctx.dbg->breakpoints().setLuaAction(id, luaAction);
                }

                std::string prefix = physical ? "Physical breakpoint " : "Breakpoint ";
                m_output(prefix + std::to_string(id) + " at $" + toHex(addr, physical ? 7 : addrWidth()));
                if (hitCountLimit > 0) {
                    m_output(" (stops at hit " + std::to_string(hitCountLimit) + ")");
                }
                if (!luaAction.empty()) {
                    m_output(" [Lua action]");
                }
                m_output("\n");
            } else {
                m_output("Error: Invalid address '" + expr + "'\n");
            }
        } else {
            m_output("Syntax: break <address> [count N]\n");
            m_output("        break when <condition> [count N]\n");
            m_output("        break at line <N> [count N]\n");
            m_output("        break at <file>:<N> [count N]\n");
            m_output("        break phys <physical_address>\n");
        }
    } else if (cmd == "delete") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        int id;
        if (ss >> id) {
            m_ctx.dbg->breakpoints().remove(id);
            m_output("Deleted breakpoint " + std::to_string(id) + "\n");
        } else {
            m_output("Syntax: delete <id>\n");
        }
    } else if (cmd == "clear") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        std::string expr;
        if (ss >> expr) {
            uint32_t addr;
            if (parseAddr(expr, addr)) {
                m_ctx.dbg->breakpoints().removeByAddress(addr);
                m_output("Cleared breakpoint at $" + toHex(addr, addrWidth()) + "\n");
            } else {
                m_output("Error: Invalid address '" + expr + "'\n");
            }
        } else {
            m_output("Syntax: clear <address>\n");
        }
    } else if (cmd == "enable") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        int id;
        if (ss >> id) {
            m_ctx.dbg->breakpoints().setEnabled(id, true);
            m_output("Enabled breakpoint " + std::to_string(id) + "\n");
        } else {
            m_output("Syntax: enable <id>\n");
        }
    } else if (cmd == "disable") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        int id;
        if (ss >> id) {
            m_ctx.dbg->breakpoints().setEnabled(id, false);
            m_output("Disabled breakpoint " + std::to_string(id) + "\n");
        } else {
            m_output("Syntax: disable <id>\n");
        }
    } else if (cmd == "watch") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        std::string typeStr;
        if (!(ss >> typeStr)) {
            m_output("Syntax: watch <read|write|value> [phys] <address> [size]\n");
            return;
        }

        // Handle value watch: watch value <addr> <size>
        if (typeStr == "value") {
            std::string addrStr;
            uint32_t size;
            if (ss >> addrStr >> size) {
                uint32_t addr;
                if (parseAddr(addrStr, addr)) {
                    int id = m_ctx.dbg->breakpoints().addWatch(addr, size);
                    m_output("Value watch " + std::to_string(id) + " at $" + toHex(addr, addrWidth()) +
                             " for " + std::to_string(size) + " bytes\n");
                } else {
                    m_output("Error: Invalid address '" + addrStr + "'\n");
                }
            } else {
                m_output("Syntax: watch value <address> <size>\n");
            }
            return;
        }

        // Handle read/write watches
        std::string expr;
        if (!(ss >> expr)) {
            m_output("Syntax: watch <read|write|value> [phys] <address> [size]\n");
            return;
        }

        // Check for "phys" modifier: watch read phys $addr
        bool physical = false;
        if (expr == "phys") {
            physical = true;
            if (!(ss >> expr)) {
                m_output("Syntax: watch <read|write> [phys] <address>\n");
                return;
            }
        }

        uint32_t addr;
        if (parseAddr(expr, addr)) {
            BreakpointType type;
            if (typeStr == "read") {
                type = BreakpointType::READ_WATCH;
            } else if (typeStr == "write") {
                type = BreakpointType::WRITE_WATCH;
            } else {
                m_output("Syntax: watch <read|write|value> [phys] <address> [size]\n");
                return;
            }
            int id = m_ctx.dbg->breakpoints().add(addr, type, physical);
            std::string prefix = physical ? "Physical watchpoint " : "Watchpoint ";
            m_output(prefix + std::to_string(id) + " at $" + toHex(addr, physical ? 7 : addrWidth()) + "\n");
        } else {
            m_output("Error: Invalid address '" + expr + "'\n");
        }
    } else if (cmd == "print") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        std::string varName;
        if (ss >> varName) {
            printVariable(varName);
        } else {
            m_output("Usage: print <variable_name>\n");
        }
    } else if (cmd == "list") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        std::string args;
        if (ss >> std::ws && std::getline(ss, args)) {
            // Remove leading/trailing whitespace
            size_t start = args.find_first_not_of(" \t");
            size_t end = args.find_last_not_of(" \t");
            if (start != std::string::npos) {
                args = args.substr(start, end - start + 1);
                handleListCommand(args);
            } else {
                showCurrentSource();
            }
        } else {
            showCurrentSource();
        }
    } else if (cmd == "stack") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        int n = 8;
        std::string nStr;
        if (ss >> nStr) { try { n = std::stoi(nStr); } catch (...) {} }
        auto& st = m_ctx.dbg->stackTrace();
        auto entries = st.recent(n);
        if (entries.empty()) {
            m_output("Stack: empty\n");
        } else {
            m_output("Stack (depth " + std::to_string(st.depth()) +
                     ", showing " + std::to_string(entries.size()) + "):\n");
            std::stringstream out;
            out << std::hex << std::uppercase << std::setfill('0');
            for (int i = 0; i < (int)entries.size(); ++i) {
                const auto& e = entries[i];
                out << "  " << std::dec << std::setw(3) << i << "  "
                    << std::left << std::setw(5) << stackPushTypeName(e.type) << "  ";
                if (e.type == StackPushType::CALL || e.type == StackPushType::BRK)
                    out << "$" << std::hex << std::setw(4) << e.value;
                else
                    out << "$" << std::hex << std::setw(2) << e.value;
                out << "  pushed by $" << std::setw(4) << e.pushedByPc << "\n";
            }
            m_output(out.str());
        }
    } else if (cmd == "frame") {
        if (!m_ctx.dbg || !m_ctx.bus || !m_ctx.cpu) {
            m_output("No machine created.\n");
            return;
        }
        std::string sub;
        bool verbose = false;
        if (ss >> sub) {
            if (sub == "verbose") {
                verbose = true;
            } else {
                m_output("Usage: frame [verbose]\n");
                return;
            }
        }

        // Estimate frame pointer and size
        uint32_t framePointer = 0x100;  // C64 stack page default
        uint32_t frameSize = 256;

        auto layout = FrameLayoutAnalyzer::analyzeCurrentFrame(m_ctx.dbg, m_ctx.bus, framePointer, frameSize);
        if (layout.empty()) {
            m_output("No frame information available.\n");
            return;
        }

        if (verbose) {
            m_output(FrameLayoutAnalyzer::formatFrameLayout(layout, framePointer, frameSize));
            m_output("\n");
            m_output(FrameLayoutAnalyzer::formatAsStructDefinition(layout));
        } else {
            // Simple format: just the struct definition
            m_output(FrameLayoutAnalyzer::formatAsStructDefinition(layout));
        }
    } else if (cmd == "sym") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        std::string sub;
        if (ss >> sub) {
            if (sub == "add") {
                std::string label, expr;
                if (ss >> label >> expr) {
                    uint32_t addr;
                    if (parseAddr(expr, addr)) {
                        m_ctx.dbg->symbols().addSymbol(addr, label);
                        m_output("Symbol added: " + label + " = $" + toHex(addr, addrWidth()) + "\n");
                    } else {
                        m_output("Error: Invalid address '" + expr + "'\n");
                    }
                } else {
                    m_output("Usage: sym add <label> <address>\n");
                }
            } else if (sub == "del") {
                std::string label;
                if (ss >> label) {
                    m_ctx.dbg->symbols().removeSymbol(label);
                    m_output("Symbol removed: " + label + "\n");
                } else {
                    m_output("Usage: sym del <label>\n");
                }
            } else if (sub == "list") {
                auto syms = m_ctx.dbg->symbols().symbols();
                if (syms.empty()) {
                    m_output("No symbols defined.\n");
                } else {
                    std::stringstream out;
                    int aw = addrWidth();
                    out << std::setfill('0') << std::uppercase << std::hex;
                    for (const auto& pair : syms) {
                        out << "$" << std::setw(aw) << pair.first << "  " << pair.second << "\n";
                    }
                    m_output(out.str());
                }
            } else if (sub == "search") {
                std::string query;
                if (ss >> query) {
                    auto syms = m_ctx.dbg->symbols().symbols();
                    std::stringstream out;
                    int aw = addrWidth();
                    bool found = false;
                    for (const auto& pair : syms) {
                        if (pair.second.find(query) != std::string::npos) {
                            out << "$" << std::hex << std::uppercase << std::setw(aw) << std::setfill('0') << pair.first << "  " << pair.second << "\n";
                            found = true;
                        }
                    }
                    if (found) m_output(out.str());
                    else m_output("No symbols matching '" + query + "' found.\n");
                } else {
                    m_output("Usage: sym search <query>\n");
                }
            } else if (sub == "load") {
                std::string path;
                if (ss >> path) {
                    if (m_ctx.dbg->symbols().loadSym(path)) {
                        m_output("Symbols loaded from: " + path + "\n");
                    } else {
                        m_output("Failed to load symbols from: " + path + "\n");
                    }
                } else {
                    m_output("Usage: sym load <path>\n");
                }
            } else if (sub == "load-c64ide") {
                // Load C64IDE symbol database based on current machine
                std::string machine_type;
                if (m_ctx.machine) {
                    machine_type = m_ctx.machine->machineId;
                }

                std::string sym_path;
                if (machine_type == "c64") {
                    sym_path = "roms/c64/c64ide_symbols.sym";
                } else if (machine_type == "vic20") {
                    // Could add vic20ide_symbols.sym in future
                    m_output("C64IDE symbols not available for VIC-20.\n");
                    return;
                } else {
                    m_output("C64IDE symbols not available for this machine.\n");
                    return;
                }

                if (m_ctx.dbg->symbols().loadSym(sym_path)) {
                    m_output("Loaded C64IDE symbols from: " + sym_path + "\n");
                } else {
                    m_output("Failed to load C64IDE symbols from: " + sym_path + "\n");
                    m_output("Make sure the file exists at: " + sym_path + "\n");
                }
            } else if (sub == "load-o45") {
                std::string path;
                if (ss >> path) {
                    if (m_ctx.dbg->loadDebugSymbolsFromO45(path)) {
                        m_output("Debug symbols loaded from .o45 file: " + path + "\n");
                    } else {
                        m_output("Failed to load debug symbols from .o45 file: " + path + "\n");
                    }
                } else {
                    m_output("Usage: sym load-o45 <path>\n");
                }
            } else if (sub == "clear") {
                m_ctx.dbg->symbols().clear();
                m_output("Symbol table cleared.\n");
            } else {
                m_output("Unknown sym subcommand: " + sub + "\n");
            }
        } else {
            m_output("Usage: sym <add|del|list|search|load|load-c64ide|load-o45|clear>\n");
        }
    } else if (cmd == "tape") {
        if (!m_ctx.machine) { m_output("No machine created.\n"); return; }
        std::string sub;
        if (ss >> sub) {
            IOHandler* tape = m_ctx.machine->ioRegistry ? m_ctx.machine->ioRegistry->findHandler("Tape") : nullptr;
            if (!tape) {
                m_output("No datasette found in this machine.\n");
                return;
            }
            if (sub == "mount") {
                std::string path;
                if (ss >> path) {
                    if (tape->mountTape(path)) {
                        m_output("Mounted tape: " + path + "\n");
                    } else {
                        m_output("Failed to mount tape: " + path + "\n");
                    }
                } else {
                    m_output("Usage: tape mount <path>\n");
                }
            } else if (sub == "record") {
                if (tape->startTapeRecord()) {
                    m_output("Tape: recording started\n");
                } else {
                    m_output("Tape: failed to start recording (write line not connected?)\n");
                }
            } else if (sub == "stoprecord") {
                tape->stopTapeRecord();
                m_output("Tape: recording stopped\n");
            } else if (sub == "save") {
                std::string path;
                if (ss >> path) {
                    if (tape->saveTapeRecording(path)) {
                        m_output("Tape recording saved: " + path + "\n");
                    } else {
                        m_output("Failed to save tape recording\n");
                    }
                } else {
                    m_output("Usage: tape save <path>\n");
                }
            } else {
                tape->controlTape(sub);
                m_output("Tape: " + sub + "\n");
            }
        } else {
            m_output("Usage: tape <mount|play|stop|rewind|record|stoprecord|save>\n");
        }
    } else if (cmd == "disk") {
        if (!m_ctx.machine) { m_output("No machine created.\n"); return; }
        std::string sub;
        if (ss >> sub) {
            if (sub == "mount") {
                int unit;
                std::string path;
                if (ss >> unit >> path) {
                    if (m_ctx.machine->ioRegistry) {
                        std::vector<IOHandler*> handlers;
                        m_ctx.machine->ioRegistry->enumerate(handlers);
                        bool handled = false;
                        for (auto* handler : handlers) {
                            if (handler->mountDisk(unit, path)) {
                                m_output("Mounted disk '" + path + "' on unit " + std::to_string(unit) + "\n");
                                handled = true;
                                break;
                            }
                        }
                        if (!handled) {
                            m_output("Failed to mount disk on unit " + std::to_string(unit) + " (unit not found or mount failed)\n");
                        }
                    }
                } else {
                    m_output("Usage: disk mount <unit> <path>\n");
                }
            } else if (sub == "eject") {
                int unit;
                if (ss >> unit) {
                    if (m_ctx.machine->ioRegistry) {
                        std::vector<IOHandler*> handlers;
                        m_ctx.machine->ioRegistry->enumerate(handlers);
                        for (auto* handler : handlers) {
                            handler->ejectDisk(unit);
                        }
                        m_output("Ejected disk from unit " + std::to_string(unit) + "\n");
                    }
                } else {
                    m_output("Usage: disk eject <unit>\n");
                }
            } else {
                m_output("Usage: disk <mount|eject>\n");
            }
        } else {
            m_output("Usage: disk <mount|eject>\n");
        }
    } else if (cmd == "cart") {
        if (!m_ctx.bus) { m_output("No machine created.\n"); return; }
        std::string path;
        if (ss >> path) {
            if (auto handler = ImageLoaderRegistry::instance().createCartridgeHandler(path)) {
                if (handler->attach(m_ctx.bus, m_ctx.machine)) {
                    auto md = handler->metadata();
                    m_output("Attached cartridge: " + md.displayName + " (" + md.type + ")\n");
                    ImageLoaderRegistry::instance().setActiveCartridge(m_ctx.bus, std::move(handler));
                    // Optional: Trigger reset
                    m_output("Resetting machine...\n");
                    m_ctx.machine->onReset(*m_ctx.machine);
                    showRegisters();
                } else {
                    m_output("Failed to attach cartridge '" + path + "'\n");
                }
            } else {
                m_output("Unsupported cartridge format: '" + path + "'\n");
            }
        } else {
            m_output("Syntax: cart <path>\n");
        }
    } else if (cmd == "eject") {
        if (!m_ctx.bus) { m_output("No machine created.\n"); return; }
        if (auto* cart = ImageLoaderRegistry::instance().getActiveCartridge(m_ctx.bus)) {
            cart->eject(m_ctx.bus);
            ImageLoaderRegistry::instance().setActiveCartridge(m_ctx.bus, nullptr);
            m_output("Cartridge ejected.\n");
            m_output("Resetting machine...\n");
            m_ctx.machine->onReset(*m_ctx.machine);
            showRegisters();
        } else {
            m_output("No cartridge attached.\n");
        }
    } else if (cmd == "screenshot") {
        if (!m_ctx.machine) { m_output("No machine created.\n"); return; }
        std::string filename;
        if (ss >> filename) {
            IVideoOutput* video = nullptr;
            if (m_ctx.machine->ioRegistry) {
                std::vector<IOHandler*> handlers;
                m_ctx.machine->ioRegistry->enumerate(handlers);
                for (auto* handler : handlers) {
                    video = dynamic_cast<IVideoOutput*>(handler);
                    if (video) break;
                }
            }
            if (video) {
                if (video->exportPng(filename)) {
                    m_output("Screenshot saved to " + filename + "\n");
                } else {
                    m_output("Failed to save screenshot to " + filename + "\n");
                }
            } else {
                m_output("No video output device found for this machine.\n");
            }
        } else {
            m_output("Syntax: screenshot <filename.png>\n");
        }
    } else if (cmd == "setpc") {
        if (!m_ctx.cpu) { m_output("No machine created.\n"); return; }
        std::string expr;
        if (ss >> expr) {
            uint32_t addr;
            if (parseAddr(expr, addr)) {
                m_ctx.cpu->setPc(addr);
                showRegisters();
            } else {
                m_output("Error: Invalid address '" + expr + "'\n");
            }
        } else {
            m_output("Syntax: setpc <address>\n");
        }
    } else if (cmd == "regs") {
        if (!m_ctx.cpu) { m_output("No machine created.\n"); return; }
        showRegisters();
    } else if (cmd == "m") {
        if (!m_ctx.bus) { m_output("No machine created.\n"); return; }
        std::string expr;
        uint32_t addr = 0;
        if (ss >> expr) {
            // Handle DEVICE.? — list all derived values for a device
            auto dotPos = expr.find('.');
            if (dotPos != std::string::npos && expr.substr(dotPos + 1) == "?" && m_ctx.machine && m_ctx.machine->ioRegistry) {
                std::string devPart = expr.substr(0, dotPos);
                std::transform(devPart.begin(), devPart.end(), devPart.begin(), ::toupper);
                std::vector<IOHandler*> handlers;
                m_ctx.machine->ioRegistry->enumerate(handlers);
                bool found = false;
                for (auto* h : handlers) {
                    std::string hn = h->name();
                    std::string hnUpper = hn;
                    std::transform(hnUpper.begin(), hnUpper.end(), hnUpper.begin(), ::toupper);
                    bool match = (hnUpper == devPart);
                    if (!match) {
                        for (auto& alias : h->deviceAliases()) {
                            std::string au = alias;
                            std::transform(au.begin(), au.end(), au.begin(), ::toupper);
                            if (au == devPart) { match = true; break; }
                        }
                    }
                    if (match) {
                        found = true;
                        auto vals = h->getDerivedValues();
                        if (vals.empty()) {
                            m_output(hn + ": no derived values defined.\n");
                        } else {
                            m_output(hn + " derived values:\n");
                            for (auto& [name, val] : vals) {
                                m_output("  " + hn + "." + name + " = $" + toHex(val, addrWidth()) + "\n");
                            }
                        }
                        // Also show registers from DeviceInfo
                        DeviceInfo info;
                        h->getDeviceInfo(info);
                        if (!info.registers.empty()) {
                            m_output(hn + " registers:\n");
                            for (auto& r : info.registers) {
                                m_output("  $" + toHex(info.baseAddr + r.offset, 4) + " " + r.name +
                                         " = $" + toHex(r.value, 2) + "  " + r.description + "\n");
                            }
                        }
                        break;
                    }
                }
                if (!found) {
                    m_output("Unknown device '" + devPart + "'. Available:\n");
                    for (auto* h : handlers) m_output("  " + std::string(h->name()) + "\n");
                }
            } else if (parseAddr(expr, addr)) {
                uint32_t len = 64;
                if (ss >> expr) {
                    uint32_t l;
                    if (parseAddr(expr, l)) len = l;
                }
                dumpMemory(addr, len);
            } else {
                m_output("Error: Invalid address '" + expr + "'\n");
            }
        }
    } else if (cmd == "f") {
        if (!m_ctx.bus) { m_output("No machine created.\n"); return; }
        std::string addrExpr, valExpr, lenExpr;
        if (ss >> addrExpr >> valExpr) {
            uint32_t addr, val;
            if (parseAddr(addrExpr, addr) && parseAddr(valExpr, val)) {
                uint32_t len = 1;
                if (ss >> lenExpr) {
                    uint32_t l;
                    if (parseAddr(lenExpr, l)) len = l;
                }
                for (uint32_t i = 0; i < len; ++i) m_ctx.bus->write8(addr + i, (uint8_t)val);
            } else {
                m_output("Error: Invalid address or value expression.\n");
            }
        }
    } else if (cmd == "copy") {
        if (!m_ctx.bus) { m_output("No machine created.\n"); return; }
        std::string srcExpr, dstExpr, lenExpr;
        if (ss >> srcExpr >> dstExpr >> lenExpr) {
            uint32_t src, dst, len;
            if (parseAddr(srcExpr, src) && parseAddr(dstExpr, dst) && parseAddr(lenExpr, len)) {
                std::vector<uint8_t> tmp(len);
                for (uint32_t i = 0; i < len; ++i) tmp[i] = m_ctx.bus->peek8(src + i);
                for (uint32_t i = 0; i < len; ++i) m_ctx.bus->write8(dst + i, tmp[i]);
            } else {
                m_output("Error: Invalid expression in copy command.\n");
            }
        }
    } else if (cmd == "swap") {
        if (!m_ctx.bus) { m_output("No machine created.\n"); return; }
        std::string a1Expr, a2Expr, lenExpr;
        if (ss >> a1Expr >> a2Expr >> lenExpr) {
            uint32_t addr1, addr2, len;
            if (parseAddr(a1Expr, addr1) && parseAddr(a2Expr, addr2) && parseAddr(lenExpr, len)) {
                std::vector<uint8_t> tmp(len);
                for (uint32_t i = 0; i < len; ++i) {
                    uint8_t v1 = m_ctx.bus->read8(addr1 + i);
                    uint8_t v2 = m_ctx.bus->read8(addr2 + i);
                    tmp[i] = v1;
                    m_ctx.bus->write8(addr1 + i, v2);
                }
                for (uint32_t i = 0; i < len; ++i) m_ctx.bus->write8(addr2 + i, tmp[i]);
                m_output("Swapped.\n");
            } else {
                m_output("Error: Invalid expression in swap command.\n");
            }
        } else {
            m_output("Syntax: swap <addr1> <addr2> <len>\n");
        }
    } else if (cmd == "search") {
        if (!m_ctx.bus) { m_output("No machine created.\n"); return; }
        uint32_t mask = m_ctx.bus->config().addrMask;
        std::string patternStr;
        std::vector<uint8_t> pattern;
        while (ss >> patternStr) {
            try {
                pattern.push_back((uint8_t)std::stoul(patternStr, nullptr, 16));
            } catch (...) {}
        }
        if (pattern.empty()) {
            m_output("Syntax: search <hex1> [hex2] ...\n");
            return;
        }
        m_lastSearchPattern = pattern;
        m_lastSearchFoundAddr = 0xFFFFFFFF;
        for (uint32_t i = 0; i + pattern.size() <= mask + 1; ++i) {
            bool match = true;
            for (size_t j = 0; j < pattern.size(); ++j) {
                if (m_ctx.bus->peek8((i + j) & mask) != pattern[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                m_output("Found at $" + toHex(i, addrWidth()) + "\n");
                if (m_lastSearchFoundAddr == 0xFFFFFFFF) m_lastSearchFoundAddr = i;
            }
        }
        if (m_lastSearchFoundAddr == 0xFFFFFFFF) m_output("Pattern not found.\n");
    } else if (cmd == "searcha") {
        if (!m_ctx.bus) { m_output("No machine created.\n"); return; }
        uint32_t mask = m_ctx.bus->config().addrMask;
        std::string pattern;
        std::getline(ss, pattern);
        if (!pattern.empty() && pattern[0] == ' ') pattern = pattern.substr(1);
        if (pattern.empty()) {
            m_output("Syntax: searcha <ascii_string>\n");
            return;
        }
        m_lastSearchPattern.assign(pattern.begin(), pattern.end());
        m_lastSearchFoundAddr = 0xFFFFFFFF;
        for (uint32_t i = 0; i + pattern.size() <= mask + 1; ++i) {
            bool match = true;
            for (size_t j = 0; j < pattern.size(); ++j) {
                if (m_ctx.bus->peek8((i + j) & mask) != (uint8_t)pattern[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                m_output("Found at $" + toHex(i, addrWidth()) + "\n");
                if (m_lastSearchFoundAddr == 0xFFFFFFFF) m_lastSearchFoundAddr = i;
            }
        }
        if (m_lastSearchFoundAddr == 0xFFFFFFFF) m_output("Pattern not found.\n");
    } else if (cmd == "findnext") {
        if (!m_ctx.bus) { m_output("No machine created.\n"); return; }
        if (m_lastSearchPattern.empty()) { m_output("No previous search.\n"); return; }
        uint32_t mask = m_ctx.bus->config().addrMask;
        uint32_t start = (m_lastSearchFoundAddr == 0xFFFFFFFF) ? 0 : (m_lastSearchFoundAddr + 1) & mask;
        for (uint32_t i = 0; i <= mask; ++i) {
            uint32_t curr = (start + i) & mask;
            if (curr + m_lastSearchPattern.size() > mask + 1) continue;
            bool match = true;
            for (size_t j = 0; j < m_lastSearchPattern.size(); ++j) {
                if (m_ctx.bus->peek8((curr + j) & mask) != m_lastSearchPattern[j]) {
                    match = false; break;
                }
            }
            if (match) {
                m_lastSearchFoundAddr = curr;
                m_output("Found at $" + toHex(curr, addrWidth()) + "\n");
                break;
            }
            if (i == mask) m_output("No further occurrences found.\n");
        }
    } else if (cmd == "findprior") {
        if (!m_ctx.bus) { m_output("No machine created.\n"); return; }
        if (m_lastSearchPattern.empty()) { m_output("No previous search.\n"); return; }
        uint32_t mask = m_ctx.bus->config().addrMask;
        uint32_t start = (m_lastSearchFoundAddr == 0xFFFFFFFF) ? mask : (m_lastSearchFoundAddr - 1) & mask;
        bool found = false;
        for (uint32_t i = 0; i <= mask; ++i) {
            uint32_t curr = (start - i) & mask;
            if (curr + m_lastSearchPattern.size() > mask + 1) continue;
            bool match = true;
            for (size_t j = 0; j < m_lastSearchPattern.size(); ++j) {
                if (m_ctx.bus->peek8((curr + j) & mask) != m_lastSearchPattern[j]) {
                    match = false; break;
                }
            }
            if (match) {
                m_lastSearchFoundAddr = curr;
                m_output("Found at $" + toHex(curr, addrWidth()) + "\n");
                found = true;
                break;
            }
        }
        if (!found) m_output("No prior occurrences found.\n");
    } else if (cmd == "disasm") {
        if (!m_ctx.cpu || !m_ctx.disasm) { m_output("No machine created.\n"); return; }
        std::string expr;
        uint32_t addr = m_ctx.cpu->pc();
        int n = 10;
        if (ss >> expr) {
            if (parseAddr(expr, addr)) {
                if (ss >> n) {}
            } else {
                m_output("Error: Invalid address '" + expr + "'\n");
                return;
            }
        }
        for (int i = 0; i < n; ++i) {
            char buf[64];
            std::stringstream res;
            res << std::hex << std::uppercase << std::setfill('0') << std::setw(addrWidth()) << addr << ": ";
            int bytes = m_ctx.disasm->disasmOne(m_ctx.bus, addr, buf, sizeof(buf));
            res << buf << "\n";
            m_output(res.str());
            addr += bytes;
        }
    } else if (cmd == "asm") {
        if (!m_ctx.cpu || !m_ctx.assem) { m_output("No machine created.\n"); return; }
        std::string expr;
        if (ss >> expr) {
            if (expr == "file") {
                // File-based assembly: asm file <path> [loadAddr]
                std::string path;
                if (!(ss >> path)) { m_output("Syntax: asm file <path> [loadAddr]\n"); return; }
                std::string outputPath = path + ".prg";
                AssemblerResult res = m_ctx.assem->assemble(path, outputPath);
                if (res.success) {
                    // Load the .prg into memory
                    uint32_t loadAddr = m_asmAddr;
                    std::string addrStr;
                    if (ss >> addrStr) parseAddr(addrStr, loadAddr);

                    FILE* f = fopen(res.outputPath.c_str(), "rb");
                    if (f) {
                        // .prg format: first 2 bytes = load address (little-endian)
                        uint8_t lo = 0, hi = 0;
                        if (fread(&lo, 1, 1, f) == 1 && fread(&hi, 1, 1, f) == 1) {
                            loadAddr = (uint16_t)lo | ((uint16_t)hi << 8);
                        }
                        int count = 0;
                        uint8_t byte;
                        while (fread(&byte, 1, 1, f) == 1) {
                            m_ctx.bus->write8(loadAddr + count, byte);
                            count++;
                        }
                        fclose(f);
                        std::stringstream msg;
                        msg << "Assembled " << count << " bytes, loaded at $"
                            << std::hex << std::uppercase << std::setfill('0')
                            << std::setw(4) << loadAddr << "\n";
                        m_output(msg.str());
                    } else {
                        m_output("Assembly succeeded but could not read output: " + res.outputPath + "\n");
                    }
                } else {
                    m_output("Assembly failed: " + res.errorMessage + "\n");
                }
            } else {
                uint32_t addr;
                if (parseAddr(expr, addr)) {
                    m_asmAddr = addr;
                    m_asmMode = true;
                } else {
                    m_output("Error: Invalid address '" + expr + "'\n");
                    return;
                }
            }
        } else {
            m_output("Syntax: asm <address> | asm file <path>\n");
        }
    } else if (cmd == "config") {
        std::string subcmd;
        if (ss >> subcmd) {
            if (subcmd == "assembler") {
                std::string name;
                if (ss >> name) {
                    m_ctx.assemblerOverride = name;
                    // Re-resolve assembler with the new override
                    if (m_ctx.cpu && m_ctx.machine) {
                        if (m_ctx.assem) { delete m_ctx.assem; m_ctx.assem = nullptr; }
                        m_ctx.assem = resolveAssembler(m_ctx.cpu->isaName(), m_ctx.machine->preferredAssembler, m_ctx.assemblerOverride);
                    }
                    m_output("Assembler override set to: " + name + "\n");
                } else {
                    // Show current assembler and available list
                    std::string current = m_ctx.assemblerOverride.empty()
                        ? (m_ctx.machine && !m_ctx.machine->preferredAssembler.empty()
                            ? m_ctx.machine->preferredAssembler
                            : "default")
                        : m_ctx.assemblerOverride;
                    m_output("Current assembler: " + current + "\n");
                    auto names = ToolchainRegistry::instance().getAssemblerNames();
                    if (!names.empty()) {
                        m_output("Available: ");
                        for (size_t i = 0; i < names.size(); ++i) {
                            if (i > 0) m_output(", ");
                            m_output(names[i]);
                        }
                        m_output("\n");
                    }
                }
            } else {
                m_output("Unknown config: " + subcmd + "\n");
            }
        } else {
            m_output("Usage: config <assembler> [name]\n");
        }
    } else if (cmd == "type") {
        if (!m_ctx.machine) { m_output("No machine created.\n"); return; }
        std::string text;
        std::getline(ss, text);
        if (!text.empty() && text[0] == ' ') text = text.substr(1);
        
        IKeyboardMatrix* kbd = nullptr;
        if (m_ctx.machine->ioRegistry) {
            std::vector<IOHandler*> handlers;
            m_ctx.machine->ioRegistry->enumerate(handlers);
            for (auto* h : handlers) {
                if ((kbd = dynamic_cast<IKeyboardMatrix*>(h))) break;
            }
        }
        
        if (kbd) {
            kbd->enqueueText(text);
            m_output("Enqueued text for typing.\n");
        } else {
            m_output("No keyboard device found in this machine.\n");
        }
    } else if (cmd == "key") {
        if (!m_ctx.machine || !m_ctx.machine->onKey) { m_output("No machine with keyboard created.\n"); return; }
        std::string keyName;
        if (std::string state; ss >> keyName >> state) {
            bool down = (state == "down" || state == "1");
            if (!m_ctx.machine->onKey(keyName, down)) {
                m_output("Unknown key: " + keyName + "\n");
            }
        } else {
            m_output("Syntax: key <name> <down|up|1|0>\n");
        }
    } else if (cmd.size() > 5 && cmd.substr(cmd.size() - 5) == ".info") {
        std::string deviceName = cmd.substr(0, cmd.size() - 5);
        if (m_ctx.machine && m_ctx.machine->ioRegistry) {
            IOHandler* handler = m_ctx.machine->ioRegistry->findHandler(deviceName);
            if (!handler) {
                std::vector<IOHandler*> handlers;
                m_ctx.machine->ioRegistry->enumerate(handlers);
                for (auto* h : handlers) {
                    std::string hname = h->name();
                    std::string target = deviceName;
                    std::transform(hname.begin(), hname.end(), hname.begin(), ::tolower);
                    std::transform(target.begin(), target.end(), target.begin(), ::tolower);
                    if (hname == target || hname.find(target) != std::string::npos) {
                        handler = h;
                        break;
                    }
                }
            }
            if (handler) {
                DeviceInfo info;
                handler->getDeviceInfo(info);
                m_output("Device: " + info.name + "\n");
                m_output("Base Address: $" + toHex(info.baseAddr) + "\n");
                m_output("Address Mask: $" + toHex(info.addrMask) + "\n");
                if (!info.dependencies.empty()) {
                    m_output("\nDependencies:\n");
                    for (const auto& d : info.dependencies) m_output("  " + d.first + ": " + d.second + "\n");
                }
                if (!info.state.empty()) {
                    m_output("\nInternal State:\n");
                    for (const auto& s : info.state) m_output("  " + s.first + ": " + s.second + "\n");
                }
                if (!info.registers.empty()) {
                    m_output("\nRegisters:\n");
                    for (size_t i = 0; i < info.registers.size(); ++i) {
                        const auto& r = info.registers[i];
                        m_output("  " + r.name + " ($" + toHex(r.offset, 2) + "): $" + toHex(r.value, 2));
                        if (!r.description.empty()) m_output(" (" + r.description + ")");
                        m_output("  ");
                        if ((i + 1) % 3 == 0) m_output("\n");
                    }
                    if (info.registers.size() % 3 != 0) m_output("\n");
                }
                return;
            } else {
                m_output("Device '" + deviceName + "' not found.\n");
            }
        } else {
            m_output("No machine or I/O registry available.\n");
        }
    } else if (cmd == "regwrite") {
        if (!m_ctx.cpu) { m_output("No machine created.\n"); return; }
        std::string regName, valExpr;
        if (ss >> regName >> valExpr) {
            uint32_t val;
            if (ExpressionEvaluator::evaluate(valExpr, m_ctx.dbg, val)) {
                int idx = m_ctx.cpu->regIndexByName(regName.c_str());
                if (idx >= 0) {
                    m_ctx.cpu->regWrite(idx, val);
                    showRegisters();
                } else {
                    m_output("Error: Unknown register '" + regName + "'\n");
                }
            } else {
                m_output("Error: Invalid expression '" + valExpr + "'\n");
            }
        } else {
            m_output("Syntax: regwrite <name> <value>\n");
        }
    } else if (cmd == "trace") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        std::string sub;
        if (ss >> sub) {
            if (sub == "dump") {
                int n = 16;
                std::string nStr;
                if (ss >> nStr) { try { n = std::stoi(nStr); } catch (...) {} }
                auto& buf = m_ctx.dbg->trace();
                if (buf.size() == 0) { m_output("Trace buffer empty.\n"); return; }

                // Collect entries matching filter, most recent first
                std::vector<size_t> indices;
                for (size_t i = buf.size(); i > 0 && (int)indices.size() < n; --i) {
                    size_t idx = i - 1;
                    const auto& e = buf.at(idx);
                    if (m_traceFilter == "calls") {
                        auto& m = e.mnemonic;
                        if (m.find("JSR") == std::string::npos && m.find("RTS") == std::string::npos &&
                            m.find("RTI") == std::string::npos && m.find("BSR") == std::string::npos)
                            continue;
                    } else if (m_traceFilter == "io") {
                        // Show only instructions that access I/O ($D000-$DFFF)
                        bool hasIo = false;
                        for (auto& mw : e.memWrites) {
                            if (mw.addr >= 0xD000 && mw.addr <= 0xDFFF) { hasIo = true; break; }
                        }
                        if (!hasIo) continue;
                    }
                    indices.push_back(idx);
                }
                std::reverse(indices.begin(), indices.end());

                if (indices.empty()) { m_output("No matching entries (filter: " + m_traceFilter + ").\n"); return; }
                for (size_t idx : indices) {
                    const auto& e = buf.at(idx);
                    std::ostringstream os;
                    os << "$" << toHex(e.addr, addrWidth()) << ": " << e.mnemonic;
                    if (!e.regs.empty()) {
                        os << "  [";
                        bool first = true;
                        for (auto& [rn, rv] : e.regs) {
                            if (!first) os << " ";
                            os << rn << "=$" << toHex(rv, 2);
                            first = false;
                        }
                        os << "]";
                    }
                    os << "\n";
                    m_output(os.str());
                }
            } else if (sub == "clear") {
                m_ctx.dbg->trace().clear();
                m_output("Trace buffer cleared.\n");
            } else if (sub == "filter") {
                std::string filter;
                if (ss >> filter) {
                    if (filter == "all" || filter == "calls" || filter == "io") {
                        m_traceFilter = filter;
                        m_output("Trace filter set to: " + filter + "\n");
                    } else {
                        m_output("Invalid filter. Valid: all, calls, io\n");
                    }
                } else {
                    m_output("Current trace filter: " + m_traceFilter + "\n");
                }
            } else if (sub == "size") {
                auto& buf = m_ctx.dbg->trace();
                m_output("Trace buffer: " + std::to_string(buf.size()) + "/" +
                         std::to_string(buf.capacity()) + " entries (filter: " + m_traceFilter + ")\n");
            } else {
                m_output("Syntax: trace <dump [n]|clear|filter [mode]|size>\n");
            }
        } else {
            m_output("Syntax: trace <dump [n]|clear|filter [mode]|size>\n");
        }
    } else if (cmd == "snapshot") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        std::string sub;
        if (ss >> sub) {
            if (sub == "save") {
                std::string name;
                if (ss >> name) {
                    int idx = m_ctx.dbg->saveSnapshot(name);
                    m_output("Snapshot " + std::to_string(idx) + " '" + name + "' saved.\n");
                } else {
                    m_output("Syntax: snapshot save <name>\n");
                }
            } else if (sub == "list") {
                const auto& snaps = m_ctx.dbg->snapshots();
                if (snaps.empty()) { m_output("No snapshots.\n"); return; }
                for (size_t i = 0; i < snaps.size(); ++i) {
                    m_output("  " + std::to_string(i) + ": " + snaps[i].label + "\n");
                }
            } else if (sub == "diff") {
                int i1, i2;
                if (ss >> i1 >> i2) {
                    auto diffs = m_ctx.dbg->diffSnapshots(i1, i2);
                    if (diffs.empty()) {
                        m_output("Snapshots are identical.\n");
                    } else {
                        m_output("Memory diff: " + std::to_string(diffs.size()) + " bytes differ:\n");
                        int shown = 0;
                        for (uint32_t a : diffs) {
                            if (++shown > 32) { m_output("  ... (" + std::to_string(diffs.size() - 32) + " more)\n"); break; }
                            m_output("  $" + toHex(a, addrWidth()) + "\n");
                        }
                    }
                } else {
                    m_output("Syntax: snapshot diff <index1> <index2>\n");
                }
            } else if (sub == "delete") {
                std::string arg;
                if (ss >> arg) {
                    if (arg == "*") {
                        int count = m_ctx.dbg->snapshots().size();
                        m_ctx.dbg->clearSnapshots();
                        m_output("Deleted " + std::to_string(count) + " snapshot(s).\n");
                    } else {
                        int idx = std::stoi(arg);
                        if (m_ctx.dbg->deleteSnapshot(idx)) {
                            m_output("Deleted snapshot " + std::to_string(idx) + ".\n");
                        } else {
                            m_output("Error: Invalid snapshot index.\n");
                        }
                    }
                } else {
                    m_output("Syntax: snapshot delete <index|*>\n");
                }
            } else {
                m_output("Syntax: snapshot <save|list|diff|delete> ...\n");
            }
        } else {
            m_output("Syntax: snapshot <save|list|diff|delete> ...\n");
        }
    } else if (cmd == "analyze") {
        if (!m_ctx.cpu || !m_ctx.bus || !m_ctx.disasm) {
            m_output("No machine or disassembler available.\n"); return;
        }
        std::string sub;
        if (ss >> sub && sub == "routine") {
            std::string expr;
            if (ss >> expr) {
                uint32_t entryAddr;
                if (!ExpressionEvaluator::evaluate(expr, m_ctx.dbg, entryAddr)) {
                    m_output("Error: Invalid address expression.\n"); return;
                }
                int maxInsns = 200;
                std::string maxStr;
                if (ss >> maxStr) { try { maxInsns = std::stoi(maxStr); } catch (...) {} }
                if (maxInsns > 5000) maxInsns = 5000;

                uint32_t addrMask = m_ctx.bus->config().addrMask;
                SymbolTable* symTab = m_ctx.dbg ? &m_ctx.dbg->symbols() : nullptr;

                std::set<uint32_t> visited;
                struct QueueItem { uint32_t pc; int depth; };
                std::vector<QueueItem> queue;
                queue.push_back({entryAddr, 0});

                struct CallInfo { uint32_t target; uint32_t from; };
                struct LoopInfo { uint32_t branchAddr; uint32_t target; };

                std::vector<CallInfo> calls;
                std::vector<LoopInfo> loops;
                std::vector<uint32_t> exits;
                int totalInsns = 0;
                uint32_t minAddr = entryAddr, maxAddr = entryAddr;
                int branchCount = 0;

                while (!queue.empty() && totalInsns < maxInsns) {
                    auto item = queue.back(); queue.pop_back();
                    uint32_t pc = item.pc;

                    while (totalInsns < maxInsns) {
                        if (visited.count(pc)) break;
                        visited.insert(pc);

                        DisasmEntry entry;
                        int bytes = m_ctx.disasm->disasmEntry(m_ctx.bus, pc, entry);
                        if (bytes <= 0) break;
                        ++totalInsns;
                        if (pc < minAddr) minAddr = pc;
                        if (pc + bytes - 1 > maxAddr) maxAddr = pc + bytes - 1;

                        if (entry.isReturn) { exits.push_back(pc); break; }
                        else if (entry.isCall) {
                            calls.push_back({entry.targetAddr & addrMask, pc});
                            pc = (pc + bytes) & addrMask;
                        } else if (entry.isBranch) {
                            ++branchCount;
                            uint32_t target = entry.targetAddr & addrMask;
                            if (target <= pc) loops.push_back({pc, target});
                            if (!visited.count(target)) queue.push_back({target, 0});
                            pc = (pc + bytes) & addrMask;
                        } else if (m_ctx.bus->peek8(pc) == 0x4C) {
                            pc = entry.targetAddr & addrMask;
                        } else if (m_ctx.bus->peek8(pc) == 0x6C || m_ctx.bus->peek8(pc) == 0x00) {
                            exits.push_back(pc); break;
                        } else {
                            pc = (pc + bytes) & addrMask;
                        }
                    }
                }

                std::ostringstream os;
                os << "=== Routine at $" << toHex(entryAddr, addrWidth());
                if (symTab) { auto l = symTab->getLabel(entryAddr); if (!l.empty()) os << " (" << l << ")"; }
                os << " ===\n";
                os << "Size: " << (maxAddr - minAddr + 1) << " bytes ($"
                   << toHex(minAddr, addrWidth()) << "-$" << toHex(maxAddr, addrWidth())
                   << "), " << totalInsns << " instructions\n";
                if (totalInsns >= maxInsns) os << "NOTE: Truncated at " << maxInsns << " instructions\n";

                if (!calls.empty()) {
                    os << "\nCalls (" << calls.size() << "):\n";
                    std::map<uint32_t, int> cc;
                    for (auto& c : calls) cc[c.target]++;
                    for (auto& [t, n] : cc) {
                        os << "  JSR $" << toHex(t, addrWidth());
                        if (symTab) { auto l = symTab->getLabel(t); if (!l.empty()) os << " (" << l << ")"; }
                        if (n > 1) os << " x" << n;
                        os << "\n";
                    }
                }
                if (!loops.empty()) {
                    os << "\nLoops (" << loops.size() << "):\n";
                    for (auto& l : loops)
                        os << "  $" << toHex(l.target, addrWidth()) << "-$" << toHex(l.branchAddr, addrWidth()) << "\n";
                }
                if (!exits.empty()) {
                    os << "\nExits: ";
                    for (size_t i = 0; i < exits.size(); ++i) {
                        if (i) os << ", ";
                        uint8_t op = m_ctx.bus->peek8(exits[i]);
                        os << (op == 0x60 ? "RTS" : op == 0x40 ? "RTI" : "BRK") << " at $" << toHex(exits[i], addrWidth());
                    }
                    os << "\n";
                }
                m_output(os.str());
            } else {
                m_output("Syntax: analyze routine <addr> [max_instructions]\n");
            }
        }
    } else if (cmd == "sid") {
        if (!m_ctx.cpu || !m_ctx.bus) { m_output("No machine created.\n"); return; }
        std::string sub;
        if (ss >> sub && sub == "load") {
            std::string path;
            if (!(ss >> path)) { m_output("Syntax: sid load <path> [subtune]\n"); return; }
            int subtune = -1;
            std::string subStr;
            if (ss >> subStr) { try { subtune = std::stoi(subStr); } catch (...) {} }

            std::ifstream f(path, std::ios::binary);
            if (!f) { m_output("Error: Cannot open file: " + path + "\n"); return; }
            f.seekg(0, std::ios::end);
            size_t fileSize = f.tellg();
            f.seekg(0);
            if (fileSize < 0x7C) { m_output("Error: File too small for PSID header.\n"); return; }

            std::vector<uint8_t> raw(fileSize);
            f.read(reinterpret_cast<char*>(raw.data()), fileSize);

            std::string magic(reinterpret_cast<char*>(raw.data()), 4);
            if (magic != "PSID" && magic != "RSID") {
                m_output("Error: Not a PSID/RSID file.\n"); return;
            }

            auto rd16be = [&](size_t off) -> uint16_t { return (raw[off] << 8) | raw[off+1]; };
            uint16_t version    = rd16be(0x04);
            uint16_t dataOffset = rd16be(0x06);
            uint16_t loadAddr   = rd16be(0x08);
            uint16_t initAddr   = rd16be(0x0A);
            uint16_t playAddr   = rd16be(0x0C);
            uint16_t songs      = rd16be(0x0E);
            uint16_t startSong  = rd16be(0x10);

            auto extractStr = [&](size_t off) -> std::string {
                char buf[33]; std::memcpy(buf, &raw[off], 32); buf[32] = '\0'; return buf;
            };
            std::string title = extractStr(0x16);
            std::string author = extractStr(0x36);

            const uint8_t* data = raw.data() + dataOffset;
            size_t dataLen = fileSize - dataOffset;
            if (loadAddr == 0 && dataLen >= 2) {
                loadAddr = data[0] | (data[1] << 8);
                data += 2; dataLen -= 2;
            }
            if (initAddr == 0) initAddr = loadAddr;
            if (subtune < 1) subtune = startSong;
            if (subtune < 1) subtune = 1;
            if (subtune > songs) subtune = songs;

            for (size_t i = 0; i < dataLen; ++i)
                m_ctx.bus->write8(loadAddr + i, data[i]);

            // Trampoline at $0002: LDA #subtune, JSR init, BRK
            m_ctx.bus->write8(0x0002, 0xA9);
            m_ctx.bus->write8(0x0003, (uint8_t)(subtune - 1));
            m_ctx.bus->write8(0x0004, 0x20);
            m_ctx.bus->write8(0x0005, initAddr & 0xFF);
            m_ctx.bus->write8(0x0006, (initAddr >> 8) & 0xFF);
            m_ctx.bus->write8(0x0007, 0x00);

            m_ctx.cpu->setPc(0x0002);
            for (int s = 0; s < 1000000; ++s) {
                if (m_ctx.cpu->pc() == 0x0007) break;
                m_ctx.cpu->step();
            }

            // Install play loop if play address exists
            if (playAddr != 0) {
                m_ctx.bus->write8(0x0002, 0x20);
                m_ctx.bus->write8(0x0003, playAddr & 0xFF);
                m_ctx.bus->write8(0x0004, (playAddr >> 8) & 0xFF);
                m_ctx.bus->write8(0x0005, 0x4C);
                m_ctx.bus->write8(0x0006, 0x02);
                m_ctx.bus->write8(0x0007, 0x00);
                m_ctx.cpu->setPc(0x0002);
            }

            m_output(magic + " v" + std::to_string(version) + ": " + title + " by " + author + "\n");
            m_output("Songs: " + std::to_string(songs) + ", subtune " + std::to_string(subtune) + "\n");
            m_output("Load: $" + toHex(loadAddr) + " Init: $" + toHex(initAddr) + " Play: $" + toHex(playAddr) + "\n");
            if (playAddr) m_output("Play loop installed at $0002. Use 'recordaudio' to capture.\n");
        } else {
            m_output("Syntax: sid load <path> [subtune]\n");
        }
    } else if (cmd == "map") {
        if (!m_ctx.machine) { m_output("No machine created.\n"); return; }
        // Find MapMmu bus in the machine descriptor
        IMapController* mapCtrl = nullptr;
        for (auto& b : m_ctx.machine->buses) {
            mapCtrl = dynamic_cast<IMapController*>(b.bus);
            if (mapCtrl) break;
        }
        if (!mapCtrl) { m_output("No MAP controller on this machine.\n"); return; }
        const MapState& ms = mapCtrl->getMapState();
        std::ostringstream os;
        os << "MAP State:\n  Enables: $" << toHex(ms.enables, 2) << "\n  Offsets: ";
        for (int i = 0; i < 8; ++i) {
            if (i) os << ", ";
            os << "$" << toHex(ms.offsets[i], 5);
        }
        os << "\n  MB Low: $" << toHex(ms.megabyteLow >> 20, 2)
           << "  MB High: $" << toHex(ms.megabyteHigh >> 20, 2) << "\n";
        m_output(os.str());
    } else if (cmd == "personality") {
        if (!m_ctx.machine) { m_output("No machine created.\n"); return; }
        if (!m_ctx.machine->ioRegistry) { m_output("No I/O registry.\n"); return; }
        std::string mode;
        if (ss >> mode) {
            // Write KEY register sequence for personality change
            uint8_t seq[2] = {0, 0};
            if (mode == "c64")       { seq[0] = 0x00; seq[1] = 0x00; }
            else if (mode == "c65")  { seq[0] = 0xA5; seq[1] = 0x96; }
            else if (mode == "mega65" || mode == "gs") { seq[0] = 0x47; seq[1] = 0x53; }
            else { m_output("Unknown personality. Use: c64, c65, mega65\n"); return; }
            m_ctx.machine->ioRegistry->dispatchWrite(nullptr, 0xD02F, seq[0]);
            m_ctx.machine->ioRegistry->dispatchWrite(nullptr, 0xD02F, seq[1]);
            m_output("Personality switched to " + mode + ".\n");
        } else {
            // Read current personality from KEY register
            uint8_t val = 0;
            m_ctx.machine->ioRegistry->dispatchRead(nullptr, 0xD02F, &val);
            m_output("KEY register: $" + toHex(val, 2) + "\n");
        }
    } else if (cmd == "iomap") {
        if (!m_ctx.machine) { m_output("No machine created.\n"); return; }
        if (!m_ctx.machine->ioRegistry) { m_output("No I/O registry.\n"); return; }
        std::string expr;
        if (ss >> expr) {
            // Show which handler claims a specific address
            uint32_t addr = 0;
            if (!parseAddr(expr, addr)) {
                m_output("Error: Invalid address '" + expr + "'\n");
                return;
            }
            // Try dispatching to find the claiming handler
            std::vector<IOHandler*> handlers;
            m_ctx.machine->ioRegistry->enumerate(handlers);
            bool found = false;
            for (auto* h : handlers) {
                uint8_t dummy = 0;
                if (h->ioRead(nullptr, addr, &dummy)) {
                    uint32_t base = h->baseAddr();
                    uint32_t end = base + h->addrMask();
                    m_output("$" + toHex(addr, addrWidth()) + " → "
                           + h->name() + " ($" + toHex(base, addrWidth())
                           + "-$" + toHex(end, addrWidth()) + ")\n");
                    found = true;
                    break;
                }
            }
            if (!found) {
                m_output("$" + toHex(addr, addrWidth()) + " → (no I/O handler claims this address)\n");
            }
        } else {
            // List all registered I/O handlers with their address ranges
            std::vector<IOHandler*> handlers;
            m_ctx.machine->ioRegistry->enumerate(handlers);
            std::ostringstream os;
            os << "I/O Address Map (" << handlers.size() << " handlers):\n";
            for (auto* h : handlers) {
                uint32_t base = h->baseAddr();
                uint32_t end = base + h->addrMask();
                os << "  $" << toHex(base, addrWidth()) << "-$"
                   << toHex(end, addrWidth()) << "  " << h->name();
                if (h->isHaltRequested()) os << " [DMA active]";
                os << "\n";
            }
            m_output(os.str());
        }
    } else if (cmd == "heatmap") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        auto& hm = m_ctx.dbg->heatmap();
        std::string sub;
        if (ss >> sub) {
            if (sub == "on")    { hm.setEnabled(true);  m_output("Heat map recording enabled.\n"); }
            else if (sub == "off")   { hm.setEnabled(false); m_output("Heat map recording disabled.\n"); }
            else if (sub == "reset") { hm.reset();           m_output("Heat map cleared.\n"); }
            else if (sub == "top") {
                int n = 20; ss >> n;
                auto spots = hm.topAddresses(n);
                if (spots.empty()) { m_output("No data recorded. Use 'heatmap on' first.\n"); return; }
                std::ostringstream os;
                os << std::left << std::setw(10) << "Addr" << std::setw(10) << "Reads"
                   << std::setw(10) << "Writes" << "Total\n"
                   << std::string(40, '-') << "\n";
                for (const auto& s : spots) {
                    os << "$" << std::left << std::setw(9) << toHex(s.addr, addrWidth())
                       << std::setw(10) << s.reads << std::setw(10) << s.writes << s.total << "\n";
                }
                m_output(os.str());
            } else if (sub == "page") {
                // Show 256-page overview as ASCII heat bar
                std::ostringstream os;
                os << "Memory heat map (per page, " << (hm.isEnabled() ? "recording" : "PAUSED") << "):\n";
                uint32_t pages = hm.size() / 256;
                for (uint32_t p = 0; p < pages; p += 16) {
                    os << "$" << toHex(p * 256, addrWidth()) << ": ";
                    for (uint32_t i = 0; i < 16 && (p + i) < pages; ++i) {
                        double h = hm.pageHeat(p + i);
                        if (h == 0)      os << '.';
                        else if (h < 0.1) os << '-';
                        else if (h < 0.3) os << '+';
                        else if (h < 0.6) os << '#';
                        else              os << '@';
                    }
                    os << "\n";
                }
                os << "Legend: .=none -=low +=medium #=high @=very high\n";
                m_output(os.str());
            } else {
                m_output("Syntax: heatmap <on|off|reset|top [n]|page>\n");
            }
        } else {
            m_output("Heat map: " + std::string(hm.isEnabled() ? "enabled" : "disabled") + "\n");
            m_output("  heatmap on       - Start recording\n");
            m_output("  heatmap off      - Stop recording\n");
            m_output("  heatmap reset    - Clear all data\n");
            m_output("  heatmap top [n]  - Show top N hotspots\n");
            m_output("  heatmap page     - ASCII page-level overview\n");
        }
    } else if (cmd == "raster") {
        if (!m_ctx.machine || !m_ctx.machine->ioRegistry) {
            m_output("No machine created.\n"); return;
        }
        // Find VIC device via name match and query state from DeviceInfo
        std::vector<IOHandler*> handlers;
        m_ctx.machine->ioRegistry->enumerate(handlers);
        IOHandler* vic = nullptr;
        for (auto* h : handlers) {
            std::string n = h->name();
            if (n.find("VIC") != std::string::npos) { vic = h; break; }
        }
        if (!vic) { m_output("No VIC device found.\n"); return; }
        DeviceInfo info;
        vic->getDeviceInfo(info);
        // Extract raster state from DeviceInfo state entries
        std::ostringstream os;
        os << "Raster beam position:\n";
        for (const auto& [k, v] : info.state) {
            if (k == "Raster Line" || k == "Raster Cycle" ||
                k == "Lines/Frame" || k == "Cycles/Line")
                os << "  " << k << ": " << v << "\n";
        }
        m_output(os.str());
    } else if (cmd == "devinfo") {
        if (!m_ctx.machine || !m_ctx.machine->ioRegistry) {
            m_output("No machine created.\n"); return;
        }
        std::string devName;
        if (!(ss >> devName)) {
            m_output("Syntax: devinfo <device_name>\n");
            m_output("Use 'iomap' to list available devices.\n");
            return;
        }
        // Find handler by exact or partial name match
        IOHandler* handler = m_ctx.machine->ioRegistry->findHandler(devName);
        if (!handler) {
            std::vector<IOHandler*> handlers;
            m_ctx.machine->ioRegistry->enumerate(handlers);
            std::string target = devName;
            std::transform(target.begin(), target.end(), target.begin(), ::tolower);
            for (auto* h : handlers) {
                std::string hn = h->name();
                std::transform(hn.begin(), hn.end(), hn.begin(), ::tolower);
                if (hn == target || hn.find(target) != std::string::npos) {
                    handler = h; break;
                }
            }
        }
        if (!handler) {
            m_output("Device '" + devName + "' not found.\n"); return;
        }
        DeviceInfo info;
        handler->getDeviceInfo(info);
        std::ostringstream os;
        os << info.name << "  base=$" << toHex(info.baseAddr, addrWidth())
           << "  mask=$" << toHex(info.addrMask, addrWidth()) << "\n";
        if (!info.registers.empty()) {
            os << "  Registers:\n";
            for (const auto& r : info.registers) {
                os << "    $" << toHex(info.baseAddr + r.offset, addrWidth())
                   << " " << std::left << std::setw(16) << r.name
                   << " = $" << toHex(r.value, 2);
                if (!r.description.empty()) os << "  " << r.description;
                os << "\n";
            }
        }
        if (!info.state.empty()) {
            os << "  State:\n";
            for (const auto& [k, v] : info.state)
                os << "    " << k << " = " << v << "\n";
        }
        m_output(os.str());
    } else if (cmd == "profile") {
        if (!m_ctx.cpu) { m_output("No machine created.\n"); return; }
        int steps = 1000000, top = 20;
        ss >> steps; ss >> top;
        if (steps <= 0) steps = 1000000;
        if (top <= 0) top = 20;

        std::unordered_map<uint32_t, int> histogram;
        uint64_t startCycles = m_ctx.cpu->cycles();
        m_output("Profiling " + std::to_string(steps) + " steps...\n");
        g_interrupted = 0;
        for (int i = 0; i < steps && !g_interrupted; ++i) {
            histogram[m_ctx.cpu->pc()]++;
            if (m_ctx.machine && m_ctx.machine->schedulerStep)
                m_ctx.machine->schedulerStep(*m_ctx.machine);
            else
                m_ctx.cpu->step();
            if (m_ctx.dbg && m_ctx.dbg->isPaused()) break;
        }
        g_interrupted = 0;
        uint64_t totalCycles = m_ctx.cpu->cycles() - startCycles;

        std::vector<std::pair<uint32_t, int>> sorted(histogram.begin(), histogram.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        if ((int)sorted.size() > top) sorted.resize(top);

        int totalSamples = 0;
        for (const auto& [addr, cnt] : histogram) totalSamples += cnt;

        std::ostringstream os;
        os << "Profile: " << totalSamples << " instructions, "
           << totalCycles << " cycles, " << histogram.size() << " unique addresses\n\n"
           << std::left << std::setw(10) << "Addr" << std::setw(8) << "Count"
           << std::setw(8) << "Pct" << "Instruction\n"
           << std::string(50, '-') << "\n";
        for (const auto& [addr, cnt] : sorted) {
            double pct = totalSamples > 0 ? (100.0 * cnt / totalSamples) : 0;
            os << "$" << std::left << std::setw(9) << toHex(addr, addrWidth())
               << std::setw(8) << cnt;
            char pctBuf[16]; std::snprintf(pctBuf, sizeof(pctBuf), "%5.1f%%", pct);
            os << std::setw(8) << pctBuf;
            // Symbol
            if (m_ctx.dbg) {
                std::string sym = m_ctx.dbg->symbols().getLabel(addr);
                if (!sym.empty()) os << sym << ": ";
            }
            // Disassembly
            if (m_ctx.disasm) {
                char buf[64];
                m_ctx.disasm->disasmOne(m_ctx.bus, addr, buf, sizeof(buf));
                os << buf;
            }
            os << "\n";
        }
        m_output(os.str());
    } else if (cmd == "measure") {
        if (!m_ctx.cpu) { m_output("No machine created.\n"); return; }
        std::string startExpr, endExpr;
        if (!(ss >> startExpr >> endExpr)) {
            m_output("Syntax: measure <start_addr> <end_addr>\n");
            m_output("  Runs from start until PC leaves [start, end). Reports cycles.\n");
            return;
        }
        uint32_t startAddr, endAddr;
        if (!parseAddr(startExpr, startAddr) || !parseAddr(endExpr, endAddr)) {
            m_output("Error: Invalid address expression\n"); return;
        }
        m_ctx.cpu->setPc(startAddr);
        uint64_t startCycles = m_ctx.cpu->cycles();
        int instrCount = 0, maxSteps = 10000000;
        for (int i = 0; i < maxSteps; ++i) {
            uint32_t pc = m_ctx.cpu->pc();
            if (pc < startAddr || pc >= endAddr) break;
            if (m_ctx.machine && m_ctx.machine->schedulerStep)
                m_ctx.machine->schedulerStep(*m_ctx.machine);
            else
                m_ctx.cpu->step();
            instrCount++;
            if (m_ctx.dbg && m_ctx.dbg->isPaused()) break;
        }
        uint64_t totalCycles = m_ctx.cpu->cycles() - startCycles;
        double avgCpi = instrCount > 0 ? (double)totalCycles / instrCount : 0;
        std::ostringstream os;
        os << "Region $" << toHex(startAddr, addrWidth()) << " - $" << toHex(endAddr, addrWidth()) << ":\n"
           << "  Instructions: " << instrCount << "\n"
           << "  Total cycles: " << totalCycles << "\n"
           << "  Avg cycles/instr: " << std::fixed << std::setprecision(1) << avgCpi << "\n"
           << "  Final PC: $" << toHex(m_ctx.cpu->pc(), addrWidth()) << "\n";
        m_output(os.str());
    } else if (cmd == "recordaudio") {
        if (!m_ctx.machine) { m_output("No machine created.\n"); return; }
        std::string filename;
        int durationMs = 0;
        if (!(ss >> filename >> durationMs) || durationMs <= 0) {
            m_output("Syntax: recordaudio <filename.wav> <duration_ms>\n"); return;
        }
        if (durationMs > 60000) durationMs = 60000;

        // Find audio device
        IAudioOutput* audioDev = nullptr;
        bool isStereo = false;
        if (m_ctx.machine->ioRegistry) {
            std::vector<IOHandler*> handlers;
            m_ctx.machine->ioRegistry->enumerate(handlers);
            for (auto* h : handlers) {
                auto* ao = dynamic_cast<IAudioOutput*>(h);
                if (ao) {
                    audioDev = ao;
                    std::string dn = h->name();
                    if (dn.find("Pair") != std::string::npos || dn.find("Stereo") != std::string::npos)
                        isStereo = true;
                    break;
                }
            }
        }
        if (!audioDev) { m_output("Error: No audio output device found.\n"); return; }

        int sampleRate = audioDev->nativeSampleRate();
        if (sampleRate <= 0) sampleRate = 44100;
        int numChannels = isStereo ? 2 : 1;
        int totalSamples = (int)((int64_t)sampleRate * durationMs / 1000) * numChannels;

        std::vector<float> recorded;
        recorded.reserve(totalSamples);

        uint32_t clockHz = 985248;
        int64_t totalCycles = (int64_t)clockHz * durationMs / 1000;
        float pullBuf[4096];
        int64_t cyclesRun = 0;
        int batchSize = sampleRate / 20;
        if (batchSize < 256) batchSize = 256;
        int cyclesPerBatch = (int)((int64_t)clockHz * batchSize / sampleRate / numChannels);
        if (cyclesPerBatch < 100) cyclesPerBatch = 100;

        while (cyclesRun < totalCycles && (int)recorded.size() < totalSamples) {
            for (int c = 0; c < cyclesPerBatch; ++c) {
                if (m_ctx.machine->schedulerStep) {
                    m_ctx.machine->schedulerStep(*m_ctx.machine);
                } else {
                    m_ctx.cpu->step();
                }
                ++cyclesRun;
            }
            int pulled = audioDev->pullSamples(pullBuf, 4096);
            for (int i = 0; i < pulled && (int)recorded.size() < totalSamples; ++i)
                recorded.push_back(pullBuf[i]);
        }
        // Final drain
        while ((int)recorded.size() < totalSamples) {
            int pulled = audioDev->pullSamples(pullBuf, 4096);
            if (pulled <= 0) break;
            for (int i = 0; i < pulled && (int)recorded.size() < totalSamples; ++i)
                recorded.push_back(pullBuf[i]);
        }

        // Convert to 16-bit PCM and write WAV
        std::vector<int16_t> pcm(recorded.size());
        for (size_t i = 0; i < recorded.size(); ++i) {
            float s = std::max(-1.0f, std::min(1.0f, recorded[i]));
            pcm[i] = (int16_t)(s * 32767.0f);
        }

        std::ofstream wav(filename, std::ios::binary);
        if (!wav) { m_output("Error: Cannot open file: " + filename + "\n"); return; }

        uint32_t dataSize = pcm.size() * 2;
        uint32_t wavFileSize = 36 + dataSize;
        uint16_t bitsPerSample = 16;
        uint16_t blockAlign = numChannels * 2;
        uint32_t byteRate = sampleRate * blockAlign;
        uint16_t audioFmt = 1;
        uint16_t nc = numChannels;
        uint32_t sr = sampleRate;
        uint32_t fmtSize = 16;

        wav.write("RIFF", 4); wav.write(reinterpret_cast<char*>(&wavFileSize), 4);
        wav.write("WAVE", 4);
        wav.write("fmt ", 4); wav.write(reinterpret_cast<char*>(&fmtSize), 4);
        wav.write(reinterpret_cast<char*>(&audioFmt), 2);
        wav.write(reinterpret_cast<char*>(&nc), 2);
        wav.write(reinterpret_cast<char*>(&sr), 4);
        wav.write(reinterpret_cast<char*>(&byteRate), 4);
        wav.write(reinterpret_cast<char*>(&blockAlign), 2);
        wav.write(reinterpret_cast<char*>(&bitsPerSample), 2);
        wav.write("data", 4); wav.write(reinterpret_cast<char*>(&dataSize), 4);
        wav.write(reinterpret_cast<const char*>(pcm.data()), dataSize);

        float durActual = (float)recorded.size() / numChannels / sampleRate;
        std::ostringstream os;
        os << "Recorded " << std::fixed << std::setprecision(2) << durActual
           << "s to " << filename << " (" << (isStereo ? "stereo" : "mono")
           << ", " << sampleRate << " Hz, " << dataSize << " bytes)\n";
        m_output(os.str());
    } else if (cmd == "load-debug-metadata") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        std::string path;
        if (ss >> path) {
            if (m_ctx.dbg->variables().loadFromDebugMetadata(path)) {
                m_output("Debug metadata loaded from: " + path + "\n");
            } else {
                m_output("Error: Could not load debug metadata from: " + path + "\n");
            }
        } else {
            m_output("Usage: load-debug-metadata <file>\n");
        }
    } else if (cmd == "vars") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        std::string funcName;
        if (ss >> funcName) {
            auto vars = m_ctx.dbg->variables().getVariablesInFunction(funcName);
            if (vars.empty()) {
                m_output("No variables found for function: " + funcName + "\n");
            } else {
                std::stringstream out;
                out << "Variables in " << funcName << ":\n";
                out << std::setfill(' ') << std::left;
                for (const auto* var : vars) {
                    out << "  " << std::setw(20) << var->displayName
                        << "  @" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << var->address
                        << "  size=" << std::dec << var->size
                        << "  type=" << formatVariableType(var->type);
                    if (var->sourceLine >= 0) {
                        out << "  line=" << var->sourceLine;
                    }
                    out << "\n";
                }
                m_output(out.str());
            }
        } else {
            auto globals = m_ctx.dbg->variables().getGlobalVariables();
            if (globals.empty()) {
                m_output("No variables defined.\n");
            } else {
                std::stringstream out;
                out << "Global Variables:\n";
                out << std::setfill(' ') << std::left;
                for (const auto* var : globals) {
                    out << "  " << std::setw(20) << var->displayName
                        << "  @" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << var->address
                        << "  size=" << std::dec << var->size
                        << "  type=" << formatVariableType(var->type) << "\n";
                }
                m_output(out.str());
            }
        }
    } else if (cmd == "log") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        std::string sub;
        if (ss >> sub) {
            if (sub == "show") {
                auto& buf = m_ctx.dbg->trace();
                if (buf.size() == 0) { m_output("Trace buffer empty.\n"); return; }

                int count = 20;
                std::string opt;
                if (ss >> opt) {
                    if (opt == "-memory") {
                        std::string memAddr;
                        if (ss >> memAddr) {
                            uint32_t addr = 0;
                            if (!parseAddr(memAddr, addr)) {
                                m_output("Error: Invalid address '" + memAddr + "'\n");
                                return;
                            }
                            // Show memory access pattern to this address
                            std::stringstream out;
                            out << "Memory access pattern for $" << std::hex << std::uppercase
                                << std::setfill('0') << std::setw(addrWidth()) << addr << ":\n";
                            int cycle = 0;
                            for (size_t i = 0; i < buf.size(); ++i) {
                                const auto& e = buf.at(i);
                                for (const auto& mw : e.memWrites) {
                                    if (mw.addr == addr) {
                                        out << "  Cycle " << cycle << ": Updated from $"
                                            << std::hex << std::uppercase << std::setfill('0')
                                            << std::setw(2) << (int)mw.before << " (at $"
                                            << std::setw(addrWidth()) << e.addr << " " << e.mnemonic << ")\n";
                                    }
                                }
                                cycle++;
                            }
                            m_output(out.str());
                        } else {
                            m_output("Usage: log show -memory <address>\n");
                        }
                    } else if (opt == "-calls") {
                        // Show only JSR/RTS instructions
                        std::stringstream out;
                        out << "Function call stack (last 20 calls):\n";
                        int callCount = 0;
                        for (size_t i = buf.size(); i > 0 && callCount < 20; --i) {
                            const auto& e = buf.at(i - 1);
                            if (e.mnemonic.find("JSR") != std::string::npos) {
                                out << "  CALL from $" << std::hex << std::uppercase << std::setfill('0')
                                    << std::setw(addrWidth()) << e.addr << " to ???\n";
                                callCount++;
                            } else if (e.mnemonic.find("RTS") != std::string::npos) {
                                out << "  RETURN from $" << std::hex << std::uppercase << std::setfill('0')
                                    << std::setw(addrWidth()) << e.addr << "\n";
                                callCount++;
                            }
                        }
                        m_output(out.str());
                    } else if (opt == "-last") {
                        // log show -last N
                        try { count = std::stoi(opt.substr(5)); } catch (...) {}
                        // Show last N instructions
                        std::stringstream out;
                        out << "Last " << count << " instructions executed:\n";
                        int shown = 0;
                        for (size_t i = buf.size(); i > 0 && shown < count; --i) {
                            const auto& e = buf.at(i - 1);
                            out << "  $" << std::hex << std::uppercase << std::setfill('0')
                                << std::setw(addrWidth()) << e.addr << ": " << e.mnemonic << "\n";
                            shown++;
                        }
                        m_output(out.str());
                    }
                } else {
                    // Default: show last 20 instructions
                    std::stringstream out;
                    out << "Last 20 instructions executed:\n";
                    int shown = 0;
                    for (size_t i = buf.size(); i > 0 && shown < 20; --i) {
                        const auto& e = buf.at(i - 1);
                        out << "  $" << std::hex << std::uppercase << std::setfill('0')
                            << std::setw(addrWidth()) << e.addr << ": " << e.mnemonic << "\n";
                        shown++;
                    }
                    m_output(out.str());
                }
            } else if (sub == "clear") {
                m_ctx.dbg->trace().clear();
                m_output("Execution log cleared.\n");
            } else {
                m_output("Usage: log show [options] | log clear\n");
                m_output("  log show              - Show last 20 instructions\n");
                m_output("  log show -last N      - Show last N instructions\n");
                m_output("  log show -memory ADDR - Show memory access pattern\n");
                m_output("  log show -calls       - Show function call stack\n");
            }
        } else {
            m_output("Usage: log show [options] | log clear\n");
        }
    } else if (cmd == "quit" || cmd == "q") {
        m_ctx.quit = true;
    } else {
        std::vector<std::string> tokens;
        std::stringstream ss2(line);
        std::string t;
        while (ss2 >> t) tokens.push_back(t);
        
        if (!PluginCommandRegistry::instance().dispatch(tokens)) {
            m_output("Unknown command: " + cmd + ". Type 'help' for info.\n");
        }
    }
}

void CliInterpreter::handleAssemblyLine(const std::string& line) {
    if (line == ".") {
        m_asmMode = false;
        m_output("Assembly ended.\n");
        return;
    }

    uint8_t opcodes[16];
    int sz = m_ctx.assem->assembleLine(line, opcodes, sizeof(opcodes), m_asmAddr);
    if (sz > 0) {
        std::stringstream res;
        res << "> " << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << m_asmAddr << " ";
        for (int i = 0; i < sz; ++i) {
            m_ctx.bus->write8(m_asmAddr + i, opcodes[i]);
            res << "$" << std::setw(2) << (int)opcodes[i] << " ";
        }
        res << "\n";
        m_output(res.str());
        m_asmAddr += sz;
    } else {
        m_output("Assembly failed: " + line + "\n");
    }
}

void CliInterpreter::printHelp(const std::string& category) {
    if (category == "debugging") {
        printDebuggingGuide();
    } else if (category.empty()) {
        printHelpOverview();
    } else {
        printHelpCategory(category);
    }
}

void CliInterpreter::printHelpOverview() {
    m_output("mmemu Debugger - Available Command Categories:\n"
             "\nUse 'help <category>' for detailed information:\n"
             "  help loading       - Program and cartridge loading\n"
             "  help execution     - Running, stepping, and flow control\n"
             "  help inspection    - Memory and register inspection\n"
             "  help debugging     - Breakpoints, watchpoints, and debugging workflow\n"
             "  help general       - General commands and utilities\n"
             "\nQuick Start:\n"
             "  1. Create a machine:  create c64\n"
             "  2. Load a program:    load game.prg\n"
             "  3. Set breakpoint:    break 2048\n"
             "  4. Run:               run\n"
             "  5. Step:              step\n"
             "  6. Inspect:           regs  (show registers)\n"
             "                        m 2048 16  (dump memory)\n"
             "\nFor interactive debugging tutorial, type: help debugging\n");

    std::vector<std::string> pluginCmds;
    PluginCommandRegistry::instance().listCommands(pluginCmds);
    if (!pluginCmds.empty()) {
        m_output("\nPlugin commands:\n");
        for (const auto& s : pluginCmds) m_output(s + "\n");
    }
}

void CliInterpreter::printHelpCategory(const std::string& category) {
    if (category == "loading") {
        m_output("LOADING - Program and Cartridge Loading Commands:\n"
                 "  load <path> [addr]  - Load a program/binary file at address\n"
                 "  load-vice <path>    - Load a VICE snapshot (.vsf) file\n"
                 "  save-vice <path>    - Save current state to VICE snapshot file\n"
                 "  cart <path>         - Attach a cartridge image\n"
                 "  eject               - Eject currently attached cartridge\n"
                 "  disk mount <unit> <path> - Mount a disk image to drive unit\n"
                 "  disk eject <unit>   - Eject a disk image\n"
                 "  tape mount <path>   - Mount a .tap file for playback\n"
                 "  tape play           - Start/resume tape playback\n"
                 "  tape stop           - Release all tape buttons\n"
                 "  tape rewind         - Rewind tape to start\n"
                 "  tape record         - Press Record button\n"
                 "  tape stoprecord     - Release Record button\n"
                 "  tape save <path>    - Write captured buffer to .tap file\n"
                 "  sid load <path>     - Load a SID music file (C64)\n"
                 "  sym load <path>     - Load symbol table from file\n"
                 "\nExample workflow:\n"
                 "  create c64\n"
                 "  load game.prg 2048\n"
                 "  run\n"
                 "\nVICE Snapshot Examples:\n"
                 "  create c64\n"
                 "  load-vice saved_state.vsf\n"
                 "  break 2000\n"
                 "  run\n"
                 "  save-vice current_state.vsf\n");
    } else if (category == "execution") {
        m_output("EXECUTION - Running, Stepping, and Flow Control:\n"
                 "  run [arg]           - Run (interruptible with periodic status)\n"
                 "                        arg: [addr] run from address or decimal step count\n"
                 "                        e.g., 'run 5000000' runs 5M steps, 'run 2048' runs from 2048\n"
                 "  run breakpoint      - Run until next breakpoint (ignores program end)\n"
                 "  step [n]            - Step CPU N times, or to next source line if no arg\n"
                 "                        step 5 executes 5 instructions (CPU-level)\n"
                 "                        step (no arg) steps to next source line, following calls\n"
                 "  next                - Step to next source line (skips over function calls)\n"
                 "  until <line>        - Run until reaching source line (requires .loc directives)\n"
                 "  finish              - Run until current function returns\n"
                 "  runto <cond>        - Run until condition expression is true\n"
                 "  setpc <addr>        - Set CPU program counter\n"
                 "  .<instr>            - Assemble and execute a single instruction\n"
                 "  key <name> <state>  - Press/release a key (state: 1/0 or down/up)\n"
                 "  type <text>         - Type text into the machine (supports \\n)\n"
                 "\nExample debugging session:\n"
                 "  break 2048          (set breakpoint at program start)\n"
                 "  run                 (run until breakpoint)\n"
                 "  step                (step one instruction)\n"
                 "  next                (step to next source line)\n"
                 "  regs                (see registers at breakpoint)\n"
                 "\nNotes:\n"
                 "  - Ctrl-C interrupts 'run' at any time\n"
                 "  - Status updates every 100k steps during long runs\n"
                 "  - 'run' without args continues from last breakpoint or loaded program\n");
    } else if (category == "inspection") {
        m_output("INSPECTION - Memory and Register Inspection:\n"
                 "  regs                - Show all CPU registers\n"
                 "  regwrite <n> <v>    - Write value V to register N\n"
                 "  m <addr> [len]      - Dump memory (default 16 bytes)\n"
                 "  disasm <addr> [n]   - Disassemble N instructions\n"
                 "  search <hex...>     - Search for hex pattern in memory\n"
                 "  searcha <str>       - Search for ASCII string in memory\n"
                 "  findnext            - Find next match of last search\n"
                 "  findprior           - Find previous match of last search\n"
                 "  copy <src> <dst> <len> - Copy memory range\n"
                 "  swap <addr1> <addr2> <len> - Swap two memory ranges\n"
                 "  f <addr> <val> [len] - Fill memory range with value\n"
                 "  save <path> <addr> <len> - Save memory to binary file\n"
                 "  screenshot <file>   - Save current screen to PNG\n"
                 "  recordaudio <f> <d> - Record D ms of audio to WAV file\n"
                 "  stack [n]           - Show stack trace (default 8 entries)\n"
                 "  devinfo <name>      - Show device registers and state\n"
                 "  iomap [addr]        - Show I/O handler for address\n"
                 "  heatmap <cmd>       - Memory access heat map\n"
                 "  raster              - Show current raster beam position\n"
                 "  profile [n] [top]   - Profile N steps, show top hotspots\n"
                 "  measure <s> <e>     - Measure cycles for address range\n"
                 "\nExample usage:\n"
                 "  m 2048 32           (dump 32 bytes from 2048)\n"
                 "  search 4C 20 A9     (find sequence of bytes)\n"
                 "  disasm $8000        (disassemble from ROM area)\n");
    } else if (category == "debugging") {
        printDebuggingGuide();
    } else if (category == "general") {
        m_output("GENERAL - General Commands and Utilities:\n"
                 "  help [category]     - Show help (or specific category help)\n"
                 "  list                - List available machine types\n"
                 "  create <id>         - Create a machine (c64, vic20, pet, mega65)\n"
                 "  asm <addr>          - Interactive assembly mode (end with '.')\n"
                 "  asm file <path>     - Assemble source file and load\n"
                 "  sym <op> [args]     - Symbol management (add/del/list/search/load/clear)\n"
                 "  script run <path>   - Run a Lua script file\n"
                 "  script eval <code>  - Execute Lua code inline\n"
                 "  log <list|level>    - Logging configuration\n"
                 "  map [offsets] [mask] - Read/Write MEGA65 MAP state\n"
                 "  personality <m>     - Switch MEGA65 I/O personality\n"
                 "  analyze routine <addr> - Analyze routine flow and targets\n"
                 "  quit, q             - Exit the program\n"
                 "\nAll address values can be expressions with symbols and math.\n"
                 "Example: break start_routine + 10\n");
    } else {
        m_output("Unknown help category: '" + category + "'\n");
        m_output("Available categories: loading, execution, inspection, debugging, general\n");
    }
}

void CliInterpreter::printDebuggingGuide() {
    m_output("DEBUGGING - Complete Breakpoint and Watchpoint Guide\n"
             "\n=== Basic Breakpoints ===\n"
             "  break <addr>        - Set execution breakpoint at address\n"
             "  break start + 10    - Breakpoint at symbol+offset\n"
             "  break $2000 action \"mmemu.log('hit')\" - Lua action on breakpoint (Issue #24)\n"
             "  break $2000 count 5 - Stop after 5 hits\n"
             "  delete <id>         - Delete breakpoint/watchpoint by id\n"
             "  enable <id>         - Re-enable a disabled breakpoint\n"
             "  disable <id>        - Disable without deleting\n"
             "  info breaks         - List all breakpoints and watchpoints\n"
             "\n=== Memory Watchpoints ===\n"
             "  watch read <addr>   - Halt when address is READ\n"
             "  watch write <addr>  - Halt when address is WRITTEN\n"
             "  Example: watch write $2000  (halt on any write to 2000)\n"
             "\n=== Inspection After Halt ===\n"
             "  regs                - Show CPU registers at breakpoint\n"
             "  m <addr> [len]      - Dump memory around the halt\n"
             "  disasm <addr>       - Show instructions near halt\n"
             "  stack               - Show call stack trace\n"
             "  list [lines]        - Show source code (requires .loc directives)\n"
             "  info locals         - Show local variables with values\n"
             "  info frame          - Show frame layout with variable addresses\n"
             "  frame [verbose]     - Show frame structure (verbose for table + struct)\n"
             "  print <varname>     - Display typed value of a specific variable\n"
             "\n=== Debug Metadata and Symbol Inspection ===\n"
             "  load-debug-metadata <file> - Load debug symbols from assembly file\n"
             "  vars                       - Show all global variables\n"
             "  vars <function>            - Show local variables in a function\n"
             "  sym add <name> <addr>      - Manually add a symbol\n"
             "  sym list                   - List all defined symbols\n"
             "  sym search <query>         - Search for symbols by name\n"
             "  sym load <file>            - Load symbols from .sym file\n"
             "  sym load-c64ide            - Load C64IDE ROM symbol database\n"
             "  sym load-o45 <file>        - Load debug symbols from .o45 object file\n"
             "\n=== Source-Level Debugging (requires .loc directives) ===\n"
             "  list                - Show source code around current PC\n"
             "  list 10-20          - Show source lines 10-20\n"
             "  break at line N     - Set breakpoint at source line\n"
             "  step                - Step to next source line, following function calls\n"
             "  next                - Step to next source line, skipping function calls\n"
             "  until <line>        - Run until reaching specific source line\n"
             "  until file:line     - Run until line in specific file\n"
             "  finish              - Run until current function returns\n"
             "  Note: step (step-into) follows JSR calls, next (step-over) skips them\n"
             "  Note: Source locations display as clickable links in terminals\n"
             "\n=== Execution History & Reverse Debugging ===\n"
             "  log show              - Show last 20 executed instructions\n"
             "  log show -last N       - Show last N instructions\n"
             "  log show -memory ADDR  - Show memory access pattern to address\n"
             "  log show -calls        - Show function call/return stack\n"
             "  log clear             - Clear execution history\n"
             "  backstep              - Step backward one instruction\n"
             "  backstep N            - Step backward N instructions\n"
             "\n=== Tracing and Flow ===\n"
             "  trace dump [n]      - Show last N executed instructions\n"
             "  trace clear         - Clear trace buffer\n"
             "  trace filter <m>    - Set trace filter (all/instructions/memory)\n"
             "  undoinfo            - Show what would be undone by backstep\n"
             "\n=== Snapshots (Save/Restore State) ===\n"
             "  snapshot save <name>  - Save current machine state\n"
             "  snapshot list         - List saved snapshots\n"
             "  snapshot diff <a> <b> - Compare two snapshots\n"
             "  snapshot delete <n>   - Delete snapshot\n"
             "\n=== Tutorial Workflow ===\n"
             "\nStep 1: Set up machine\n"
             "  create c64\n"
             "  load program.prg 2048\n"
             "\nStep 2: Set breakpoint at start of code\n"
             "  break 2048\n"
             "\nStep 3: Run program (will halt at breakpoint)\n"
             "  run\n"
             "\nStep 4: Inspect state at breakpoint\n"
             "  regs                    (see CPU state)\n"
             "  m 2048 32               (see memory around PC)\n"
             "  disasm 2048 10          (see next instructions)\n"
             "\nStep 5: Step through one instruction\n"
             "  step                    (execute next instruction)\n"
             "  regs                    (see new state)\n"
             "\nStep 6: Continue running (if desired)\n"
             "  run                     (continue to next breakpoint)\n"
             "\nStep 7: Set memory watchpoint to catch writes\n"
             "  watch write $2000\n"
             "  run                     (halt when $2000 is written)\n"
             "\nStep 8: Examine trace history\n"
             "  trace dump              (show instruction history)\n"
             "  trace filter instructions\n"
             "\n=== Tips ===\n"
             "- Use 'info breaks' to see all active breakpoints\n"
             "- Breakpoints use expressions: break loop_start + 2\n"
             "- Watchpoints catch both read and write operations\n"
             "- Snapshots let you save/compare state at key points\n"
             "- Trace buffer shows last ~1000 instructions executed\n"
             "- Use 'help execution' for step and run details\n"
             "- Use 'help inspection' for memory/register tools\n");
}

void CliInterpreter::dumpMemory(uint32_t addr, uint32_t len) {
    std::stringstream res;
    for (uint32_t i = 0; i < len; i += 16) {
        res << std::hex << std::uppercase << std::setfill('0') << std::setw(addrWidth()) << (addr + i) << ": ";
        for (uint32_t j = 0; j < 16 && (i + j) < len; ++j) {
            res << std::setw(2) << (int)m_ctx.bus->peek8(addr + i + j) << " ";
        }
        res << "\n";
    }
    m_output(res.str());
}

void CliInterpreter::showRegisters() {
    std::stringstream res;
    int count = m_ctx.cpu->regCount();
    for (int i = 0; i < count; ++i) {
        const auto* desc = m_ctx.cpu->regDescriptor(i);
        if (desc->flags & REGFLAG_INTERNAL) continue;
        
        uint32_t val = m_ctx.cpu->regRead(i);
        res << desc->name << ": ";
        if (desc->width == RegWidth::R16) {
            res << "$" << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << val;
        } else {
            res << "$" << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << val;
        }
        res << "  ";
        if ((i + 1) % 4 == 0) res << "\n";
    }
    res << "\nCycles: " << std::dec << m_ctx.cpu->cycles() << "\n";
    m_output(res.str());
}

void CliInterpreter::saveMemory(const std::string& path, uint32_t addr, uint32_t len) {
    if (!m_ctx.bus) {
        m_output("No bus available.\n");
        return;
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        m_output("Failed to open file for writing: " + path + "\n");
        return;
    }
    for (uint32_t i = 0; i < len; ++i) {
        uint8_t val = m_ctx.bus->read8(addr + i);
        fputc(val, f);
    }
    fclose(f);
    m_output("Saved " + std::to_string(len) + " bytes to " + path + "\n");
}

void CliInterpreter::showLocals() {
    if (!m_ctx.dbg || !m_ctx.cpu || !m_ctx.bus) {
        m_output("No machine created.\n");
        return;
    }

    auto info = DebugHelpers::getLocalVariablesInfo(m_ctx.dbg, m_ctx.bus);
    if (!info.hasVariables) {
        m_output("No variables defined.\n");
        return;
    }

    m_output("Local Variables:\n");
    for (const auto& var : info.variables) {
        std::string valueStr = DebugHelpers::formatVariableValue(var);

        std::ostringstream line;
        line << "  " << std::left << std::setw(20) << var.displayName;
        line << " (" << std::left << std::setw(10) << var.type << ") @ ";
        line << "$" << std::hex << std::uppercase << std::setfill('0')
             << std::setw(addrWidth()) << var.address;
        line << " = " << valueStr;

        m_output(line.str() + "\n");
    }
}

void CliInterpreter::showFrameLayout() {
    if (!m_ctx.dbg || !m_ctx.bus || !m_ctx.cpu) {
        m_output("No machine created.\n");
        return;
    }

    // Estimate frame pointer and size
    uint32_t framePointer = 0x100;  // C64 stack page default
    uint32_t frameSize = 256;

    auto layout = FrameLayoutAnalyzer::analyzeCurrentFrame(m_ctx.dbg, m_ctx.bus, framePointer, frameSize);
    if (layout.empty()) {
        m_output("No frame information available.\n");
        return;
    }

    m_output(FrameLayoutAnalyzer::formatFrameLayout(layout, framePointer, frameSize));
    m_output("\n");
    m_output(FrameLayoutAnalyzer::formatAsStructDefinition(layout));
}

void CliInterpreter::printVariable(const std::string& varName) {
    if (!m_ctx.dbg || !m_ctx.bus) {
        m_output("No machine created.\n");
        return;
    }

    auto var = DebugHelpers::getVariableInfo(m_ctx.dbg, m_ctx.bus, varName);
    if (var.name.empty()) {
        m_output("Variable '" + varName + "' not found.\n");
        return;
    }

    std::string valueStr = DebugHelpers::formatVariableValue(var);

    std::ostringstream out;
    out << var.displayName << " (" << var.type << ")\n";
    out << "  Address: $" << std::hex << std::uppercase << std::setfill('0')
        << std::setw(addrWidth()) << var.address << "\n";
    out << "  Size: " << std::dec << var.size << " bytes\n";
    out << "  Value: " << valueStr << "\n";

    if (var.sourceLine >= 0) {
        out << "  Source line: " << var.sourceLine << "\n";
    }

    m_output(out.str());
}

void CliInterpreter::showSourceLines(const std::string& file, int startLine, int endLine) {
    // Check cache first
    auto cacheIt = m_sourceFileCache.find(file);
    std::vector<std::string> srcLines;

    if (cacheIt == m_sourceFileCache.end()) {
        // Load source file
        std::ifstream f(file);
        if (!f.is_open()) {
            m_output("Source file not found: " + file + "\n");
            return;
        }

        std::string line;
        while (std::getline(f, line)) {
            srcLines.push_back(line);
        }
        f.close();

        m_sourceFileCache[file] = srcLines;
    } else {
        srcLines = cacheIt->second;
    }

    if (srcLines.empty()) {
        m_output("No source lines available.\n");
        return;
    }

    // Validate line range (1-indexed)
    if (startLine < 1) startLine = 1;
    if (endLine < startLine) endLine = startLine;
    if (endLine > (int)srcLines.size()) endLine = srcLines.size();

    m_output(file + " (" + std::to_string(startLine) + "-" + std::to_string(endLine) + "):\n");

    for (int i = startLine - 1; i < endLine && i < (int)srcLines.size(); i++) {
        std::ostringstream line;
        line << std::right << std::setw(4) << (i + 1) << "  " << srcLines[i] << "\n";
        m_output(line.str());
    }
}

void CliInterpreter::showCurrentSource() {
    if (!m_ctx.cpu || !m_ctx.dbg) {
        m_output("No machine created.\n");
        return;
    }

    uint32_t pc = m_ctx.cpu->pc();

    // Create CLI formatter for terminal hyperlinks
    auto formatter = SourceLocationFormatterFactory::create(
        SourceLocationFormatterFactory::Context::CLI);

    // Get source location from source map
    auto srcLoc = m_ctx.dbg->sourceMap().addrToSource(pc);

    // Create formatted location with address context
    FormattedSourceLocation loc;
    if (srcLoc.line >= 0 && !srcLoc.file.empty()) {
        loc.file = srcLoc.file;
        loc.line = srcLoc.line;
        loc.address = pc;

        m_output("Current PC: $" + toHex(pc, addrWidth()) + "\n");
        m_output("Source location: " + formatter->formatWithAddress(loc) + "\n");

        // Show source code context if available
        showSourceLines(srcLoc.file, std::max(1, srcLoc.line - 2), srcLoc.line + 2);
    } else {
        loc.file = "[no source]";
        loc.line = -1;
        loc.address = pc;

        m_output("Current PC: $" + toHex(pc, addrWidth()) + "\n");
        m_output("Source location: " + formatter->format(loc) + "\n");
        m_output("(load .loc directives from assembly or .debug_info file to enable source display)\n");
    }
}

void CliInterpreter::handleListCommand(const std::string& args) {
    // Parse list command arguments:
    // - "10" or "10-20" (source line range from current source file)
    // - "filename:10" (show around line 10 in specific file)
    // - "filename:10-20" (show lines 10-20 in specific file)

    if (!m_ctx.cpu || !m_ctx.dbg) {
        m_output("No machine created.\n");
        return;
    }

    // Check if args contain filename:
    size_t colonPos = args.find(':');
    std::string filename;
    std::string lineSpec;

    if (colonPos != std::string::npos) {
        // filename:line or filename:line-line format
        filename = args.substr(0, colonPos);
        lineSpec = args.substr(colonPos + 1);
    } else {
        // Get current source file from PC
        uint32_t pc = m_ctx.cpu->pc();
        auto srcLoc = m_ctx.dbg->sourceMap().addrToSource(pc);

        if (srcLoc.file.empty()) {
            m_output("Cannot determine current source file. Use 'list filename:line' instead.\n");
            return;
        }
        filename = srcLoc.file;
        lineSpec = args;
    }

    // Parse line specification: "10" or "10-20"
    int startLine, endLine;
    size_t dashPos = lineSpec.find('-');

    if (dashPos != std::string::npos) {
        // Range: "10-20"
        try {
            startLine = std::stoi(lineSpec.substr(0, dashPos));
            endLine = std::stoi(lineSpec.substr(dashPos + 1));
        } catch (const std::exception& e) {
            m_output("Invalid line range format. Use: 'list 10-20' or 'list filename:10-20'\n");
            return;
        }
    } else {
        // Single line: "10" - show context around it
        try {
            int line = std::stoi(lineSpec);
            startLine = std::max(1, line - 3);
            endLine = line + 3;
        } catch (const std::exception& e) {
            m_output("Invalid line number. Use: 'list 10' or 'list filename:10'\n");
            return;
        }
    }

    showSourceLines(filename, startLine, endLine);
}
