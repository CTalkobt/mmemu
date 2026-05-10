#include "cli_interpreter.h"
#include "include/util/logging.h"
#include "libcore/main/machines/machine_registry.h"
#include "libtoolchain/main/toolchain_registry.h"
#include "libcore/main/image_loader.h"
#include "libcore/main/sim_config.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "plugin_command_registry.h"
#include "libdebug/main/expression_evaluator.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>

static std::string toHex(uint32_t v, int width = 4) {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0') << std::setw(width) << v;
    return ss.str();
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
        if (!m_ctx.cpu || !m_ctx.assem) {
            m_output("No machine created or no assembler for this ISA.\n");
            return;
        }
        std::string instr = line.substr(1);
        uint8_t opcodes[16];
        if (int sz = m_ctx.assem->assembleLine(instr, opcodes, sizeof(opcodes), m_ctx.cpu->pc()); sz > 0) {
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
        printHelp();
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
        int n = 1;
        if (ss >> n) {} else { n = 1; }
        for (int i = 0; i < n; ++i) {
            if (m_ctx.machine && m_ctx.machine->schedulerStep) {
                m_ctx.machine->schedulerStep(*m_ctx.machine);
            } else {
                m_ctx.cpu->step();
            }
        }
        showRegisters();
    } else if (cmd == "run") {
        if (!m_ctx.cpu) { m_output("No machine created.\n"); return; }
        std::string expr;
        if (ss >> expr) {
            uint32_t addr;
            if (parseAddr(expr, addr)) {
                m_ctx.cpu->setPc(addr);
            } else {
                m_output("Error: Invalid address '" + expr + "'\n");
                return;
            }
        } else if (m_ctx.lastLoadAddr != 0) {
            m_ctx.cpu->setPc(m_ctx.lastLoadAddr);
        }
        m_output("Running... (Ctrl-C to stop - actually not supported in CLI yet, will run until break)\n");
        m_ctx.dbg->resume();
        while (!m_ctx.dbg->isPaused()) {
            if (m_ctx.machine && m_ctx.machine->schedulerStep) {
                m_ctx.machine->schedulerStep(*m_ctx.machine);
            } else {
                m_ctx.cpu->step();
            }
            if (m_ctx.cpu->isProgramEnd(m_ctx.bus)) break;
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
            if (auto* loader = ImageLoaderRegistry::instance().findLoader(path)) {
                if (loader->load(path, m_ctx.bus, m_ctx.machine, addr)) {
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
    } else if (cmd == "info") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        std::string sub;
        ss >> sub;
        if (sub == "breaks") {
            const auto& breaks = m_ctx.dbg->breakpoints().breakpoints();
            if (breaks.empty()) {
                m_output("No breakpoints set.\n");
            } else {
                m_output("Num     Type        Disp Enb Address\n");
                for (const auto& bp : breaks) {
                    std::stringstream row;
                    row << std::left << std::setw(8) << bp.id;
                    std::string type;
                    switch (bp.type) {
                        case BreakpointType::EXEC: type = "exec"; break;
                        case BreakpointType::READ_WATCH: type = "read"; break;
                        case BreakpointType::WRITE_WATCH: type = "write"; break;
                    }
                    row << std::left << std::setw(12) << type;
                    row << "keep  " << (bp.enabled ? "y" : "n") << "  ";
                    row << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << bp.addr;
                    m_output(row.str() + "\n");
                }
            }
        }
    } else if (cmd == "break") {
        if (!m_ctx.dbg) { m_output("No machine created.\n"); return; }
        std::string expr;
        if (ss >> expr) {
            uint32_t addr;
            if (parseAddr(expr, addr)) {
                int id = m_ctx.dbg->breakpoints().add(addr, BreakpointType::EXEC);
                m_output("Breakpoint " + std::to_string(id) + " at $" + toHex(addr) + "\n");
            } else {
                m_output("Error: Invalid address '" + expr + "'\n");
            }
        } else {
            m_output("Syntax: break <address>\n");
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
        std::string typeStr, expr;
        if (ss >> typeStr >> expr) {
            uint32_t addr;
            if (parseAddr(expr, addr)) {
                BreakpointType type;
                if (typeStr == "read") {
                    type = BreakpointType::READ_WATCH;
                } else if (typeStr == "write") {
                    type = BreakpointType::WRITE_WATCH;
                } else {
                    m_output("Syntax: watch <read|write> <address>\n");
                    return;
                }
                int id = m_ctx.dbg->breakpoints().add(addr, type);
                m_output("Watchpoint " + std::to_string(id) + " at $" + toHex(addr) + "\n");
            } else {
                m_output("Error: Invalid address '" + expr + "'\n");
            }
        } else {
            m_output("Syntax: watch <read|write> <address>\n");
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
                        m_output("Symbol added: " + label + " = $" + toHex(addr) + "\n");
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
                    out << std::setfill('0') << std::uppercase << std::hex;
                    for (const auto& pair : syms) {
                        out << "$" << std::setw(4) << pair.first << "  " << pair.second << "\n";
                    }
                    m_output(out.str());
                }
            } else if (sub == "search") {
                std::string query;
                if (ss >> query) {
                    auto syms = m_ctx.dbg->symbols().symbols();
                    std::stringstream out;
                    bool found = false;
                    for (const auto& pair : syms) {
                        if (pair.second.find(query) != std::string::npos) {
                            out << "$" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << pair.first << "  " << pair.second << "\n";
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
            } else if (sub == "clear") {
                m_ctx.dbg->symbols().clear();
                m_output("Symbol table cleared.\n");
            } else {
                m_output("Unknown sym subcommand: " + sub + "\n");
            }
        } else {
            m_output("Usage: sym <add|del|list|search|load|clear>\n");
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
            if (parseAddr(expr, addr)) {
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
                char buf[16];
                snprintf(buf, sizeof(buf), "%04X", i);
                m_output("Found at $" + std::string(buf) + "\n");
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
                char buf[16];
                snprintf(buf, sizeof(buf), "%04X", i);
                m_output("Found at $" + std::string(buf) + "\n");
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
                char buf[16];
                snprintf(buf, sizeof(buf), "%04X", curr);
                m_output("Found at $" + std::string(buf) + "\n");
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
                char buf[16];
                snprintf(buf, sizeof(buf), "%04X", curr);
                m_output("Found at $" + std::string(buf) + "\n");
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
            res << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << addr << ": ";
            int bytes = m_ctx.disasm->disasmOne(m_ctx.bus, addr, buf, sizeof(buf));
            res << buf << "\n";
            m_output(res.str());
            addr += bytes;
        }
    } else if (cmd == "asm") {
        if (!m_ctx.cpu || !m_ctx.assem) { m_output("No machine created.\n"); return; }
        std::string expr;
        if (ss >> expr) {
            uint32_t addr;
            if (parseAddr(expr, addr)) {
                m_asmAddr = addr;
                m_asmMode = true;
            } else {
                m_output("Error: Invalid address '" + expr + "'\n");
                return;
            }
        } else {
            m_output("Syntax: asm <address>\n");
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
                    // Show current assembler
                    std::string current = m_ctx.assemblerOverride.empty()
                        ? (m_ctx.machine && !m_ctx.machine->preferredAssembler.empty()
                            ? m_ctx.machine->preferredAssembler
                            : "default")
                        : m_ctx.assemblerOverride;
                    m_output("Current assembler: " + current + "\n");
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

void CliInterpreter::printHelp() {
    m_output("Available commands:\n"
             "  help, ?          - Show this help\n"
             "  list             - List available machine types\n"
             "  create <id>      - Create a machine of the given type\n"
             "  step [n]         - Step the CPU N times (default 1)\n"
             "  run              - Run until a breakpoint or stop\n"
             "  setpc <addr>     - Set the CPU program counter\n"
             "  regs             - Show CPU registers\n"
             "  m <addr> [len]   - Dump memory\n"
             "  f <addr> <val> [len] - Fill memory range\n"
             "  copy <src> <dst> <len> - Copy memory range\n"
             "  swap <addr1> <addr2> <len> - Swap two memory ranges\n"
             "  search <hex1>... - Search for hex pattern in memory (all matches)\n"
             "  searcha <str>    - Search for ASCII string in memory (all matches)\n"
             "  findnext         - Find next occurrence of last search pattern\n"
             "  findprior        - Find prior occurrence of last search pattern\n"
             "  disasm <addr> [n]- Disassemble N instructions\n"
             "  asm <addr>       - Interactive assembly mode (end with '.')\n"
             "  type <text>      - Type text into the machine (supports \\n)\n"
             "  key <name> <state>- Press/release a key (state: 1/0 or down/up)\n"
             "  load <path> [addr]- Load a program/binary file\n"
             "  screenshot <file>  - Save current screen to a PNG file\n"
             "  cart <path>      - Attach a cartridge image\n"
             "  tape mount <path>   - Mount a .tap file for playback\n"
             "  tape play           - Press Play (start/resume playback)\n"
             "  tape stop           - Release all tape buttons\n"
             "  tape rewind         - Rewind to start of tape\n"
             "  tape record         - Press Record (capture write-line to memory buffer)\n"
             "  tape stoprecord     - Release Record button (stops capture, buffer retained)\n"
             "  tape save <path>    - Write captured buffer to a .tap file\n"
             "  (record/stoprecord/save: use together to save a program to tape)\n"
             "  disk mount <unit> <path> - Mount a disk image\n"
             "  disk eject <unit>        - Eject a disk image\n"
             "  eject            - Eject currently attached cartridge\n"
             "  run [addr]       - Run from address (or last loaded address)\n"
             "  sym <add|del|list|search|load|clear> - Symbol table management\n"
             "  .<instr>         - Assemble and execute a single instruction\n"             "  quit, q          - Exit the program\n"
             "\nDebugging:\n"
             "  break <addr>     - Set execution breakpoint at address\n"
             "  watch read <addr> - Set read watchpoint at address\n"
             "  watch write <addr>- Set write watchpoint at address\n"
             "  delete <id>      - Delete breakpoint/watchpoint by id\n"
             "  enable <id>      - Enable breakpoint/watchpoint\n"
             "  disable <id>     - Disable breakpoint/watchpoint\n"
             "  info breaks      - List all breakpoints and watchpoints\n"
             "  stack [n]        - Show stack trace (default 8 most recent entries)\n");

    std::vector<std::string> pluginCmds;
    PluginCommandRegistry::instance().listCommands(pluginCmds);
    if (!pluginCmds.empty()) {
        m_output("\nPlugin commands:\n");
        for (const auto& s : pluginCmds) m_output(s + "\n");
    }
}

void CliInterpreter::dumpMemory(uint32_t addr, uint32_t len) {
    std::stringstream res;
    for (uint32_t i = 0; i < len; i += 16) {
        res << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << (addr + i) << ": ";
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
