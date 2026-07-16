#include "../include/mmemu_serial_monitor.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/select.h>

namespace mmemu {

// Register implementation
std::string Register::toString() const {
    std::stringstream ss;
    int digits = width / 4;
    ss << name << "=" << std::hex << std::setw(digits) << std::setfill('0') << value;
    return ss.str();
}

// CPUFlags implementation
bool CPUFlags::getFlag(const std::string& flag) const {
    if (flag == "N" || flag == "n") return getNegative();
    if (flag == "V" || flag == "v") return getOverflow();
    if (flag == "B" || flag == "b") return getBreak();
    if (flag == "D" || flag == "d") return getDecimal();
    if (flag == "I" || flag == "i") return getInterrupt();
    if (flag == "Z" || flag == "z") return getZero();
    if (flag == "C" || flag == "c") return getCarry();
    throw std::invalid_argument("Unknown flag: " + flag);
}

std::string CPUFlags::toString() const {
    std::string result;
    result += getNegative() ? 'N' : '-';
    result += getOverflow() ? 'V' : '-';
    result += getBreak() ? 'B' : '-';
    result += getDecimal() ? 'D' : '-';
    result += getInterrupt() ? 'I' : '-';
    result += getZero() ? 'Z' : '-';
    result += getCarry() ? 'C' : '-';
    return result;
}

// Instruction implementation
std::string Instruction::toString() const {
    std::stringstream ss;
    ss << std::hex << std::setw(6) << std::setfill('0') << addr << " ";
    ss << mnemonic;
    if (!operands.empty()) {
        ss << " " << operands;
    }
    return ss.str();
}

// SerialMonitor implementation
SerialMonitor::SerialMonitor(const std::string& host, int port, double timeout)
    : m_host(host), m_port(port), m_timeout(timeout), m_socket(-1),
      m_connected(false), m_lastMemAddr(0) {
}

SerialMonitor::~SerialMonitor() {
    if (m_connected) {
        disconnect();
    }
}

void SerialMonitor::connect() {
    if (m_connected) {
        return;
    }

    m_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket < 0) {
        throw ConnectionError("Failed to create socket");
    }

    // Set socket timeout
    struct timeval tv;
    tv.tv_sec = (int)m_timeout;
    tv.tv_usec = (int)((m_timeout - tv.tv_sec) * 1000000);
    if (setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        closeSocket();
        throw ConnectionError("Failed to set socket timeout");
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);

    if (inet_pton(AF_INET, m_host.c_str(), &addr.sin_addr) <= 0) {
        closeSocket();
        throw ConnectionError("Invalid address: " + m_host);
    }

    if (::connect(m_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        closeSocket();
        throw ConnectionError("Failed to connect to " + m_host + ":" + std::to_string(m_port));
    }

    m_connected = true;
}

void SerialMonitor::disconnect() {
    closeSocket();
    m_connected = false;
}

bool SerialMonitor::isConnected() const {
    return m_connected;
}

void SerialMonitor::closeSocket() {
    if (m_socket >= 0) {
        close(m_socket);
        m_socket = -1;
    }
}

std::string SerialMonitor::sendCommand(const std::string& cmd) {
    if (!m_connected) {
        throw ConnectionError("Not connected");
    }

    // Send command with newline
    std::string fullCmd = cmd + "\n";
    if (send(m_socket, fullCmd.c_str(), fullCmd.length(), 0) < 0) {
        m_connected = false;
        throw ConnectionError("Failed to send command");
    }

    // Read response
    std::string response;
    char buffer[4096];
    int bytes;

    while (true) {
        bytes = recv(m_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes < 0) {
            break;  // Timeout
        }
        if (bytes == 0) {
            break;  // Connection closed
        }
        buffer[bytes] = '\0';
        response += buffer;
        if (response.find('\n') != std::string::npos) {
            break;
        }
    }

    // Trim whitespace
    response.erase(0, response.find_first_not_of(" \t\r\n"));
    response.erase(response.find_last_not_of(" \t\r\n") + 1);

    return response;
}

std::map<std::string, uint32_t> SerialMonitor::readRegisters() {
    std::string response = sendCommand("r");

    if (response.find("ERROR") != std::string::npos) {
        throw ProtocolError("Read registers failed: " + response);
    }

    std::map<std::string, uint32_t> result;

    // Parse: PC=002000 A=42 X=01 Y=02 SP=01F8 P=30
    std::istringstream iss(response);
    std::string pair;

    while (iss >> pair) {
        size_t pos = pair.find('=');
        if (pos != std::string::npos) {
            std::string name = pair.substr(0, pos);
            std::string value = pair.substr(pos + 1);

            try {
                result[name] = std::stoul(value, nullptr, 16);
            } catch (...) {
                // Ignore parse errors
            }
        }
    }

    return result;
}

void SerialMonitor::setPc(uint32_t addr) {
    std::stringstream ss;
    ss << "g " << std::hex << addr;

    std::string response = sendCommand(ss.str());

    if (response.find("ERROR") != std::string::npos ||
        response.find("OK") == std::string::npos) {
        throw ProtocolError("Set PC failed: " + response);
    }
}

void SerialMonitor::enableInterrupts() {
    std::string response = sendCommand("i enable");

    if (response.find("ERROR") != std::string::npos) {
        throw ProtocolError("Enable interrupts failed: " + response);
    }
}

void SerialMonitor::disableInterrupts() {
    std::string response = sendCommand("i disable");

    if (response.find("ERROR") != std::string::npos) {
        throw ProtocolError("Disable interrupts failed: " + response);
    }
}

bool SerialMonitor::getInterruptStatus() {
    std::string response = sendCommand("i status");

    if (response.find("ERROR") != std::string::npos) {
        throw ProtocolError("Get interrupt status failed: " + response);
    }

    return response.find("ENABLED") != std::string::npos;
}

std::vector<uint8_t> SerialMonitor::readMemory(uint32_t addr, int length) {
    std::stringstream ss;
    if (addr > 0) {
        ss << "m " << std::hex << addr;
        m_lastMemAddr = addr;
    } else {
        ss << "m";
    }

    std::string response = sendCommand(ss.str());

    if (response.find("ERROR") != std::string::npos) {
        throw ProtocolError("Read memory failed: " + response);
    }

    std::vector<uint8_t> result;

    // Parse hex bytes from memory dump
    std::istringstream iss(response);
    std::string line;

    while (std::getline(iss, line)) {
        if (line.find(':') == std::string::npos) {
            continue;
        }

        // Extract hex part: "002000: 4C 30 E5 A2..."
        size_t colonPos = line.find(':');
        if (colonPos == std::string::npos) {
            continue;
        }

        std::istringstream hexStream(line.substr(colonPos + 1));
        std::string hexByte;

        while (hexStream >> hexByte) {
            // Stop at ASCII part (after spacing)
            if (hexByte.find('|') != std::string::npos) {
                break;
            }

            try {
                result.push_back(std::stoi(hexByte, nullptr, 16));
                if (result.size() >= length) {
                    break;
                }
            } catch (...) {
                continue;
            }
        }

        if (result.size() >= length) {
            break;
        }
    }

    m_lastMemAddr = addr + result.size();
    return result;
}

void SerialMonitor::writeMemory(uint32_t addr, uint8_t value) {
    std::stringstream ss;
    ss << "s " << std::hex << addr << " " << std::hex << (int)value;

    std::string response = sendCommand(ss.str());

    if (response.find("ERROR") != std::string::npos ||
        response.find("OK") == std::string::npos) {
        throw ProtocolError("Write memory failed: " + response);
    }
}

void SerialMonitor::writeMemoryBlock(uint32_t addr, const std::vector<uint8_t>& data) {
    for (size_t i = 0; i < data.size(); ++i) {
        writeMemory(addr + i, data[i]);
    }
}

std::vector<Instruction> SerialMonitor::disassemble(uint32_t addr, int count) {
    std::stringstream ss;
    if (addr > 0) {
        ss << "d " << std::hex << addr;
    } else {
        ss << "d";
    }

    std::string response = sendCommand(ss.str());

    if (response.find("ERROR") != std::string::npos) {
        throw ProtocolError("Disassemble failed: " + response);
    }

    std::vector<Instruction> result;

    std::istringstream iss(response);
    std::string line;

    while (std::getline(iss, line)) {
        if (line.empty()) {
            continue;
        }

        std::istringstream lineStream(line);
        std::string addrStr, part1, part2;

        if (!(lineStream >> addrStr)) {
            continue;
        }

        try {
            uint32_t instructAddr = std::stoul(addrStr, nullptr, 16);

            // Try to parse mnemonic and operands
            std::string mnemonic, operands;
            if (lineStream >> part1) {
                // Check if this is a hex byte or mnemonic
                if (part1[0] >= '0' && part1[0] <= '9') {
                    // Hex bytes, skip to actual mnemonic
                    while (lineStream >> part2) {
                        // First non-hex is likely the mnemonic
                        if (!std::all_of(part2.begin(), part2.end(),
                                        [](char c) { return isxdigit(c); })) {
                            mnemonic = part2;
                            lineStream >> operands;
                            break;
                        }
                    }
                } else {
                    mnemonic = part1;
                    lineStream >> operands;
                }
            }

            if (!mnemonic.empty()) {
                result.push_back(Instruction(instructAddr, mnemonic, operands));
            }
        } catch (...) {
            continue;
        }

        if (result.size() >= count) {
            break;
        }
    }

    return result;
}

void SerialMonitor::setBreakpoint(uint32_t addr) {
    std::stringstream ss;
    ss << "b " << std::hex << addr;

    std::string response = sendCommand(ss.str());

    if (response.find("ERROR") != std::string::npos ||
        response.find("OK") == std::string::npos) {
        throw ProtocolError("Set breakpoint failed: " + response);
    }
}

void SerialMonitor::clearBreakpoints() {
    std::string response = sendCommand("b");

    if (response.find("ERROR") != std::string::npos ||
        response.find("OK") == std::string::npos) {
        throw ProtocolError("Clear breakpoints failed: " + response);
    }
}

bool SerialMonitor::getFlag(const std::string& flag) {
    std::stringstream ss;
    ss << "e " << flag;

    std::string response = sendCommand(ss.str());

    if (response.find("ERROR") != std::string::npos) {
        throw ProtocolError("Flag watch failed: " + response);
    }

    return response.find("SET") != std::string::npos;
}

void SerialMonitor::enableTrace() {
    std::string response = sendCommand("t on");

    if (response.find("ERROR") != std::string::npos) {
        throw ProtocolError("Enable trace failed: " + response);
    }
}

void SerialMonitor::disableTrace() {
    std::string response = sendCommand("t off");

    if (response.find("ERROR") != std::string::npos) {
        throw ProtocolError("Disable trace failed: " + response);
    }
}

std::string SerialMonitor::getTraceDump() {
    std::string response = sendCommand("t dump");

    if (response.find("ERROR") != std::string::npos) {
        throw ProtocolError("Get trace dump failed: " + response);
    }

    return response;
}

std::string SerialMonitor::getCpuHistory() {
    std::string response = sendCommand("z");

    if (response.find("ERROR") != std::string::npos) {
        throw ProtocolError("Get CPU history failed: " + response);
    }

    return response;
}

std::string SerialMonitor::getCpuView() {
    std::string response = sendCommand("@");

    if (response.find("ERROR") != std::string::npos) {
        throw ProtocolError("Get CPU view failed: " + response);
    }

    return response;
}

std::string SerialMonitor::help() {
    return sendCommand("?");
}

} // namespace mmemu
