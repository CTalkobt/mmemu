#pragma once
#include <cstdint>
#include <string>
#include <atomic>
#include <thread>
#include <vector>

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
 *   ? g G p P m M s c Z0 z0 qSupported qAttached qXfer:features:read D k
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

    /** One register of the `g` packet: its ICore index (-1: none, sent as zeros) and size in bytes. */
    struct GdbReg {
        std::string name;
        int coreIdx;
        int bytes;
    };
    /**
     * The `g` packet's registers for the current CPU, little-endian each:
     *  - the 6502 family (6502, 65C02, 45GS02): A, X, Y, SP, PC (16-bit), P, as GDB's 6502;
     *  - the SM83: AF, BC, DE, HL, SP, PC (16-bit each), the start of GDB's Z80 order;
     *  - any other CPU: its registers in the core's order, less the internal ones.
     */
    std::vector<GdbReg> regLayout() const;
    /** The layout as a GDB target description (qXfer:features:read:target.xml), for non-6502 CPUs. */
    std::string targetXml() const;
    /** The 6502 family's: `c` also stops on BRK ($00), which is NOP or another instruction elsewhere. */
    bool is6502Family() const;
    int findCoreReg(const char* name) const;
    std::string handleReadReg(const std::string& params);
    std::string handleWriteReg(const std::string& params);
};
