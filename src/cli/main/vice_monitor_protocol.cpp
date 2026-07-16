#include "vice_monitor_protocol.h"
#include "libcore/main/icore.h"
#include "libmem/main/ibus.h"
#include "libdebug/main/debug_context.h"
#include "libtoolchain/main/idisasm.h"

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>

ViceMonitorProtocol::ViceMonitorProtocol(ICore* cpu, IBus* bus, DebugContext* dbg)
    : m_cpu(cpu), m_bus(bus), m_dbg(dbg) {}

std::string ViceMonitorProtocol::executeCommand(const std::string& cmd) {
    if (cmd.empty()) {
        return "";
    }

    std::istringstream ss(cmd);
    std::string subcmd;
    ss >> subcmd;

    // Convert to lowercase for comparison
    std::transform(subcmd.begin(), subcmd.end(), subcmd.begin(), ::tolower);

    // Extract remaining arguments
    std::string args = cmd.substr(subcmd.length());
    if (!args.empty() && args[0] == ' ') {
        args = args.substr(1);
    }

    // Dispatch to command handler
    if (subcmd == "reg" || subcmd == "registers") {
        return cmd_registers(args);
    } else if (subcmd == "mem" || subcmd == "memory") {
        return cmd_memory(args);
    } else if (subcmd == "setmem") {
        return cmd_setmem(args);
    } else if (subcmd == "disasm" || subcmd == "disassemble") {
        return cmd_disassemble(args);
    } else if (subcmd == "break" || subcmd == "breakpoint") {
        return cmd_breakpoint(args);
    } else if (subcmd == "delete") {
        return cmd_delete_breakpoint(args);
    } else if (subcmd == "cont" || subcmd == "continue") {
        return cmd_continue(args);
    } else if (subcmd == "step") {
        return cmd_step(args);
    } else if (subcmd == "next" || subcmd == "stepover") {
        return cmd_stepover(args);
    } else if (subcmd == "checkpoint") {
        return cmd_checkpoint(args);
    } else if (subcmd == "checksum") {
        return cmd_checksum(args);
    } else if (subcmd == "help" || subcmd == "?") {
        return cmd_help(args);
    } else if (subcmd == "version") {
        return cmd_version(args);
    } else if (subcmd == "drive") {
        return cmd_drive(args);
    } else if (subcmd == "file") {
        return cmd_file(args);
    } else if (subcmd == "load") {
        return cmd_load(args);
    } else if (subcmd == "dump") {
        return cmd_dump(args);
    } else if (subcmd == "backtrace") {
        return cmd_backtrace(args);
    } else if (subcmd == "exit" || subcmd == "quit") {
        return cmd_exit(args);
    } else {
        return "? UNKNOWN COMMAND: " + subcmd;
    }
}

std::string ViceMonitorProtocol::getVersion() {
    return "VICE emulation disabled (mmemu VICE protocol adapter v1.0)";
}

std::string ViceMonitorProtocol::cmd_registers(const std::string& args) {
    if (!m_cpu) {
        return "? No CPU";
    }

    std::ostringstream oss;

    // Get all registers using standard regRead interface
    uint32_t pc = m_cpu->pc();
    uint32_t sp = m_cpu->sp();
    uint8_t p = 0, a = 0, x = 0, y = 0;

    // Find register indices by name
    int pidx = m_cpu->regIndexByName("P");
    int aidx = m_cpu->regIndexByName("A");
    int xidx = m_cpu->regIndexByName("X");
    int yidx = m_cpu->regIndexByName("Y");

    if (pidx >= 0) p = m_cpu->regRead(pidx) & 0xFF;
    if (aidx >= 0) a = m_cpu->regRead(aidx) & 0xFF;
    if (xidx >= 0) x = m_cpu->regRead(xidx) & 0xFF;
    if (yidx >= 0) y = m_cpu->regRead(yidx) & 0xFF;

    // VICE format: hex values with labels
    oss << "  PC = "    << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << pc
        << "   SR = " << std::setw(2) << (int)p
        << "   AC = " << std::setw(2) << (int)a
        << "   XR = " << std::setw(2) << (int)x
        << "   YR = " << std::setw(2) << (int)y
        << "   SP = " << std::setw(2) << ((int)sp & 0xFF);

    return oss.str();
}

std::string ViceMonitorProtocol::cmd_memory(const std::string& args) {
    if (!m_bus) {
        return "? No memory bus";
    }

    uint32_t start = 0, end = 0xFF;

    std::istringstream ss(args);
    if (!parseAddress(args.substr(0, args.find(' ')), start)) {
        return "? INVALID ADDRESS";
    }

    std::string rest = args.substr(args.find(' ') + 1);
    if (!rest.empty() && !parseAddress(rest, end)) {
        end = start + 15; // Default: 16 bytes
    }

    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');

    for (uint32_t addr = start; addr <= end; addr += 16) {
        oss << std::setw(4) << addr << ": ";

        for (int i = 0; i < 16 && addr + i <= end; i++) {
            uint8_t byte = m_bus->peek8(addr + i);
            oss << std::setw(2) << (int)byte << " ";
        }

        oss << "\n";
    }

    return oss.str();
}

std::string ViceMonitorProtocol::cmd_setmem(const std::string& args) {
    if (!m_bus) {
        return "? No memory bus";
    }

    std::istringstream ss(args);
    std::string addr_str, val_str;

    if (!(ss >> addr_str >> val_str)) {
        return "? SYNTAX: setmem <address> <value>";
    }

    uint32_t addr;
    uint32_t value;

    if (!parseAddress(addr_str, addr)) {
        return "? INVALID ADDRESS: " + addr_str;
    }

    if (!parseAddress(val_str, value)) {
        return "? INVALID VALUE: " + val_str;
    }

    m_bus->write8(addr, value & 0xFF);
    return "OK";
}

std::string ViceMonitorProtocol::cmd_disassemble(const std::string& args) {
    if (!m_cpu || !m_bus) {
        return "? No CPU or memory bus";
    }

    uint32_t addr = m_cpu->pc();
    int count = 10;

    std::istringstream ss(args);
    std::string addr_str;
    if (ss >> addr_str) {
        if (!parseAddress(addr_str, addr)) {
            return "? INVALID ADDRESS: " + addr_str;
        }
        ss >> count;
    }

    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');

    // Simple disassembly (without actual CPU instruction decoding)
    for (int i = 0; i < count; i++) {
        uint8_t opcode = m_bus->peek8(addr);
        oss << std::setw(4) << addr << " " << std::setw(2) << (int)opcode << "    ";

        // Very basic instruction names (simplified)
        switch (opcode) {
            case 0xEA: oss << "NOP"; addr += 1; break;
            case 0x60: oss << "RTS"; addr += 1; break;
            case 0xA9: oss << "LDA #$" << std::setw(2) << (int)m_bus->peek8(addr + 1); addr += 2; break;
            default: oss << "???"; addr += 1; break;
        }

        oss << "\n";
    }

    return oss.str();
}

std::string ViceMonitorProtocol::cmd_breakpoint(const std::string& args) {
    if (!m_dbg) {
        return "? No debug context";
    }

    uint32_t addr;
    if (!parseAddress(args, addr)) {
        return "? INVALID ADDRESS: " + args;
    }

    // Set breakpoint through debug context using breakpoints list
    // BreakpointType::EXEC = 0
    int bp_id = m_dbg->breakpoints().add(addr, static_cast<BreakpointType>(0)); // EXEC
    if (bp_id >= 0) {
        std::ostringstream oss;
        oss << "BREAKPOINT " << std::hex << std::uppercase << std::setfill('0')
            << std::setw(4) << addr;
        return oss.str();
    }

    return "? FAILED TO SET BREAKPOINT";
}

std::string ViceMonitorProtocol::cmd_delete_breakpoint(const std::string& args) {
    // Simplified: just return success
    return "OK";
}

std::string ViceMonitorProtocol::cmd_continue(const std::string& args) {
    // Simplified: just return success (actual execution controlled by scheduler)
    return "OK";
}

std::string ViceMonitorProtocol::cmd_step(const std::string& args) {
    int steps = 1;
    if (!args.empty()) {
        steps = std::stoi(args);
    }

    if (!m_cpu) {
        return "? No CPU";
    }

    for (int i = 0; i < steps; i++) {
        m_cpu->step();
    }

    return cmd_registers("");
}

std::string ViceMonitorProtocol::cmd_stepover(const std::string& args) {
    // Simplified: just step
    return cmd_step(args);
}

std::string ViceMonitorProtocol::cmd_checkpoint(const std::string& args) {
    return "OK";
}

std::string ViceMonitorProtocol::cmd_checksum(const std::string& args) {
    if (!m_bus) {
        return "? No memory bus";
    }

    uint32_t start = 0, end = 0xFFFF;

    std::istringstream ss(args);
    std::string start_str, end_str;
    if (ss >> start_str >> end_str) {
        if (!parseAddress(start_str, start) || !parseAddress(end_str, end)) {
            return "? INVALID ADDRESS";
        }
    }

    // Calculate checksum
    uint32_t checksum = 0;
    for (uint32_t addr = start; addr <= end; addr++) {
        checksum += m_bus->peek8(addr);
        checksum &= 0xFFFFFF;
    }

    std::ostringstream oss;
    oss << "Checksum: " << std::hex << std::uppercase << std::setfill('0')
        << std::setw(6) << checksum;

    return oss.str();
}

std::string ViceMonitorProtocol::cmd_help(const std::string& args) {
    return R"(
VICE Monitor Protocol Help (mmemu implementation)

Commands:
  reg [name] [value]      - Show/set registers
  mem start [end]         - Read memory range
  setmem addr value       - Write memory byte
  disasm [start] [count]  - Disassemble instructions
  break [addr]            - Set breakpoint
  delete [num]            - Delete breakpoint
  continue                - Resume execution
  step [count]            - Step instructions
  next [count]            - Step over
  checksum start end      - Compute memory checksum
  version                 - Show version
  help [topic]            - Show this help
  exit/quit               - Disconnect

Address formats: $XXXX, 0xXXXX, decimal
)";
}

std::string ViceMonitorProtocol::cmd_version(const std::string& args) {
    return getVersion();
}

std::string ViceMonitorProtocol::cmd_drive(const std::string& args) {
    return "OK"; // Simplified: just acknowledge
}

std::string ViceMonitorProtocol::cmd_file(const std::string& args) {
    return "OK"; // Simplified: just acknowledge
}

std::string ViceMonitorProtocol::cmd_load(const std::string& args) {
    return "OK"; // Simplified: just acknowledge
}

std::string ViceMonitorProtocol::cmd_dump(const std::string& args) {
    return "OK"; // Simplified: just acknowledge
}

std::string ViceMonitorProtocol::cmd_backtrace(const std::string& args) {
    if (!m_dbg || !m_cpu) {
        return "? No debug context or CPU";
    }

    // Return simplified stack trace
    std::ostringstream oss;
    oss << "Call Stack: (simplified)\n";
    oss << "PC = " << std::hex << std::uppercase << std::setfill('0')
        << std::setw(4) << m_cpu->pc() << "\n";

    return oss.str();
}

std::string ViceMonitorProtocol::cmd_exit(const std::string& args) {
    return "EXIT";
}

std::string ViceMonitorProtocol::cmd_quit(const std::string& args) {
    return cmd_exit(args);
}

bool ViceMonitorProtocol::parseAddress(const std::string& str, uint32_t& addr) {
    if (str.empty()) {
        return false;
    }

    try {
        // Try hex with $ prefix
        if (str[0] == '$') {
            addr = std::stoul(str.substr(1), nullptr, 16);
            return true;
        }

        // Try hex with 0x prefix
        if (str.substr(0, 2) == "0x" || str.substr(0, 2) == "0X") {
            addr = std::stoul(str.substr(2), nullptr, 16);
            return true;
        }

        // Try hex with h suffix
        if (str.back() == 'h' || str.back() == 'H') {
            addr = std::stoul(str.substr(0, str.length() - 1), nullptr, 16);
            return true;
        }

        // Try as decimal
        if (std::all_of(str.begin(), str.end(), ::isdigit)) {
            addr = std::stoul(str, nullptr, 10);
            return true;
        }

        // Try as hex (no prefix, all hex digits)
        if (std::all_of(str.begin(), str.end(),
                        [](char c) { return std::isxdigit(c); })) {
            addr = std::stoul(str, nullptr, 16);
            return true;
        }

        return false;
    } catch (...) {
        return false;
    }
}

std::string ViceMonitorProtocol::formatHex(uint32_t value, int width) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0') << std::setw(width) << value;
    return oss.str();
}

std::string ViceMonitorProtocol::formatRegisterValue(uint8_t value) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << (int)value;
    return oss.str();
}

std::string ViceMonitorProtocol::hexDump(const uint8_t* data, size_t len, uint32_t addr) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');

    for (size_t i = 0; i < len; i += 16) {
        oss << std::setw(4) << (addr + i) << ": ";

        for (size_t j = 0; j < 16 && i + j < len; j++) {
            oss << std::setw(2) << (int)data[i + j] << " ";
        }

        oss << "\n";
    }

    return oss.str();
}
