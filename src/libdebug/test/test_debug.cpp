#include "test_harness.h"
#include "libdebug/main/debug_context.h"
#include "plugins/6502/main/cpu6502.h"
#include "libmem/main/memory_bus.h"

TEST_CASE(debug_breakpoints) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    cpu.setDataBus(&bus);
    
    DebugContext dbg(&cpu, &bus);
    cpu.setObserver(&dbg);
    bus.setObserver(&dbg);

    // Program: NOP ($EA); STA $10 ($85 $10)
    bus.write8(0x1000, 0xEA);
    bus.write8(0x1001, 0x85);
    bus.write8(0x1002, 0x10);
    cpu.setPc(0x1000);
    cpu.regWrite(0, 0x42); // A = $42

    dbg.breakpoints().add(0x1001, BreakpointType::EXEC);
    dbg.breakpoints().add(0x0010, BreakpointType::WRITE_WATCH);
    dbg.breakpoints().add(0x0011, BreakpointType::READ_WATCH);

    cpu.step(); // Execute NOP
    ASSERT(dbg.breakpoints().breakpoints()[0].hitCount == 0);

    cpu.step(); // EXEC bp fires at 0x1001 — STA does NOT execute (onStep returns false)
    ASSERT(dbg.breakpoints().breakpoints()[0].hitCount == 1); // EXEC bp hit
    ASSERT(dbg.breakpoints().breakpoints()[1].hitCount == 0); // STA never ran, no write

    // Test write watchpoint via direct bus write
    bus.write8(0x0010, 0x42);
    ASSERT(dbg.breakpoints().breakpoints()[1].hitCount == 1); // WRITE wp hit

    // Test read-watch
    bus.read8(0x0011);
    ASSERT(dbg.breakpoints().breakpoints()[2].hitCount == 1); // READ wp hit
}

TEST_CASE(debug_trace) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    cpu.setDataBus(&bus);
    
    DebugContext dbg(&cpu, &bus);
    cpu.setObserver(&dbg);

    bus.write8(0x1000, 0xA9); // LDA #$42
    bus.write8(0x1001, 0x42);
    cpu.setPc(0x1000);
    cpu.regWrite(0, 0x42); // pre-set A so trace captures $42 (recorded before execution)

    cpu.step();

    ASSERT(dbg.trace().size() == 1);
    ASSERT(dbg.trace().at(0).addr == 0x1000);
    ASSERT(dbg.trace().at(0).regs.at("A") == 0x42);
}

TEST_CASE(debug_snapshots) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    cpu.setDataBus(&bus);
    
    DebugContext dbg(&cpu, &bus);

    cpu.regWrite(0, 0xAA);
    bus.write8(0x2000, 0x55);
    
    int snapIdx = dbg.saveSnapshot("test");
    
    cpu.regWrite(0, 0xBB);
    bus.write8(0x2000, 0x66);
    
    dbg.restoreSnapshot(snapIdx);
    
    ASSERT(cpu.regRead(0) == 0xAA);
    ASSERT(bus.read8(0x2000) == 0x55);
}

// Trace text and registers are captured raw and formatted on read; the text
// shows the instruction that executed even after the code is overwritten.
TEST_CASE(debug_trace_lazy_formatting) {
    FlatMemoryBus bus("system", 16);
    MOS6502 cpu;
    cpu.setDataBus(&bus);
    DebugContext dbg(&cpu, &bus);
    cpu.setObserver(&dbg);
    bus.setObserver(&dbg);   // records memory writes for undo

    const uint8_t prog[] = { 0xA9, 0x42,         // LDA #$42
                             0x48,               // PHA
                             0x8D, 0x00, 0x10 }; // STA $1000 (overwrites the LDA)
    for (int i = 0; i < (int)sizeof(prog); i++) bus.write8(0x1000 + i, prog[i]);
    cpu.setPc(0x1000);
    cpu.step(); cpu.step(); cpu.step();

    ASSERT_EQ((int)dbg.trace().size(), 3);
    ASSERT_EQ((int)bus.peek8(0x1000), 0x42);                // code was overwritten
    ASSERT(dbg.trace().at(0).mnemonic.find("LDA") == 0);    // trace shows what ran
    ASSERT(dbg.trace().at(0).mnemonic.find("#$42") != std::string::npos);
    ASSERT(dbg.trace().at(1).mnemonic.find("PHA") == 0);
    ASSERT(dbg.trace().at(2).mnemonic.find("STA") == 0);
    ASSERT(dbg.trace().at(2).mnemonic.find("$1000") != std::string::npos);
    ASSERT_EQ((int)dbg.trace().at(2).regs.at("A"), 0x42);   // recorded before STA executed

    // Stack tracking still sees the PHA without disassembly text
    ASSERT_EQ(dbg.stackTrace().depth(), 1);

    // Reverse step restores registers from the raw capture
    ASSERT(dbg.reverseStep());
    ASSERT_EQ((int)cpu.pc(), 0x1003);
    ASSERT_EQ((int)bus.peek8(0x1000), 0xA9);                // memory write undone
}

// decodeEntry() (used per step) must agree with disassembleEntry() on every opcode.
TEST_CASE(debug_6502_decode_entry_matches_disassembly) {
    MOS6502 cpu;
    for (int op = 0; op < 256; op++) {
        for (uint8_t operand : { (uint8_t)0x00, (uint8_t)0x7F, (uint8_t)0x80, (uint8_t)0xFE }) {
            FlatMemoryBus bus("system", 16);
            bus.write8(0x2000, (uint8_t)op);
            bus.write8(0x2001, operand);
            bus.write8(0x2002, (uint8_t)(operand ^ 0x5A));
            DisasmEntry full, lite;
            cpu.disassembleEntry(&bus, 0x2000, &full);
            cpu.decodeEntry(&bus, 0x2000, &lite);
            ASSERT_EQ(lite.bytes, full.bytes);
            ASSERT_EQ(lite.isCall, full.isCall);
            ASSERT_EQ(lite.isReturn, full.isReturn);
            ASSERT_EQ(lite.isBranch, full.isBranch);
            ASSERT_EQ(lite.isIllegal, full.isIllegal);
            ASSERT_EQ(lite.targetAddr, full.targetAddr);
            ASSERT(lite.stackOp == full.stackOp);
        }
    }
}
