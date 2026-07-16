#pragma once

#include <string>
#include <map>
#include <cstdint>

/**
 * VICE Monitor Protocol Support
 *
 * Implements the VICE remote monitor protocol for compatibility with
 * VICE-based debugging tools and IDEs (like C64IDE).
 *
 * Protocol: Text-based TCP protocol on configurable port (default 6510)
 * Reference: https://sourceforge.net/projects/vice-emu/
 *
 * This allows mmemu to act as a drop-in replacement for VICE in tools
 * that use the VICE monitor protocol.
 */

class ICore;
class IBus;
class DebugContext;

/**
 * VICE Monitor Protocol Implementation
 *
 * Supports:
 * - Register read/write
 * - Memory read/write
 * - Breakpoint management
 * - Step/continue execution
 * - Disassembly
 * - Checksum calculation
 * - Version/help queries
 */
class ViceMonitorProtocol {
public:
    ViceMonitorProtocol(ICore* cpu, IBus* bus, DebugContext* dbg);

    /**
     * Parse and execute a VICE monitor command.
     *
     * @param cmd Command string (without newline)
     * @return Response string (may be multi-line)
     */
    std::string executeCommand(const std::string& cmd);

    /**
     * Get protocol version string.
     */
    static std::string getVersion();

private:
    ICore* m_cpu;
    IBus* m_bus;
    DebugContext* m_dbg;

    // Command implementations
    std::string cmd_registers(const std::string& args);        // reg [name] [value]
    std::string cmd_memory(const std::string& args);            // mem [start] [end]
    std::string cmd_setmem(const std::string& args);            // setmem addr value
    std::string cmd_disassemble(const std::string& args);       // disasm [start] [num]
    std::string cmd_breakpoint(const std::string& args);        // break [addr]
    std::string cmd_delete_breakpoint(const std::string& args); // delete [num]
    std::string cmd_continue(const std::string& args);          // continue
    std::string cmd_step(const std::string& args);              // step [num]
    std::string cmd_stepover(const std::string& args);          // next [num]
    std::string cmd_checkpoint(const std::string& args);        // checkpoint
    std::string cmd_checksum(const std::string& args);          // checksum start end
    std::string cmd_help(const std::string& args);              // help [topic]
    std::string cmd_version(const std::string& args);           // version
    std::string cmd_drive(const std::string& args);             // drive [num]
    std::string cmd_file(const std::string& args);              // file [filename]
    std::string cmd_load(const std::string& args);              // load [filename]
    std::string cmd_dump(const std::string& args);              // dump [filename]
    std::string cmd_backtrace(const std::string& args);         // backtrace
    std::string cmd_exit(const std::string& args);              // exit
    std::string cmd_quit(const std::string& args);              // quit

    // Helper functions
    bool parseAddress(const std::string& str, uint32_t& addr);
    std::string formatHex(uint32_t value, int width = 2);
    std::string formatRegisterValue(uint8_t value);
    std::string hexDump(const uint8_t* data, size_t len, uint32_t addr = 0);
};
