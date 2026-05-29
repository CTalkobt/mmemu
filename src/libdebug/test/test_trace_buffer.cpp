#include "test_harness.h"
#include "trace_buffer.h"
#include "libmem/main/memory_bus.h"
#include "plugins/6502/main/cpu6502.h"

// --- CircularBuffer popBack/back tests ---

TEST_CASE(circular_buffer_popBack) {
    CircularBuffer<int> buf(5);
    buf.push(10);
    buf.push(20);
    buf.push(30);
    ASSERT(buf.size() == 3);

    int val;
    ASSERT(buf.popBack(val));
    ASSERT(val == 30);
    ASSERT(buf.size() == 2);

    ASSERT(buf.popBack(val));
    ASSERT(val == 20);
    ASSERT(buf.size() == 1);

    ASSERT(buf.popBack(val));
    ASSERT(val == 10);
    ASSERT(buf.size() == 0);

    // Empty buffer
    ASSERT(!buf.popBack(val));
}

TEST_CASE(circular_buffer_back) {
    CircularBuffer<int> buf(3);
    buf.push(10);
    ASSERT(buf.back() == 10);

    buf.push(20);
    ASSERT(buf.back() == 20);

    buf.push(30);
    ASSERT(buf.back() == 30);

    // After wrap
    buf.push(40);
    ASSERT(buf.back() == 40);
    ASSERT(buf.size() == 3);
}

TEST_CASE(circular_buffer_popBack_after_wrap) {
    CircularBuffer<int> buf(3);
    buf.push(10);
    buf.push(20);
    buf.push(30);
    buf.push(40); // wraps: [40, 20, 30], oldest=20

    int val;
    ASSERT(buf.popBack(val));
    ASSERT(val == 40);
    ASSERT(buf.popBack(val));
    ASSERT(val == 30);
    ASSERT(buf.popBack(val));
    ASSERT(val == 20);
    ASSERT(buf.size() == 0);
    ASSERT(!buf.popBack(val));
}

// --- TraceBuffer reverseStep tests ---

TEST_CASE(trace_reverse_step_registers) {
    auto* bus = new FlatMemoryBus("test", 16);
    MOS6502 cpu;
    cpu.setDataBus(bus);
    cpu.setCodeBus(bus);
    cpu.reset();

    TraceBuffer tb(100);

    // Simulate a trace entry with known register state
    TraceEntry e;
    e.addr = 0x0200;
    e.regs["A"] = 0x42;
    e.regs["X"] = 0x10;
    e.regs["Y"] = 0x20;
    e.regs["SP"] = 0xFD;
    e.regs["PC"] = 0x0200;
    e.regs["P"] = 0x24;
    tb.push(e);

    // Modify CPU state
    cpu.regWriteByName("A", 0xFF);
    cpu.regWriteByName("X", 0xFF);
    cpu.setPc(0x1000);

    // Reverse step should restore
    ASSERT(tb.reverseStep(&cpu, bus));
    ASSERT(cpu.regReadByName("A") == 0x42);
    ASSERT(cpu.regReadByName("X") == 0x10);
    ASSERT(cpu.pc() == 0x0200);
    ASSERT(tb.size() == 0);

    delete bus;
}

TEST_CASE(trace_reverse_step_memory_undo) {
    auto* bus = new FlatMemoryBus("test", 16);
    MOS6502 cpu;
    cpu.setDataBus(bus);
    cpu.setCodeBus(bus);
    cpu.reset();

    TraceBuffer tb(100);

    // Pre-fill memory
    bus->write8(0x1000, 0xAA);
    bus->write8(0x1001, 0xBB);

    // Trace entry with memory writes to undo
    TraceEntry e;
    e.addr = 0x0200;
    e.regs["PC"] = 0x0200;
    e.memWrites.push_back({0x1000, 0xAA}); // old value was 0xAA
    e.memWrites.push_back({0x1001, 0xBB}); // old value was 0xBB
    tb.push(e);

    // Simulate the writes that happened during execution
    bus->write8(0x1000, 0x11);
    bus->write8(0x1001, 0x22);
    ASSERT(bus->read8(0x1000) == 0x11);
    ASSERT(bus->read8(0x1001) == 0x22);

    // Reverse step should restore old values
    ASSERT(tb.reverseStep(&cpu, bus));
    ASSERT(bus->read8(0x1000) == 0xAA);
    ASSERT(bus->read8(0x1001) == 0xBB);

    delete bus;
}

TEST_CASE(trace_reverse_step_undo_order) {
    // Memory writes should be undone in reverse order
    auto* bus = new FlatMemoryBus("test", 16);
    MOS6502 cpu;
    cpu.setDataBus(bus);
    cpu.setCodeBus(bus);
    cpu.reset();

    TraceBuffer tb(100);

    // Two writes to the SAME address — order matters
    TraceEntry e;
    e.addr = 0x0200;
    e.regs["PC"] = 0x0200;
    e.memWrites.push_back({0x1000, 0x00}); // first write: old=0x00
    e.memWrites.push_back({0x1000, 0x11}); // second write: old=0x11 (value from first write)
    tb.push(e);

    bus->write8(0x1000, 0xFF); // current state after both writes

    // Reverse should undo second write first (restore 0x11), then first (restore 0x00)
    ASSERT(tb.reverseStep(&cpu, bus));
    ASSERT(bus->read8(0x1000) == 0x00);

    delete bus;
}

TEST_CASE(trace_reverse_step_empty) {
    auto* bus = new FlatMemoryBus("test", 16);
    MOS6502 cpu;
    cpu.setDataBus(bus);
    cpu.setCodeBus(bus);

    TraceBuffer tb(100);
    ASSERT(!tb.reverseStep(&cpu, bus));

    delete bus;
}

TEST_CASE(trace_reverse_step_multiple) {
    auto* bus = new FlatMemoryBus("test", 16);
    MOS6502 cpu;
    cpu.setDataBus(bus);
    cpu.setCodeBus(bus);
    cpu.reset();

    TraceBuffer tb(100);

    // Push 3 entries
    TraceEntry e1; e1.addr = 0x0200; e1.regs["A"] = 0x00; e1.regs["PC"] = 0x0200;
    TraceEntry e2; e2.addr = 0x0202; e2.regs["A"] = 0x42; e2.regs["PC"] = 0x0202;
    TraceEntry e3; e3.addr = 0x0204; e3.regs["A"] = 0x99; e3.regs["PC"] = 0x0204;
    tb.push(e1);
    tb.push(e2);
    tb.push(e3);
    ASSERT(tb.size() == 3);

    // Reverse 3 steps
    ASSERT(tb.reverseStep(&cpu, bus));
    ASSERT(cpu.regReadByName("A") == 0x99);
    ASSERT(cpu.pc() == 0x0204);

    ASSERT(tb.reverseStep(&cpu, bus));
    ASSERT(cpu.regReadByName("A") == 0x42);
    ASSERT(cpu.pc() == 0x0202);

    ASSERT(tb.reverseStep(&cpu, bus));
    ASSERT(cpu.regReadByName("A") == 0x00);
    ASSERT(cpu.pc() == 0x0200);

    ASSERT(!tb.reverseStep(&cpu, bus));

    delete bus;
}

// --- Original trace buffer tests ---

TEST_CASE(trace_buffer_basic) {
    TraceBuffer buffer(3);
    ASSERT(buffer.size() == 0);

    TraceEntry e1; e1.addr = 0x1000;
    TraceEntry e2; e2.addr = 0x2000;
    TraceEntry e3; e3.addr = 0x3000;

    buffer.push(e1);
    buffer.push(e2);
    buffer.push(e3);

    ASSERT(buffer.size() == 3);
    // When size < capacity, it returns by index directly
    ASSERT(buffer.at(0).addr == 0x1000);
    ASSERT(buffer.at(1).addr == 0x2000);
    ASSERT(buffer.at(2).addr == 0x3000);
}

TEST_CASE(trace_buffer_wrap) {
    TraceBuffer buffer(3);
    TraceEntry e1; e1.addr = 0x1000;
    TraceEntry e2; e2.addr = 0x2000;
    TraceEntry e3; e3.addr = 0x3000;
    TraceEntry e4; e4.addr = 0x4000;

    buffer.push(e1);
    buffer.push(e2);
    buffer.push(e3);
    buffer.push(e4); // Should overwrite e1

    ASSERT(buffer.size() == 3);
    // head was 0, after 3 pushes it's 0 again. After 4th push, it's 1.
    // buffer = [e4, e2, e3], head = 1.
    // CircularBuffer[0] when size == capacity returns buffer[(head + index) % capacity]
    // index 0 -> buffer[(1+0)%3] = buffer[1] = e2
    // index 1 -> buffer[(1+1)%3] = buffer[2] = e3
    // index 2 -> buffer[(1+2)%3] = buffer[0] = e4

    ASSERT(buffer.at(0).addr == 0x2000);
    ASSERT(buffer.at(1).addr == 0x3000);
    ASSERT(buffer.at(2).addr == 0x4000);
}
