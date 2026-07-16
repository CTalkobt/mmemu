#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace mmemu {

// Exception classes
class SerialMonitorException : public std::runtime_error {
public:
    explicit SerialMonitorException(const std::string& message)
        : std::runtime_error(message) {}
};

class ConnectionError : public SerialMonitorException {
public:
    explicit ConnectionError(const std::string& message)
        : SerialMonitorException(message) {}
};

class ProtocolError : public SerialMonitorException {
public:
    explicit ProtocolError(const std::string& message)
        : SerialMonitorException(message) {}
};

// CPU register representation
struct Register {
    std::string name;
    uint32_t value;
    int width;  // bits

    Register(const std::string& n, uint32_t v, int w = 8)
        : name(n), value(v), width(w) {}

    std::string toString() const;
};

// CPU flags
class CPUFlags {
public:
    explicit CPUFlags(uint8_t p_value) : m_pValue(p_value) {}

    bool getNegative() const { return (m_pValue & 0x80) != 0; }
    bool getOverflow() const { return (m_pValue & 0x40) != 0; }
    bool getBreak() const { return (m_pValue & 0x10) != 0; }
    bool getDecimal() const { return (m_pValue & 0x08) != 0; }
    bool getInterrupt() const { return (m_pValue & 0x04) != 0; }
    bool getZero() const { return (m_pValue & 0x02) != 0; }
    bool getCarry() const { return (m_pValue & 0x01) != 0; }

    bool getFlag(const std::string& flag) const;
    std::string toString() const;

private:
    uint8_t m_pValue;
};

// Disassembled instruction
struct Instruction {
    uint32_t addr;
    std::string mnemonic;
    std::string operands;

    Instruction() : addr(0) {}
    Instruction(uint32_t a, const std::string& m, const std::string& o = "")
        : addr(a), mnemonic(m), operands(o) {}

    std::string toString() const;
};

// Serial Monitor client
class SerialMonitor {
public:
    SerialMonitor(const std::string& host = "localhost", int port = 2000, double timeout = 5.0);
    ~SerialMonitor();

    // Connection management
    void connect();
    void disconnect();
    bool isConnected() const;

    // CPU control
    std::map<std::string, uint32_t> readRegisters();
    void setPc(uint32_t addr);
    void enableInterrupts();
    void disableInterrupts();
    bool getInterruptStatus();

    // Memory operations
    std::vector<uint8_t> readMemory(uint32_t addr = 0, int length = 256);
    void writeMemory(uint32_t addr, uint8_t value);
    void writeMemoryBlock(uint32_t addr, const std::vector<uint8_t>& data);

    // Debugging
    std::vector<Instruction> disassemble(uint32_t addr = 0, int count = 16);
    void setBreakpoint(uint32_t addr);
    void clearBreakpoints();
    bool getFlag(const std::string& flag);

    // Tracing
    void enableTrace();
    void disableTrace();
    std::string getTraceDump();
    std::string getCpuHistory();
    std::string getCpuView();

    // Help
    std::string help();

private:
    std::string m_host;
    int m_port;
    double m_timeout;
    int m_socket;
    bool m_connected;
    uint32_t m_lastMemAddr;

    std::string sendCommand(const std::string& cmd);
    void closeSocket();
};

} // namespace mmemu
