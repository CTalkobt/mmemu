#include "vice_monitor_server.h"
#include "vice_monitor_protocol.h"
#include "libcore/main/icore.h"
#include "libmem/main/ibus.h"
#include "libdebug/main/debug_context.h"

#include <iostream>
#include <cstring>
#include <cerrno>
#include <algorithm>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

ViceMonitorServer::ViceMonitorServer(ICore* cpu, IBus* bus, DebugContext* dbg)
    : m_cpu(cpu), m_bus(bus), m_dbg(dbg) {
    m_protocol = std::make_unique<ViceMonitorProtocol>(cpu, bus, dbg);
}

ViceMonitorServer::~ViceMonitorServer() {
    stop();
}

bool ViceMonitorServer::start(uint16_t port) {
    if (m_running) return false;
    if (!m_cpu || !m_bus) return false;

    m_port = port;
    m_listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listenSocket < 0) {
        std::cerr << "[VICE Monitor] Failed to create socket: " << strerror(errno) << "\n";
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
        std::cerr << "[VICE Monitor] Failed to bind to port " << port << ": " << strerror(errno) << "\n";
        close(m_listenSocket);
        m_listenSocket = -1;
        return false;
    }

    if (listen(m_listenSocket, 1) < 0) {
        std::cerr << "[VICE Monitor] Failed to listen: " << strerror(errno) << "\n";
        close(m_listenSocket);
        m_listenSocket = -1;
        return false;
    }

    m_running = true;
    m_stopRequested = false;
    m_listenerThread = std::make_unique<std::thread>(&ViceMonitorServer::listenLoop, this);

    std::cout << "[VICE Monitor] Server started on localhost:" << port << " (VICE protocol compatible)\n";
    return true;
}

void ViceMonitorServer::stop() {
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

    std::cout << "[VICE Monitor] Server stopped\n";
}

void ViceMonitorServer::listenLoop() {
    while (m_running && !m_stopRequested) {
        struct sockaddr_in clientAddr = {};
        socklen_t clientAddrLen = sizeof(clientAddr);

        struct pollfd pfd = {m_listenSocket, POLLIN, 0};
        int pr = poll(&pfd, 1, 500); // 500ms timeout for stop checking

        if (pr <= 0) continue; // Timeout or error

        int clientSocket = accept(m_listenSocket, (struct sockaddr*)&clientAddr, &clientAddrLen);
        if (clientSocket < 0) {
            if (errno != EINTR) {
                std::cerr << "[VICE Monitor] Accept failed: " << strerror(errno) << "\n";
            }
            continue;
        }

        std::cerr << "[VICE Monitor] Client connected\n";
        handleClient(clientSocket);
        close(clientSocket);
        std::cerr << "[VICE Monitor] Client disconnected\n";
    }
}

void ViceMonitorServer::handleClient(int clientSocket) {
    std::string buffer;
    char readBuf[256];

    // Send initial banner
    std::string banner = "VICE emulation disabled (mmemu VICE protocol adapter)\n";
    writeLine(clientSocket, banner);

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
                    std::string response = m_protocol->executeCommand(buffer);

                    // Handle special responses
                    if (response == "EXIT") {
                        return; // Close connection
                    }

                    // Send response (multi-line responses already have newlines)
                    if (!response.empty()) {
                        writeLine(clientSocket, response);
                    }

                    buffer.clear();
                }
            } else if (c >= 32 && c < 127) { // Printable ASCII
                buffer += c;
            }
        }
    }
}

bool ViceMonitorServer::readLine(int socket, std::string& line) {
    line.clear();
    char c;

    while (true) {
        ssize_t n = read(socket, &c, 1);
        if (n <= 0) return false; // Connection closed or error

        if (c == '\n') {
            return true;
        }

        if (c == '\r') {
            continue; // Skip carriage return
        }

        if (c >= 32 && c < 127) {
            line += c;
        }
    }
}

bool ViceMonitorServer::writeLine(int socket, const std::string& response) {
    std::string output = response;
    if (!output.empty() && output.back() != '\n') {
        output += "\n";
    }

    ssize_t n = send(socket, output.c_str(), output.length(), 0);
    return n > 0;
}
