#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <memory>

class ICore;
class IBus;
class DebugContext;

/**
 * Serial Monitor Server - MEGA65 Compatible
 *
 * Implements a virtual serial monitor interface matching the MEGA65's
 * serial monitor protocol, exposed as a TCP loopback port.
 *
 * This enables compatibility with MEGA65 development tools (m65 CLI, etc.)
 * and allows them to interact with the emulator.
 *
 * Protocol: Text-based commands over TCP (matching hardware)
 * Default port: configurable via --serial-monitor-port CLI flag
 * Baud rate simulation: 2,000,000 bps default (controllable via + command)
 */
class SerialMonitorServer {
public:
    SerialMonitorServer(ICore* cpu, IBus* bus, DebugContext* dbg);
    ~SerialMonitorServer();

    /**
     * Start listening on the specified TCP port.
     * Runs in background thread.
     *
     * @param port TCP port to listen on
     * @return true if successfully started, false on error
     */
    bool start(uint16_t port);

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

    uint16_t m_port = 0;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::unique_ptr<std::thread> m_listenerThread;
    int m_listenSocket = -1;

    // Current UART divisor (affects baud rate simulation)
    uint32_t m_uartDivisor = 20; // Default 2,000,000 bps

    // Background thread entry point
    void listenLoop();

    // Handle a single client connection
    void handleClient(int clientSocket);

    // Parse and execute a command, return response
    std::string executeCommand(const std::string& cmdLine);

    // Command handlers
    std::string cmd_registers();                                  // R
    std::string cmd_memory(uint32_t addr = 0xFFFFFFFF);          // M
    std::string cmd_setmemory(uint32_t addr, uint8_t value);    // S
    std::string cmd_disassemble(uint32_t addr, int count = 16);  // D
    std::string cmd_setpc(uint32_t addr);                        // G
    std::string cmd_breakpoint(uint32_t addr = 0xFFFFFFFF);     // B
    std::string cmd_help();                                      // ?
    std::string cmd_trace(const std::string& mode);              // T
    std::string cmd_watchpoint(uint32_t addr = 0xFFFFFFFF);     // W
    std::string cmd_history();                                   // Z
    std::string cmd_uart_divisor(uint32_t divisor);             // +
    std::string cmd_loadmemory(uint32_t start, uint32_t end);   // L
    std::string cmd_flagwatch(const std::string& flag);          // E
    std::string cmd_interrupts(const std::string& cmd);          // I
    std::string cmd_cpu_memory();                                // @

    // Helper: format address for 28-bit display
    std::string formatAddr(uint32_t addr);

    // Helper: parse hex/decimal/binary address
    bool parseAddress(const std::string& str, uint32_t& addr);

    // Helper: read last PC from debug context
    uint32_t getLastPC();

    // Helper: send raw bytes to client with error checking
    bool sendToClient(int socket, const std::string& data);

    // Last memory dump address (for continuation)
    uint32_t m_lastMemAddr = 0x0000;
};
