#include "test_harness.h"
#include "cli_interpreter.h"
#include "plugin_command_registry.h"
#include "plugins/6502/main/cpu6502.h"
#include "plugins/6502/main/assembler_6502.h"
#include "plugins/6502/main/disassembler_6502.h"
#include "libmem/main/memory_bus.h"
#include "libdebug/main/debug_context.h"
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <fstream>

TEST_CASE(cli_basic_commands) {
    CliContext ctx;
    std::string output;
    CliInterpreter interpreter(ctx, [&](const std::string& s) { output += s; });

    // help command
    interpreter.processLine("help");
    ASSERT(output.find("Available Command Categories") != std::string::npos);
    output.clear();

    // unknown command
    interpreter.processLine("garbage_command");
    ASSERT(output.find("Unknown command") != std::string::npos);
}

TEST_CASE(cli_state_management) {
    CliContext ctx;
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    Assembler6502 assem;
    cpu.setDataBus(&bus);
    ctx.bus = &bus;
    ctx.cpu = &cpu;
    ctx.assem = &assem;

    std::string output;
    CliInterpreter interpreter(ctx, [&](const std::string& s) { output += s; });

    // Test quit
    ASSERT(ctx.quit == false);
    interpreter.processLine("quit");
    ASSERT(ctx.quit == true);

    // Test asm mode state
    ASSERT(interpreter.isAssemblyMode() == false);
    interpreter.processLine("asm $1000");
    ASSERT(interpreter.isAssemblyMode() == true);
    ASSERT(interpreter.getAsmAddr() == 0x1000);

    // Assembly line (NOP is $EA)
    interpreter.processLine("NOP");
    ASSERT(bus.read8(0x1000) == 0xEA);
    ASSERT(interpreter.getAsmAddr() == 0x1001);

    // Exit asm mode
    interpreter.processLine(".");
    ASSERT(interpreter.isAssemblyMode() == false);
}

TEST_CASE(cli_error_handling) {
    CliContext ctx;
    std::string output;
    CliInterpreter interpreter(ctx, [&](const std::string& s) { output += s; });

    // Try commands that require a machine without having one
    interpreter.processLine("step");
    ASSERT(output.find("No machine created") != std::string::npos);
    output.clear();

    interpreter.processLine("m 1000");
    ASSERT(output.find("No machine created") != std::string::npos);
    output.clear();

    interpreter.processLine("regs");
    ASSERT(output.find("No machine created") != std::string::npos);
}

TEST_CASE(cli_registers_and_memory) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    Assembler6502 assem;
    Disassembler6502 disasm;
    cpu.setDataBus(&bus);
    DebugContext dbg(&cpu, &bus);

    CliContext ctx;
    ctx.bus = &bus; ctx.cpu = &cpu; ctx.assem = &assem; ctx.disasm = &disasm; ctx.dbg = &dbg;

    std::string output;
    CliInterpreter interp(ctx, [&](const std::string& s) { output += s; });

    // regs
    cpu.setPc(0x1234);
    interp.processLine("regs");
    ASSERT(output.find("PC") != std::string::npos);
    ASSERT(output.find("1234") != std::string::npos);
    output.clear();

    // setpc
    interp.processLine("setpc $2000");
    ASSERT_EQ(cpu.pc(), (uint16_t)0x2000);
    output.clear();

    // memory dump
    bus.write8(0x100, 0xAB);
    interp.processLine("m $100 16");
    ASSERT(output.find("AB") != std::string::npos);
    output.clear();

    // fill
    interp.processLine("f $200 $FF 4");
    for (int i = 0; i < 4; i++) ASSERT_EQ(bus.read8(0x200 + i), (uint8_t)0xFF);
    output.clear();

    // copy
    interp.processLine("copy $200 $300 4");
    for (int i = 0; i < 4; i++) ASSERT_EQ(bus.read8(0x300 + i), (uint8_t)0xFF);
    output.clear();

    // swap
    bus.write8(0x400, 0x11);
    bus.write8(0x500, 0x22);
    interp.processLine("swap $400 $500 1");
    ASSERT_EQ(bus.read8(0x400), (uint8_t)0x22);
    ASSERT_EQ(bus.read8(0x500), (uint8_t)0x11);
}

TEST_CASE(cli_step_and_disasm) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    Assembler6502 assem;
    Disassembler6502 disasm;
    cpu.setDataBus(&bus);
    DebugContext dbg(&cpu, &bus);

    CliContext ctx;
    ctx.bus = &bus; ctx.cpu = &cpu; ctx.assem = &assem; ctx.disasm = &disasm; ctx.dbg = &dbg;

    std::string output;
    CliInterpreter interp(ctx, [&](const std::string& s) { output += s; });

    // Place NOP NOP BRK at $0200
    bus.write8(0x0200, 0xEA); // NOP
    bus.write8(0x0201, 0xEA); // NOP
    bus.write8(0x0202, 0x00); // BRK
    cpu.setPc(0x0200);

    interp.processLine("step");
    ASSERT_EQ(cpu.pc(), (uint16_t)0x0201);
    output.clear();

    interp.processLine("step 1");
    ASSERT_EQ(cpu.pc(), (uint16_t)0x0202);
    output.clear();

    // disasm
    interp.processLine("disasm $0200 3");
    ASSERT(output.find("NOP") != std::string::npos);
    ASSERT(output.find("BRK") != std::string::npos);
}

TEST_CASE(cli_breakpoints) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    Assembler6502 assem;
    Disassembler6502 disasm;
    cpu.setDataBus(&bus);
    DebugContext dbg(&cpu, &bus);

    CliContext ctx;
    ctx.bus = &bus; ctx.cpu = &cpu; ctx.assem = &assem; ctx.disasm = &disasm; ctx.dbg = &dbg;

    std::string output;
    CliInterpreter interp(ctx, [&](const std::string& s) { output += s; });

    interp.processLine("break $1000");
    ASSERT(output.find("Breakpoint") != std::string::npos);
    output.clear();

    interp.processLine("watch write $D020");
    ASSERT(output.find("Watchpoint") != std::string::npos);
    output.clear();

    interp.processLine("watch read $D021");
    ASSERT(output.find("Watchpoint") != std::string::npos);
    output.clear();

    interp.processLine("info breaks");
    ASSERT(output.find("exec") != std::string::npos);
    ASSERT(output.find("write") != std::string::npos);
    ASSERT(output.find("read") != std::string::npos);
    output.clear();

    interp.processLine("disable 1");
    ASSERT(output.find("Disabled") != std::string::npos);
    output.clear();

    interp.processLine("enable 1");
    ASSERT(output.find("Enabled") != std::string::npos);
    output.clear();

    interp.processLine("delete 1");
    ASSERT(output.find("Deleted") != std::string::npos);
}

TEST_CASE(cli_symbols) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    cpu.setDataBus(&bus);
    DebugContext dbg(&cpu, &bus);

    CliContext ctx;
    ctx.bus = &bus; ctx.cpu = &cpu; ctx.dbg = &dbg;

    std::string output;
    CliInterpreter interp(ctx, [&](const std::string& s) { output += s; });

    interp.processLine("sym add start $1000");
    ASSERT(output.find("Symbol added") != std::string::npos);
    output.clear();

    interp.processLine("sym add loop $1005");
    output.clear();

    interp.processLine("sym list");
    ASSERT(output.find("start") != std::string::npos);
    ASSERT(output.find("loop") != std::string::npos);
    output.clear();

    interp.processLine("sym search start");
    ASSERT(output.find("start") != std::string::npos);
    output.clear();

    interp.processLine("sym search nonexistent");
    ASSERT(output.find("No symbols matching") != std::string::npos);
    output.clear();

    interp.processLine("sym del start");
    ASSERT(output.find("removed") != std::string::npos);
    output.clear();

    interp.processLine("sym clear");
    ASSERT(output.find("cleared") != std::string::npos);
    output.clear();

    // Subcommand usage
    interp.processLine("sym");
    ASSERT(output.find("Usage") != std::string::npos);
    output.clear();

    interp.processLine("sym unknown");
    ASSERT(output.find("Unknown") != std::string::npos);
}

TEST_CASE(cli_search) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    cpu.setDataBus(&bus);
    DebugContext dbg(&cpu, &bus);

    CliContext ctx;
    ctx.bus = &bus; ctx.cpu = &cpu; ctx.dbg = &dbg;

    std::string output;
    CliInterpreter interp(ctx, [&](const std::string& s) { output += s; });

    // Place a known pattern
    bus.write8(0x500, 0xDE);
    bus.write8(0x501, 0xAD);

    interp.processLine("search DE AD");
    ASSERT(output.find("Found at") != std::string::npos);
    output.clear();

    interp.processLine("findnext");
    output.clear();

    interp.processLine("findprior");
    output.clear();

    // ASCII search
    bus.write8(0x600, 'H');
    bus.write8(0x601, 'I');
    interp.processLine("searcha HI");
    ASSERT(output.find("Found") != std::string::npos);
    output.clear();

    // No previous search
    CliContext ctx2;
    ctx2.bus = &bus; ctx2.cpu = &cpu; ctx2.dbg = &dbg;
    std::string out2;
    CliInterpreter interp2(ctx2, [&](const std::string& s) { out2 += s; });
    interp2.processLine("findnext");
    ASSERT(out2.find("No previous search") != std::string::npos);
}

TEST_CASE(cli_stack_command) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    cpu.setDataBus(&bus);
    DebugContext dbg(&cpu, &bus);

    CliContext ctx;
    ctx.bus = &bus; ctx.cpu = &cpu; ctx.dbg = &dbg;

    std::string output;
    CliInterpreter interp(ctx, [&](const std::string& s) { output += s; });

    // Empty stack
    interp.processLine("stack");
    ASSERT(output.find("empty") != std::string::npos);
    output.clear();

    // Push some entries
    dbg.stackTrace().push(StackPushType::CALL, 0x1000, 0x2000);
    dbg.stackTrace().push(StackPushType::PHA, 0x2000, 0x42);
    interp.processLine("stack");
    ASSERT(output.find("CALL") != std::string::npos);
    ASSERT(output.find("PHA") != std::string::npos);
}

TEST_CASE(cli_save_memory) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    cpu.setDataBus(&bus);
    DebugContext dbg(&cpu, &bus);

    CliContext ctx;
    ctx.bus = &bus; ctx.cpu = &cpu; ctx.dbg = &dbg;

    std::string output;
    CliInterpreter interp(ctx, [&](const std::string& s) { output += s; });

    bus.write8(0x100, 0xAA);
    bus.write8(0x101, 0xBB);

    std::string path = "/tmp/mmemu_test_cli_save.bin";
    interp.processLine("save " + path + " $100 2");
    ASSERT(output.find("Saved") != std::string::npos);

    // Verify
    std::ifstream f(path, std::ios::binary);
    ASSERT(f.good());
    uint8_t b[2];
    f.read((char*)b, 2);
    ASSERT_EQ(b[0], (uint8_t)0xAA);
    ASSERT_EQ(b[1], (uint8_t)0xBB);
    std::filesystem::remove(path);
}

TEST_CASE(cli_config_assembler) {
    CliContext ctx;
    std::string output;
    CliInterpreter interp(ctx, [&](const std::string& s) { output += s; });

    // Without machine — show current (uses default)
    interp.processLine("config assembler");
    ASSERT(output.find("Current assembler") != std::string::npos);
    output.clear();

    // Set override (doesn't require machine)
    interp.processLine("config assembler myasm");
    ASSERT(output.find("override set") != std::string::npos);
    ASSERT(ctx.assemblerOverride == "myasm");
    output.clear();

    // Unknown config
    interp.processLine("config unknown");
    ASSERT(output.find("Unknown config") != std::string::npos);
    output.clear();

    // Usage
    interp.processLine("config");
    ASSERT(output.find("Usage") != std::string::npos);
}

TEST_CASE(cli_inline_assemble_execute) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    Assembler6502 assem;
    cpu.setDataBus(&bus);
    DebugContext dbg(&cpu, &bus);

    CliContext ctx;
    ctx.bus = &bus; ctx.cpu = &cpu; ctx.assem = &assem; ctx.dbg = &dbg;

    std::string output;
    CliInterpreter interp(ctx, [&](const std::string& s) { output += s; });

    cpu.setPc(0x1000);
    // Execute LDA #$42 via inline assemble
    interp.processLine(".LDA #$42");
    ASSERT_EQ(cpu.regRead(0), (uint32_t)0x42); // A = $42
    // PC should be restored to original
    ASSERT_EQ(cpu.pc(), (uint16_t)0x1000);
}

TEST_CASE(cli_error_no_machine_commands) {
    CliContext ctx;
    std::string output;
    CliInterpreter interp(ctx, [&](const std::string& s) { output += s; });

    // All commands requiring a machine should print an error
    for (const char* cmd : {"disasm", "break $1000", "delete 1", "enable 1",
                            "disable 1", "watch read $100", "info breaks",
                            "stack", "sym list", "setpc $100", "search FF",
                            "searcha A", "findnext", "findprior", "f $0 0",
                            "copy $0 $1 1", "swap $0 $1 1", "reset",
                            "load test.prg", "cart test.crt", "eject",
                            "save /tmp/x $0 1", "screenshot x.png",
                            "tape mount x", "disk mount 8 x", "run",
                            "type hello", "key a down"}) {
        output.clear();
        interp.processLine(cmd);
        ASSERT(output.find("No machine") != std::string::npos ||
               output.find("No bus") != std::string::npos ||
               output.find("No machine with keyboard") != std::string::npos);
    }
}

// Plugin Registry Tests
static int test_cmd_called = 0;
static int test_cmd_execute(int argc, const char* const* argv, void* ctx) {
    test_cmd_called++;
    return 0;
}

TEST_CASE(plugin_registry_registration) {
    PluginCommandRegistry& reg = PluginCommandRegistry::instance();
    
    PluginCommandInfo info;
    info.name = "cli_test_cmd";
    info.usage = "test usage";
    info.execute = test_cmd_execute;
    info.ctx = nullptr;

    // Register
    bool ok = reg.registerCommand(&info);
    ASSERT(ok == true);

    // Duplicate registration should fail
    ok = reg.registerCommand(&info);
    ASSERT(ok == false);

    // Built-in collision
    reg.registerBuiltIn("builtincmd");
    info.name = "builtincmd";
    ok = reg.registerCommand(&info);
    ASSERT(ok == false);
}

TEST_CASE(plugin_registry_dispatch) {
    PluginCommandRegistry& reg = PluginCommandRegistry::instance();
    
    test_cmd_called = 0;
    std::vector<std::string> tokens = {"cli_test_cmd", "arg1"};
    
    bool handled = reg.dispatch(tokens);
    ASSERT(handled == true);
    ASSERT(test_cmd_called == 1);

    tokens = {"unknown_plugin_cmd"};
    handled = reg.dispatch(tokens);
    ASSERT(handled == false);
}
