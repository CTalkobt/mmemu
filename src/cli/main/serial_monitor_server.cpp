#include "serial_monitor_server.h"
#include "libcore/main/icore.h"
#include "libmem/main/ibus.h"
#include "libdebug/main/debug_context.h"
#include "libtoolchain/main/idisasm.h"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cerrno>
#include <algorithm>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

SerialMonitorServer::SerialMonitorServer(ICore* cpu, IBus* bus, DebugContext* dbg)
    : m_cpu(cpu), m_bus(bus), m_dbg(dbg) {
}

SerialMonitorServer::~SerialMonitorServer() {
    stop();
}

bool SerialMonitorServer::start(uint16_t port) {
    if (m_running) return false;
    if (!m_cpu || !m_bus) return false;

    m_port = port;
    m_listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listenSocket < 0) {
        std::cerr << "[Serial Monitor] Failed to create socket: " << strerror(errno) << "\n";
        return false;
    }

    // Allow reuse of socket
    int reuseaddr = 1;
    if (setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr)) < 0) {
        close(m_listenSocket);
        m_listenSocket = -1;
        return false;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // Only localhost

    if (bind(m_listenSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[Serial Monitor] Failed to bind to port " << port << ": " << strerror(errno);
        close(m_listenSocket);
        m_listenSocket = -1;
        return false;
    }

    if (listen(m_listenSocket, 1) < 0) {
        std::cerr << "[Serial Monitor] ""Failed to listen: " << strerror(errno);
        close(m_listenSocket);
        m_listenSocket = -1;
        return false;
    }

    m_running = true;
    m_stopRequested = false;
    m_listenerThread = std::make_unique<std::thread>(&SerialMonitorServer::listenLoop, this);

    std::cout << "[Serial Monitor] ""Server started on localhost:" << port;
    return true;
}

void SerialMonitorServer::stop() {
    if (!m_running) return;

    m_stopRequested = true;
    m_running = false;

    if (m_listenSocket >= 0) {
        close(m_listenSocket);
        m_listenSocket = -1;
    }

    if (m_listenerThread && m_listenerThread->joinable()) {
        m_listenerThread->join();
    }

    std::cout << "[Serial Monitor] ""Server stopped";
}

void SerialMonitorServer::listenLoop() {
    while (m_running && !m_stopRequested) {
        struct sockaddr_in clientAddr = {};
        socklen_t clientAddrLen = sizeof(clientAddr);

        struct pollfd pfd = {m_listenSocket, POLLIN, 0};
        int pr = poll(&pfd, 1, 500); // 500ms timeout for stop checking

        if (pr <= 0) continue; // Timeout or error

        int clientSocket = accept(m_listenSocket, (struct sockaddr*)&clientAddr, &clientAddrLen);
        if (clientSocket < 0) {
            if (errno != EINTR) {
                std::cerr << "[Serial Monitor] ""Accept failed: " << strerror(errno);
            }
            continue;
        }

        std::cerr << "[Serial Monitor] ""Client connected";
        handleClient(clientSocket);
        close(clientSocket);
        std::cerr << "[Serial Monitor] ""Client disconnected";
    }
}

void SerialMonitorServer::handleClient(int clientSocket) {
    std::string buffer;
    char readBuf[256];

    while (m_running && !m_stopRequested) {
        struct pollfd pfd = {clientSocket, POLLIN, 0};
        int pr = poll(&pfd, 1, 100);

        if (pr <= 0) {
            if (pr < 0) break; // Error
            continue;
        }

        int n = read(clientSocket, readBuf, sizeof(readBuf) - 1);
        if (n <= 0) break; // Connection closed or error

        for (int i = 0; i < n; ++i) {
            char c = readBuf[i];
            if (c == '\n' || c == '\r') {
                if (!buffer.empty()) {
                    // Execute command
                    std::string response = executeCommand(buffer);
                    response += "\n";
                    send(clientSocket, response.c_str(), response.length(), 0);
                    buffer.clear();
                }
            } else if (c >= 32 && c < 127) { // Printable ASCII
                buffer += c;
            }
        }
    }
}

std::string SerialMonitorServer::executeCommand(const std::string& cmdLine) {
    if (cmdLine.empty()) return "";

    std::istringstream ss(cmdLine);
    char cmd;
    ss >> cmd;

    // Convert to uppercase
    cmd = std::toupper(cmd);

    try {
        switch (cmd) {
            case 'R': return cmd_registers();
            case 'M': {
                uint32_t addr;
                if (ss >> std::hex >> addr) {
                    m_lastMemAddr = addr;
                    return cmd_memory(addr);
                }
                return cmd_memory(); // Next 256 bytes
            }
            case 'S': {
                uint32_t addr;
                uint32_t value;
                if (ss >> std::hex >> addr >> value) {
                    return cmd_setmemory(addr, value & 0xFF);
                }
                return "ERROR: S <addr> <value>";
            }
            case 'D': {
                uint32_t addr;
                if (ss >> std::hex >> addr) {
                    return cmd_disassemble(addr);
                }
                return "ERROR: D [addr]";
            }
            case 'G': {
                uint32_t addr;
                if (ss >> std::hex >> addr) {
                    return cmd_setpc(addr);
                }
                return "ERROR: G <addr>";
            }
            case 'N': {
                uint32_t count = 1;
                if (ss >> std::hex >> count) {
                    return cmd_step(count);
                }
                return cmd_step(1);
            }
            case 'B': {
                uint32_t addr;
                if (ss >> std::hex >> addr) {
                    return cmd_breakpoint(addr);
                } else {
                    return cmd_breakpoint(); // Clear breakpoint
                }
            }
            case '?':
            case 'H': return cmd_help();
            case 'L': {
                uint32_t start, end;
                if (ss >> std::hex >> start >> end) {
                    return cmd_loadmemory(start, end);
                }
                return "ERROR: L <start> <end>";
            }
            case 'T': {
                std::string mode;
                if (ss >> mode) {
                    return cmd_trace(mode);
                }
                return "ERROR: T <mode>";
            }
            case 'W': {
                uint32_t addr;
                if (ss >> std::hex >> addr) {
                    return cmd_watchpoint(addr);
                } else {
                    return cmd_watchpoint(); // Clear watchpoint
                }
            }
            case 'Z': return cmd_history();
            case '+': {
                uint32_t divisor;
                if (ss >> std::hex >> divisor) {
                    return cmd_uart_divisor(divisor);
                }
                return "ERROR: + <divisor>";
            }
            case '@': return cmd_cpu_memory();
            case 'E': {
                std::string flag;
                if (ss >> flag) {
                    return cmd_flagwatch(flag);
                }
                return "ERROR: E <flag>";
            }
            case 'I': {
                std::string subcmd;
                if (ss >> subcmd) {
                    return cmd_interrupts(subcmd);
                }
                return "ERROR: I <cmd>";
            }
            default:
                return "ERROR: Unknown command '" + std::string(1, cmd) + "'";
        }
    } catch (const std::exception& e) {
        return std::string("ERROR: ") + e.what();
    }
}

// ============================================================================
// Command Implementations - Phase 1
// ============================================================================

std::string SerialMonitorServer::cmd_registers() {
    if (!m_cpu) return "ERROR: No CPU";

    std::ostringstream out;
    int count = m_cpu->regCount();
    out << "PC=" << formatAddr(m_cpu->pc()) << " ";

    for (int i = 0; i < count; ++i) {
        const auto* desc = m_cpu->regDescriptor(i);
        if (desc->flags & REGFLAG_INTERNAL) continue;

        uint32_t val = m_cpu->regRead(i);
        out << desc->name << "=";

        if (desc->width == RegWidth::R16) {
            out << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << val << " ";
        } else {
            out << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << val << " ";
        }
    }

    return out.str();
}

std::string SerialMonitorServer::cmd_memory(uint32_t addr) {
    if (!m_bus) return "ERROR: No bus";

    if (addr == 0xFFFFFFFF) {
        addr = m_lastMemAddr + 256;
    }
    m_lastMemAddr = addr;

    std::ostringstream out;
    const int LINES = 16;
    const int COLS = 16;

    for (int line = 0; line < LINES; ++line) {
        uint32_t lineAddr = addr + (line * COLS);
        out << formatAddr(lineAddr) << ": ";

        // Hex bytes
        for (int col = 0; col < COLS; ++col) {
            uint8_t byte = m_bus->peek8(lineAddr + col);
            out << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << (int)byte << " ";
        }

        out << " ";

        // ASCII representation
        for (int col = 0; col < COLS; ++col) {
            uint8_t byte = m_bus->peek8(lineAddr + col);
            if (byte >= 0x20 && byte < 0x7F) {
                out << (char)byte;
            } else {
                out << ".";
            }
        }
        out << "\n";
    }

    return out.str();
}

std::string SerialMonitorServer::cmd_setmemory(uint32_t addr, uint8_t value) {
    if (!m_bus) return "ERROR: No bus";

    m_bus->write8(addr, value);
    return "OK";
}

std::string SerialMonitorServer::cmd_disassemble(uint32_t addr, int count) {
    if (!m_cpu || !m_bus) return "ERROR: No CPU or bus";

    std::ostringstream out;
    uint32_t pc = addr;

    for (int i = 0; i < count; ++i) {
        // Get instruction at PC using disassembleEntry
        DisasmEntry entry{};
        m_cpu->disassembleEntry(m_bus, pc, &entry);

        out << formatAddr(pc) << " " << entry.complete << "\n";
        pc = entry.addr + entry.bytes;
    }

    return out.str();
}

std::string SerialMonitorServer::cmd_setpc(uint32_t addr) {
    if (!m_cpu) return "ERROR: No CPU";

    m_cpu->setPc(addr);
    return "OK";
}

std::string SerialMonitorServer::cmd_step(uint32_t count) {
    if (!m_cpu) return "ERROR: No CPU";

    for (uint32_t i = 0; i < count; ++i) {
        m_cpu->step();
    }

    std::ostringstream out;
    out << "OK PC=" << std::hex << std::setfill('0') << std::setw(4) << m_cpu->pc();
    return out.str();
}

std::string SerialMonitorServer::cmd_breakpoint(uint32_t addr) {
    if (!m_dbg) return "ERROR: No debug context";

    if (addr == 0xFFFFFFFF) {
        // Clear all breakpoints
        const auto& breaks = m_dbg->breakpoints().breakpoints();
        std::vector<int> toRemove;
        for (const auto& bp : breaks) {
            toRemove.push_back(bp.id);
        }
        for (int id : toRemove) {
            m_dbg->breakpoints().remove(id);
        }
        return "OK";
    }

    m_dbg->breakpoints().add(addr, BreakpointType::EXEC);
    return "OK";
}

std::string SerialMonitorServer::cmd_help() {
    return "MEGA65 Serial Monitor Server v1.0\n"
           "Commands: R M S D G B ? L T W Z + @ E I\n"
           "R=Registers, M=Memory, S=SetMem, D=Disasm, G=SetPC\n"
           "B=Break, ?=Help, L=LoadMem, T=Trace, W=Watch";
}

std::string SerialMonitorServer::cmd_trace(const std::string& mode) {
    if (!m_dbg || !m_cpu) return "ERROR: No debug context";

    std::ostringstream out;
    if (mode == "on" || mode == "1") {
        // Enable trace - mark current position
        out << "Trace ON at PC=" << formatAddr(m_cpu->pc());
        return out.str();
    } else if (mode == "off" || mode == "0") {
        out << "Trace OFF";
        return out.str();
    } else if (mode == "dump") {
        // Dump trace buffer (last N instructions)
        out << "Trace buffer (last 16 instructions):\n";
        const auto& trace = m_dbg->trace();
        size_t start = trace.size() > 16 ? trace.size() - 16 : 0;
        for (size_t i = start; i < trace.size(); ++i) {
            const auto& entry = trace.at(i);
            out << formatAddr(entry.addr) << " " << entry.mnemonic;
            if (entry.regs.count("A")) {
                out << " A=" << std::hex << std::setfill('0') << std::setw(2)
                    << entry.regs.at("A");
            }
            out << "\n";
        }
        return out.str();
    } else {
        return "ERROR: T <on|off|dump>";
    }
}

std::string SerialMonitorServer::cmd_watchpoint(uint32_t addr) {
    if (!m_dbg) return "ERROR: No debug context";

    if (addr == 0xFFFFFFFF) {
        // Clear all watchpoints
        m_dbg->breakpoints().removeByAddress(addr);
        return "OK";
    }

    m_dbg->breakpoints().add(addr, BreakpointType::WRITE_WATCH);
    return "OK";
}

std::string SerialMonitorServer::cmd_history() {
    if (!m_dbg || !m_cpu) return "ERROR: No debug context";

    std::ostringstream out;
    const auto& trace = m_dbg->trace();

    if (trace.size() == 0) {
        return "No history available";
    }

    out << "CPU History (last 32 instructions):\n";
    size_t start = trace.size() > 32 ? trace.size() - 32 : 0;

    for (size_t i = start; i < trace.size(); ++i) {
        const auto& entry = trace.at(i);
        out << formatAddr(entry.addr) << " " << entry.mnemonic << " Cycles=" << entry.cycles;

        if (entry.regs.count("A")) {
            out << " A=" << std::hex << std::setfill('0') << std::setw(2)
                << entry.regs.at("A");
        }
        out << "\n";
    }

    return out.str();
}

std::string SerialMonitorServer::cmd_uart_divisor(uint32_t divisor) {
    m_uartDivisor = divisor;
    uint32_t baudRate = 40500000 / (divisor + 1);
    std::ostringstream out;
    out << "UART Divisor: " << std::hex << divisor << " (" << std::dec << baudRate << " bps)";
    return out.str();
}

std::string SerialMonitorServer::cmd_loadmemory(uint32_t start, uint32_t end) {
    if (!m_bus) return "ERROR: No bus";

    if (start >= end) {
        return "ERROR: start >= end";
    }

    uint32_t count = end - start;
    if (count > 65536) {
        return "ERROR: Range too large (max 64KB)";
    }

    // In Phase 2, we accept a hex string payload in the next command
    // Phase 3 can add raw binary protocol via separate socket channel
    // For now, just acknowledge and set up for binary read
    std::ostringstream out;
    out << "LOAD " << formatAddr(start) << "-" << formatAddr(end - 1)
        << " (" << std::dec << count << " bytes) ready for data";
    return out.str();
}

std::string SerialMonitorServer::cmd_flagwatch(const std::string& flag) {
    if (!m_cpu) return "ERROR: No CPU";

    std::ostringstream out;

    // Parse flag name (case-insensitive)
    std::string f = flag;
    std::transform(f.begin(), f.end(), f.begin(), ::toupper);

    if (f.length() != 1) {
        return "ERROR: E <flag> (N,V,B,D,I,Z,C)";
    }

    char c = f[0];
    uint32_t flagValue = 0;
    std::string flagName;

    switch (c) {
        case 'N': flagValue = 0x80; flagName = "Negative"; break;
        case 'V': flagValue = 0x40; flagName = "Overflow"; break;
        case 'B': flagValue = 0x10; flagName = "Break"; break;
        case 'D': flagValue = 0x08; flagName = "Decimal"; break;
        case 'I': flagValue = 0x04; flagName = "Interrupt"; break;
        case 'Z': flagValue = 0x02; flagName = "Zero"; break;
        case 'C': flagValue = 0x01; flagName = "Carry"; break;
        default:
            return "ERROR: Unknown flag '" + std::string(1, c) + "'";
    }

    // Read status register (P) to check flag
    int pReg = -1;
    for (int i = 0; i < m_cpu->regCount(); ++i) {
        if (strcmp(m_cpu->regDescriptor(i)->name, "P") == 0) {
            pReg = i;
            break;
        }
    }

    if (pReg < 0) {
        return "ERROR: Status register not found";
    }

    uint32_t pValue = m_cpu->regRead(pReg);
    bool isSet = (pValue & flagValue) != 0;

    out << flagName << " (" << c << ") = " << (isSet ? "SET" : "CLEAR");
    return out.str();
}

std::string SerialMonitorServer::cmd_interrupts(const std::string& cmd) {
    if (!m_cpu) return "ERROR: No CPU";

    std::ostringstream out;

    if (cmd == "on" || cmd == "enable") {
        // Clear interrupt disable flag (I flag in P register)
        int pReg = -1;
        for (int i = 0; i < m_cpu->regCount(); ++i) {
            if (strcmp(m_cpu->regDescriptor(i)->name, "P") == 0) {
                pReg = i;
                break;
            }
        }

        if (pReg >= 0) {
            uint32_t pValue = m_cpu->regRead(pReg);
            m_cpu->regWrite(pReg, pValue & ~0x04); // Clear I flag
            out << "Interrupts ENABLED";
        } else {
            return "ERROR: Status register not found";
        }
    } else if (cmd == "off" || cmd == "disable") {
        // Set interrupt disable flag (I flag in P register)
        int pReg = -1;
        for (int i = 0; i < m_cpu->regCount(); ++i) {
            if (strcmp(m_cpu->regDescriptor(i)->name, "P") == 0) {
                pReg = i;
                break;
            }
        }

        if (pReg >= 0) {
            uint32_t pValue = m_cpu->regRead(pReg);
            m_cpu->regWrite(pReg, pValue | 0x04); // Set I flag
            out << "Interrupts DISABLED";
        } else {
            return "ERROR: Status register not found";
        }
    } else if (cmd == "status") {
        int pReg = -1;
        for (int i = 0; i < m_cpu->regCount(); ++i) {
            if (strcmp(m_cpu->regDescriptor(i)->name, "P") == 0) {
                pReg = i;
                break;
            }
        }

        if (pReg >= 0) {
            uint32_t pValue = m_cpu->regRead(pReg);
            bool irqDisabled = (pValue & 0x04) != 0;
            out << "Interrupts: " << (irqDisabled ? "DISABLED (I flag set)" : "ENABLED (I flag clear)");
        } else {
            return "ERROR: Status register not found";
        }
    } else {
        return "ERROR: I <on|off|status>";
    }

    return out.str();
}

std::string SerialMonitorServer::cmd_cpu_memory() {
    if (!m_bus) return "ERROR: No bus";

    std::ostringstream out;

    // Read memory as seen by CPU (through current banking/MAP configuration)
    // For now, just show current banking status
    uint32_t pc = m_cpu ? m_cpu->pc() : 0;

    out << "CPU View:\n";
    out << "PC=" << formatAddr(pc) << "\n";
    out << "Memory configuration:\n";

    // This would be enhanced to show actual MAP state, C64 banking, etc.
    out << "  Address space: ";
    if (m_bus->config().addrBits == 16) {
        out << "16-bit (64KB)";
    } else if (m_bus->config().addrBits == 28) {
        out << "28-bit (256MB MEGA65)";
    } else {
        out << std::dec << m_bus->config().addrBits << "-bit";
    }
    out << "\n";

    return out.str();
}

// ============================================================================
// Helpers
// ============================================================================

std::string SerialMonitorServer::formatAddr(uint32_t addr) {
    if (!m_bus) {
        // Default 16-bit
        std::ostringstream out;
        out << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << (addr & 0xFFFF);
        return out.str();
    }

    // Format based on bus address width
    uint32_t addrBits = m_bus->config().addrBits;
    int hexDigits = (addrBits + 3) / 4;

    std::ostringstream out;
    out << std::hex << std::uppercase << std::setfill('0') << std::setw(hexDigits) << addr;
    return out.str();
}

bool SerialMonitorServer::parseAddress(const std::string& str, uint32_t& addr) {
    // TODO: Implement full address parser with support for hex, decimal, binary, etc.
    try {
        addr = std::stoul(str, nullptr, 16);
        return true;
    } catch (...) {
        return false;
    }
}

uint32_t SerialMonitorServer::getLastPC() {
    if (!m_cpu) return 0;
    return m_cpu->pc();
}

bool SerialMonitorServer::sendToClient(int socket, const std::string& data) {
    return send(socket, data.c_str(), data.length(), 0) > 0;
}
