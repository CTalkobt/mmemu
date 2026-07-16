#include "../include/mmemu_serial_monitor.h"
#include <iostream>
#include <iomanip>
#include <cstring>

using namespace mmemu;

void printMemoryDump(uint32_t addr, const std::vector<uint8_t>& data) {
    for (size_t offset = 0; offset < data.size(); offset += 16) {
        std::cout << std::hex << std::setw(6) << std::setfill('0') << (addr + offset) << ": ";

        // Print hex
        for (size_t i = offset; i < offset + 16 && i < data.size(); ++i) {
            std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)data[i] << " ";
        }

        std::cout << " ";

        // Print ASCII
        for (size_t i = offset; i < offset + 16 && i < data.size(); ++i) {
            char c = (char)data[i];
            std::cout << (c >= 32 && c < 127 ? c : '.');
        }

        std::cout << std::endl;
    }
}

int main(int argc, char** argv) {
    std::string host = "localhost";
    int port = 2000;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        }
    }

    try {
        SerialMonitor mm(host, port);
        mm.connect();

        std::cout << "Connected to " << host << ":" << port << std::endl << std::endl;

        // Read and display registers
        std::cout << "CPU Registers:" << std::endl;
        auto regs = mm.readRegisters();
        for (const auto& [name, value] : regs) {
            std::cout << "  " << name << " = $" << std::hex << std::setw(6) << std::setfill('0') << value << std::endl;
        }
        std::cout << std::endl;

        // Read memory at address 0x0000
        std::cout << "Memory at $0000:" << std::endl;
        auto memory = mm.readMemory(0x0000, 256);
        printMemoryDump(0x0000, memory);
        std::cout << std::endl;

        // Disassemble from address 0x0000
        std::cout << "Disassembly from $0000:" << std::endl;
        auto instrs = mm.disassemble(0x0000, 8);
        for (const auto& instr : instrs) {
            std::cout << "  " << instr.toString() << std::endl;
        }
        std::cout << std::endl;

        // Check CPU flags
        std::cout << "CPU Flags:" << std::endl;
        if (regs.count("P")) {
            CPUFlags flags(regs["P"]);
            std::cout << "  " << flags.toString() << std::endl;
            std::cout << "  Negative: " << (flags.getNegative() ? "SET" : "CLEAR") << std::endl;
            std::cout << "  Overflow: " << (flags.getOverflow() ? "SET" : "CLEAR") << std::endl;
            std::cout << "  Break: " << (flags.getBreak() ? "SET" : "CLEAR") << std::endl;
            std::cout << "  Decimal: " << (flags.getDecimal() ? "SET" : "CLEAR") << std::endl;
            std::cout << "  Interrupt: " << (flags.getInterrupt() ? "SET" : "CLEAR") << std::endl;
            std::cout << "  Zero: " << (flags.getZero() ? "SET" : "CLEAR") << std::endl;
            std::cout << "  Carry: " << (flags.getCarry() ? "SET" : "CLEAR") << std::endl;
        }
        std::cout << std::endl;

        mm.disconnect();
        std::cout << "Disconnected" << std::endl;

        return 0;

    } catch (const ConnectionError& e) {
        std::cerr << "Connection error: " << e.what() << std::endl;
        return 1;
    } catch (const ProtocolError& e) {
        std::cerr << "Protocol error: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
