#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <memory>

class ICore;
class IBus;
class DebugContext;
class ViceMonitorProtocol;

/**
 * VICE Monitor Server
 *
 * Implements the VICE remote monitor protocol over TCP, allowing
 * compatibility with VICE-based tools and IDEs (like C64IDE).
 *
 * This provides a drop-in replacement for VICE's monitor interface,
 * enabling tools designed for VICE to work with mmemu.
 *
 * Protocol: Text-based commands over TCP
 * Default port: 6510 (VICE default)
 * Connection: localhost only
 */
class ViceMonitorServer {
public:
    ViceMonitorServer(ICore* cpu, IBus* bus, DebugContext* dbg);
    ~ViceMonitorServer();

    /**
     * Start listening on the specified TCP port.
     * Runs in background thread.
     *
     * @param port TCP port to listen on (default: 6510)
     * @return true if successfully started, false on error
     */
    bool start(uint16_t port = 6510);

    /**
     * Stop the server and close all connections.
     */
    void stop();

    /**
     * Check if server is running.
     */
    bool isRunning() const { return m_running; }

    /**
     * Get the port the server is listening on.
     */
    uint16_t getPort() const { return m_port; }

private:
    ICore* m_cpu;
    IBus* m_bus;
    DebugContext* m_dbg;
    std::unique_ptr<ViceMonitorProtocol> m_protocol;

    uint16_t m_port = 0;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::unique_ptr<std::thread> m_listenerThread;
    int m_listenSocket = -1;

    // Background thread entry point
    void listenLoop();

    // Handle a single client connection
    void handleClient(int clientSocket);

    // Helper: read line from socket
    bool readLine(int socket, std::string& line);

    // Helper: write response to socket
    bool writeLine(int socket, const std::string& response);
};
