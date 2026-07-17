#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

/**
 * Hardware Test Bridge - Cross-validation between emulator and real MEGA65
 *
 * Provides a unified interface to communicate with MEGA65 via serial monitor protocol.
 * Supports both:
 * - Emulator mode: TCP connection to SerialMonitorServer
 * - Hardware mode: Direct serial port connection (USB-UART adapter)
 *
 * Uses MEGA65's serial monitor text protocol to load code, run tests, and retrieve results.
 */
class HardwareTestBridge {
public:
    enum class Mode {
        EMULATOR,  // TCP connection to SerialMonitorServer
        HARDWARE   // Direct serial port
    };

    struct TestResult {
        bool success;
        std::string error;
        uint64_t executionCycles;
        std::vector<uint8_t> memorySnapshot;  // Dump of result region
        std::string output;                    // Serial log output
    };

    /**
     * Connect to emulator via TCP
     * @param host TCP host (default "127.0.0.1")
     * @param port TCP port (default 6502)
     * @return true if connection successful
     */
    static std::unique_ptr<HardwareTestBridge> connectEmulator(
        const std::string& host = "127.0.0.1",
        uint16_t port = 6502
    );

    /**
     * Connect to hardware via serial port
     * @param portPath Serial port path (e.g., "/dev/ttyUSB0")
     * @param baudRate Baud rate (default 2000000)
     * @return true if connection successful
     */
    static std::unique_ptr<HardwareTestBridge> connectHardware(
        const std::string& portPath,
        uint32_t baudRate = 2000000
    );

    virtual ~HardwareTestBridge();

    /**
     * Get connection mode
     */
    Mode getMode() const { return m_mode; }

    /**
     * Check if connection is active
     */
    bool isConnected() const { return m_connected; }

    /**
     * Load binary data into memory at specified address
     * @param addr Start address
     * @param data Binary data to load
     * @return true if successful
     */
    bool loadMemory(uint32_t addr, const std::vector<uint8_t>& data);

    /**
     * Load binary file into memory
     * @param addr Start address
     * @param filePath Path to binary file
     * @return true if successful
     */
    bool loadMemoryFile(uint32_t addr, const std::string& filePath);

    /**
     * Read memory range
     * @param addr Start address
     * @param size Number of bytes to read
     * @return Memory contents
     */
    std::vector<uint8_t> readMemory(uint32_t addr, uint32_t size);

    /**
     * Write single byte to memory
     * @param addr Address
     * @param value Byte value
     * @return true if successful
     */
    bool writeMemory(uint32_t addr, uint8_t value);

    /**
     * Set program counter
     * @param addr Program counter value
     * @return true if successful
     */
    bool setPC(uint32_t addr);

    /**
     * Read current CPU register value
     * @param regName Register name (A, X, Y, PC, SP, SR)
     * @return Register value
     */
    uint32_t readRegister(const std::string& regName);

    /**
     * Step CPU by N instructions
     * @param count Number of instructions to step
     * @return true if successful
     */
    bool step(int count = 1);

    /**
     * Run until breakpoint or halt
     * @return true if completed successfully
     */
    bool run();

    /**
     * Run a test program
     * @param programAddr Address where test program is loaded
     * @param resultAddr Address where test results are written
     * @param resultSize Size of result region to capture
     * @param timeoutMs Maximum time to wait (0 = no timeout)
     * @return Test result with memory snapshot and output
     */
    TestResult runTest(
        uint32_t programAddr,
        uint32_t resultAddr,
        uint32_t resultSize,
        uint32_t timeoutMs = 5000
    );

    /**
     * Get last serial output from device
     */
    std::string getSerialOutput() const { return m_serialOutput; }

    /**
     * Clear serial output buffer
     */
    void clearSerialOutput() { m_serialOutput.clear(); }

    /**
     * Get cycle count (emulator only, 0 for hardware)
     */
    uint64_t getCycles() const { return m_cycles; }

protected:
    HardwareTestBridge(Mode mode);

    Mode m_mode;
    bool m_connected = false;

    // Serial output accumulator
    std::string m_serialOutput;
    uint64_t m_cycles = 0;

    /**
     * Send command and get response (to be implemented by subclasses)
     */
    virtual std::string sendCommand(const std::string& cmd) = 0;

    /**
     * Parse hex value from response
     */
    static uint32_t parseHexValue(const std::string& response);

    /**
     * Parse register value from R command response
     */
    static uint32_t parseRegisterValue(const std::string& response, const std::string& regName);
};

/**
 * Emulator bridge - TCP connection to SerialMonitorServer
 */
class EmulatorTestBridge : public HardwareTestBridge {
public:
    EmulatorTestBridge(const std::string& host, uint16_t port);
    ~EmulatorTestBridge() override;

    bool connect();

private:
    std::string m_host;
    uint16_t m_port;
    int m_socket = -1;

    std::string sendCommand(const std::string& cmd) override;

    bool sendRaw(const std::string& data);
    std::string receiveResponse();
};

/**
 * Hardware bridge - Direct serial port connection
 */
class HardwarePortBridge : public HardwareTestBridge {
public:
    HardwarePortBridge(const std::string& portPath, uint32_t baudRate);
    ~HardwarePortBridge() override;

    bool connect();

private:
    std::string m_portPath;
    uint32_t m_baudRate;
    int m_serialFd = -1;

    std::string sendCommand(const std::string& cmd) override;

    bool openPort();
    void closePort();
    bool sendRaw(const std::string& data);
    std::string receiveResponse();
};
