#include "gdb_server.h"
#include "libcore/main/icore.h"
#include "libmem/main/ibus.h"
#include "libdebug/main/debug_context.h"
#include "libdebug/main/breakpoint_list.h"
#include "libtoolchain/main/variable_symbol.h"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cerrno>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string GdbServer::toHexByte(uint8_t v) {
    char buf[3];
    std::snprintf(buf, sizeof(buf), "%02x", v);
    return buf;
}

std::string GdbServer::toHex16LE(uint16_t v) {
    // GDB expects little-endian byte order in hex
    return toHexByte(v & 0xFF) + toHexByte((v >> 8) & 0xFF);
}

uint8_t GdbServer::fromHex(char hi, char lo) {
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return 0;
    };
    return (nibble(hi) << 4) | nibble(lo);
}

// ---------------------------------------------------------------------------
// Register mapping: GDB 6502 order → ICore register index
// GDB order: A(0), X(1), Y(2), SP(3), PC(4), P(5)
// ---------------------------------------------------------------------------

int GdbServer::mapGdbRegToCore(int gdbIdx) const {
    // Map by name since ICore register indices vary by CPU type
    static const char* gdbRegNames[] = {"A", "X", "Y", "SP", "PC", "P"};
    if (gdbIdx < 0 || gdbIdx >= GDB_REG_COUNT) return -1;
    int rc = m_cpu->regCount();
    for (int i = 0; i < rc; ++i) {
        if (std::strcmp(m_cpu->regDescriptor(i)->name, gdbRegNames[gdbIdx]) == 0)
            return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// RSP packet framing
// ---------------------------------------------------------------------------

std::string GdbServer::recvPacket(int fd) {
    // Read until we get a complete $...#xx packet
    // Also handle bare 0x03 (Ctrl+C interrupt)
    std::string buf;
    char c;

    // Skip until '$' or 0x03
    while (true) {
        if (m_stopRequested) return "";
        struct pollfd pfd = {fd, POLLIN, 0};
        int pr = poll(&pfd, 1, 100); // 100ms timeout for stop checking
        if (pr <= 0) continue;
        int n = read(fd, &c, 1);
        if (n <= 0) return "";
        if (c == 0x03) return "\x03"; // Interrupt
        if (c == '$') break;
    }

    // Read payload until '#'
    while (true) {
        if (m_stopRequested) return "";
        int n = read(fd, &c, 1);
        if (n <= 0) return "";
        if (c == '#') break;
        buf += c;
    }

    // Read 2-char checksum (we don't validate it, just consume)
    char cs[2];
    if (read(fd, cs, 2) != 2) return "";

    // Send ACK
    char ack = '+';
    if (write(fd, &ack, 1) < 0) return "";

    return buf;
}

void GdbServer::sendPacket(int fd, const std::string& data) {
    // Compute checksum
    uint8_t cksum = 0;
    for (char c : data) cksum += (uint8_t)c;

    std::string pkt = "$" + data + "#" + toHexByte(cksum);
    if (write(fd, pkt.c_str(), pkt.size()) < 0) return;

    // Wait for ACK ('+') — non-blocking with timeout
    struct pollfd pfd = {fd, POLLIN, 0};
    if (poll(&pfd, 1, 1000) > 0) {
        char ack;
        if (read(fd, &ack, 1) < 0) return;
    }
}

void GdbServer::sendError(int fd, int code) {
    sendPacket(fd, "E" + toHexByte(code));
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

std::string GdbServer::handleQuery(const std::string& pkt) {
    if (pkt.find("qSupported") == 0) {
        // Advertise mmemu-specific extensions for metadata
        return "PacketSize=4096;swbreak+;hwbreak-;mmemu-symbols+;mmemu-variables+;mmemu-frame+";
    }
    if (pkt == "qAttached") {
        return "1"; // Attached to existing process
    }
    if (pkt == "qC") {
        return "QC1"; // Thread ID 1
    }
    if (pkt.find("qfThreadInfo") == 0) {
        return "m1";
    }
    if (pkt.find("qsThreadInfo") == 0) {
        return "l"; // End of list
    }
    // mmemu-specific metadata queries (Issue #100)
    if (pkt.find("qMmemuSymbols:") == 0) {
        return handleQuerySymbols(pkt.substr(14)); // Skip "qMmemuSymbols:"
    }
    if (pkt.find("qMmemuVariables:") == 0) {
        return handleQueryVariables(pkt.substr(16)); // Skip "qMmemuVariables:"
    }
    if (pkt == "qMmemuFrame") {
        return handleQueryFrameInfo();
    }
    return ""; // Empty = unsupported
}

std::string GdbServer::handleReadRegs() {
    std::string result;
    // GDB 6502: A, X, Y, SP (8-bit each), PC (16-bit LE), P (8-bit)
    for (int g = 0; g < GDB_REG_COUNT; ++g) {
        int ci = mapGdbRegToCore(g);
        if (ci < 0) {
            result += (g == 4) ? "0000" : "00"; // PC is 16-bit
            continue;
        }
        uint32_t val = m_cpu->regRead(ci);
        if (g == 4) { // PC is 16-bit LE
            result += toHex16LE(val & 0xFFFF);
        } else {
            result += toHexByte(val & 0xFF);
        }
    }
    return result;
}

std::string GdbServer::handleWriteRegs(const std::string& data) {
    // Parse hex pairs: A(1), X(1), Y(1), SP(1), PC(2 bytes LE), P(1)
    size_t pos = 0;
    for (int g = 0; g < GDB_REG_COUNT && pos + 1 < data.size(); ++g) {
        int ci = mapGdbRegToCore(g);
        if (g == 4) { // PC: 2 bytes LE
            if (pos + 3 >= data.size()) break;
            uint8_t lo = fromHex(data[pos], data[pos+1]);
            uint8_t hi = fromHex(data[pos+2], data[pos+3]);
            if (ci >= 0) m_cpu->regWrite(ci, lo | (hi << 8));
            pos += 4;
        } else {
            uint8_t val = fromHex(data[pos], data[pos+1]);
            if (ci >= 0) m_cpu->regWrite(ci, val);
            pos += 2;
        }
    }
    return "OK";
}

std::string GdbServer::handleReadMem(const std::string& params) {
    // Format: addr,length (hex)
    size_t comma = params.find(',');
    if (comma == std::string::npos) return "E01";
    uint32_t addr = std::stoul(params.substr(0, comma), nullptr, 16);
    uint32_t len = std::stoul(params.substr(comma + 1), nullptr, 16);
    if (len > 0x10000) len = 0x10000;

    std::string result;
    result.reserve(len * 2);
    for (uint32_t i = 0; i < len; ++i)
        result += toHexByte(m_bus->peek8(addr + i));
    return result;
}

std::string GdbServer::handleWriteMem(const std::string& params) {
    // Format: addr,length:hex_data
    size_t comma = params.find(',');
    size_t colon = params.find(':');
    if (comma == std::string::npos || colon == std::string::npos) return "E01";
    uint32_t addr = std::stoul(params.substr(0, comma), nullptr, 16);
    std::string hexData = params.substr(colon + 1);

    for (size_t i = 0; i + 1 < hexData.size(); i += 2) {
        m_bus->write8(addr++, fromHex(hexData[i], hexData[i+1]));
    }
    return "OK";
}

std::string GdbServer::handleStep() {
    m_cpu->step();
    return "S05"; // SIGTRAP
}

std::string GdbServer::handleContinue(int fd) {
    if (m_dbg) m_dbg->resume();

    // Set fd to non-blocking so we can check for interrupt (0x03)
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int steps = 0;
    const int maxSteps = 100000000;
    bool interrupted = false;

    while (steps < maxSteps && !m_stopRequested) {
        m_cpu->step();
        ++steps;

        // Check for breakpoint hit
        if (m_dbg && m_dbg->isPaused()) break;

        // Check for BRK
        if (m_bus->peek8(m_cpu->pc()) == 0x00) break;

        // Periodically check for interrupt from GDB (every 1024 steps)
        if ((steps & 0x3FF) == 0) {
            char c;
            int n = read(fd, &c, 1);
            if (n > 0 && c == 0x03) {
                interrupted = true;
                break;
            }
        }
    }

    // Restore blocking mode
    fcntl(fd, F_SETFL, flags);

    if (interrupted) return "S02"; // SIGINT
    return "S05"; // SIGTRAP (breakpoint or step)
}

std::string GdbServer::handleInsertBreakpoint(const std::string& params) {
    // Format: type,addr,kind
    // type 0 = software breakpoint
    size_t c1 = params.find(',');
    size_t c2 = params.find(',', c1 + 1);
    if (c1 == std::string::npos || c2 == std::string::npos) return "E01";

    int type = std::stoi(params.substr(0, c1));
    if (type != 0) return ""; // Only software breakpoints supported

    uint32_t addr = std::stoul(params.substr(c1 + 1, c2 - c1 - 1), nullptr, 16);
    if (m_dbg) {
        m_dbg->breakpoints().add(addr, BreakpointType::EXEC);
    }
    return "OK";
}

std::string GdbServer::handleRemoveBreakpoint(const std::string& params) {
    size_t c1 = params.find(',');
    size_t c2 = params.find(',', c1 + 1);
    if (c1 == std::string::npos || c2 == std::string::npos) return "E01";

    int type = std::stoi(params.substr(0, c1));
    if (type != 0) return "";

    uint32_t addr = std::stoul(params.substr(c1 + 1, c2 - c1 - 1), nullptr, 16);
    if (m_dbg) {
        // Find and remove breakpoint at this address
        for (const auto& bp : m_dbg->breakpoints().breakpoints()) {
            if (bp.addr == addr && bp.type == BreakpointType::EXEC) {
                m_dbg->breakpoints().remove(bp.id);
                break;
            }
        }
    }
    return "OK";
}

// ---------------------------------------------------------------------------
// Server lifecycle
// ---------------------------------------------------------------------------

GdbServer::GdbServer(ICore* cpu, IBus* bus, DebugContext* dbg)
    : m_cpu(cpu), m_bus(bus), m_dbg(dbg) {}

GdbServer::~GdbServer() {
    stop();
}

bool GdbServer::start(uint16_t port) {
    m_listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listenFd < 0) return false;

    int opt = 1;
    setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (bind(m_listenFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(m_listenFd);
        m_listenFd = -1;
        return false;
    }

    if (listen(m_listenFd, 1) < 0) {
        close(m_listenFd);
        m_listenFd = -1;
        return false;
    }

    m_stopRequested = false;
    m_running = true;
    m_thread = std::thread(&GdbServer::serverLoop, this);
    return true;
}

void GdbServer::stop() {
    m_stopRequested = true;
    if (m_clientFd >= 0) { close(m_clientFd); m_clientFd = -1; }
    if (m_listenFd >= 0) { close(m_listenFd); m_listenFd = -1; }
    if (m_thread.joinable()) m_thread.join();
    m_running = false;
}

void GdbServer::serverLoop() {
    while (!m_stopRequested) {
        // Wait for connection with timeout
        struct pollfd pfd = {m_listenFd, POLLIN, 0};
        int pr = poll(&pfd, 1, 500);
        if (pr <= 0) continue;

        struct sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(m_listenFd, (struct sockaddr*)&clientAddr, &clientLen);
        if (clientFd < 0) continue;

        m_clientFd = clientFd;
        handleClient(clientFd);
        close(clientFd);
        m_clientFd = -1;
    }
}

void GdbServer::handleClient(int fd) {
    while (!m_stopRequested) {
        std::string pkt = recvPacket(fd);
        if (pkt.empty()) break;

        std::string reply;

        if (pkt == "\x03") {
            // Ctrl+C interrupt — report current state
            reply = "S02"; // SIGINT
        } else if (pkt[0] == 'q' || pkt[0] == 'Q') {
            reply = handleQuery(pkt);
        } else if (pkt == "?") {
            reply = "S05"; // Halted (SIGTRAP)
        } else if (pkt == "g") {
            reply = handleReadRegs();
        } else if (pkt[0] == 'G') {
            reply = handleWriteRegs(pkt.substr(1));
        } else if (pkt[0] == 'm') {
            reply = handleReadMem(pkt.substr(1));
        } else if (pkt[0] == 'M') {
            reply = handleWriteMem(pkt.substr(1));
        } else if (pkt == "s") {
            reply = handleStep();
        } else if (pkt[0] == 'c') {
            reply = handleContinue(fd);
        } else if (pkt[0] == 'Z') {
            reply = handleInsertBreakpoint(pkt.substr(1));
        } else if (pkt[0] == 'z') {
            reply = handleRemoveBreakpoint(pkt.substr(1));
        } else if (pkt == "D") {
            // Detach
            sendOk(fd);
            break;
        } else if (pkt == "k") {
            // Kill — just disconnect
            break;
        } else if (pkt[0] == 'H') {
            // Set thread — we only have one, always OK
            reply = "OK";
        } else if (pkt == "vMustReplyEmpty") {
            reply = "";
        } else {
            reply = ""; // Unsupported = empty response
        }

        sendPacket(fd, reply);
    }
}

// mmemu-specific metadata handlers (Issue #100 - IDE integration)

std::string GdbServer::handleQuerySymbols(const std::string& params) {
    if (!m_dbg) return "";

    // Return symbol table as hex-encoded JSON
    // Format: qMmemuSymbols:search_pattern
    // Response: JSON with symbols array
    // Example response: {"symbols":[{"name":"main","addr":"0x2000"},{"name":"loop","addr":"0x2010"}]}

    std::string response = "{\"symbols\":[";
    const auto& symTable = m_dbg->symbols().symbols();
    bool first = true;
    for (const auto& [addr, label] : symTable) {
        if (!first) response += ",";
        response += "{\"name\":\"" + label + "\",\"addr\":\"0x";
        char buf[16];
        snprintf(buf, sizeof(buf), "%04x", addr);
        response += buf;
        response += "\"}";
        first = false;
    }
    response += "]}";

    // Hex-encode the response
    std::string encoded;
    for (char c : response) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", (unsigned char)c);
        encoded += hex;
    }
    return encoded;
}

std::string GdbServer::handleQueryVariables(const std::string& params) {
    if (!m_dbg) return "";

    // Return variable information as hex-encoded JSON
    // Format: qMmemuVariables:function_name
    // Response: JSON with variables array
    // Example: {"variables":[{"name":"x","addr":"0x10","size":2,"type":"int16"}]}

    std::string response = "{\"variables\":[";
    auto vars = m_dbg->variables().getVariablesInFunction(params);
    bool first = true;
    for (const auto* var : vars) {
        if (!first) response += ",";
        response += "{\"name\":\"" + var->displayName + "\",\"addr\":\"0x";
        char buf[16];
        snprintf(buf, sizeof(buf), "%04x", var->address);
        response += buf;
        response += "\",\"size\":" + std::to_string(var->size) + ",\"type\":\"" + formatVariableType(var->type) + "\"}";
        first = false;
    }
    response += "]}";

    // Hex-encode the response
    std::string encoded;
    for (char c : response) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", (unsigned char)c);
        encoded += hex;
    }
    return encoded;
}

std::string GdbServer::handleQueryFrameInfo() {
    if (!m_dbg || !m_dbg->cpu()) return "";

    // Return frame information as hex-encoded JSON
    // Response: {"pc":"0x2000","sp":"0xff","frameSize":256}

    uint32_t pc = m_dbg->cpu()->pc();
    uint32_t sp = m_dbg->cpu()->regRead(3); // SP is usually register 3

    std::string response = "{\"pc\":\"0x";
    char buf[16];
    snprintf(buf, sizeof(buf), "%04x", pc);
    response += buf;
    response += "\",\"sp\":\"0x";
    snprintf(buf, sizeof(buf), "%02x", (unsigned int)sp);
    response += buf;
    response += "\",\"frameSize\":256}";

    // Hex-encode the response
    std::string encoded;
    for (char c : response) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", (unsigned char)c);
        encoded += hex;
    }
    return encoded;
}
