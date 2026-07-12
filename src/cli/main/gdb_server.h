#pragma once
#include <cstdint>
#include <string>
#include <atomic>
#include <thread>

class ICore;
class IBus;
class DebugContext;

/**
 * GDB Remote Serial Protocol server.
 *
 * Listens on a TCP port and translates GDB RSP commands to emulator
 * operations (register read/write, memory access, step, continue,
 * breakpoints). Runs in a background thread.
 *
 * Supports the minimal command set needed for basic debugging:
 *   ? g G m M s c Z0 z0 qSupported qAttached D k
 */
class GdbServer {
public:
    GdbServer(ICore* cpu, IBus* bus, DebugContext* dbg);
    ~GdbServer();

    /** Start listening on the given port. Returns false on bind failure. */
    bool start(uint16_t port);

    /** Stop the server and close all connections. */
    void stop();

    bool isRunning() const { return m_running; }

private:
    ICore* m_cpu;
    IBus* m_bus;
    DebugContext* m_dbg;

    int m_listenFd = -1;
    int m_clientFd = -1;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::thread m_thread;

    void serverLoop();
    void handleClient(int fd);

    // RSP packet I/O
    std::string recvPacket(int fd);
    void sendPacket(int fd, const std::string& data);
    void sendOk(int fd) { sendPacket(fd, "OK"); }
    void sendError(int fd, int code);
    void sendEmpty(int fd) { sendPacket(fd, ""); }

    // Command handlers
    std::string handleQuery(const std::string& pkt);
    std::string handleReadRegs();
    std::string handleWriteRegs(const std::string& data);
    std::string handleReadMem(const std::string& params);
    std::string handleWriteMem(const std::string& params);
    std::string handleStep();
    std::string handleContinue(int fd);
    std::string handleInsertBreakpoint(const std::string& params);
    std::string handleRemoveBreakpoint(const std::string& params);

    // Metadata handlers (Issue #100)
    std::string handleQuerySymbols(const std::string& params);
    std::string handleQueryVariables(const std::string& params);
    std::string handleQueryFrameInfo();

    // Helpers
    static std::string toHexByte(uint8_t v);
    static std::string toHex16LE(uint16_t v);
    static uint8_t fromHex(char hi, char lo);

    // 6502 register order for GDB: A, X, Y, SP, PC (16-bit), P
    static constexpr int GDB_REG_COUNT = 6;
    int mapGdbRegToCore(int gdbIdx) const;
};
