#pragma once

#include "util/circular_buffer.h"
#include <cstdint>
#include <string>
#include <vector>
#include <map>

struct MemoryWrite {
    uint32_t addr;
    uint8_t  before;
};

struct TraceEntry {
    static constexpr int MAX_INSTR_BYTES = 8;
    static constexpr int MAX_RAW_REGS    = 24;
    static constexpr int MAX_CPU_STATE   = 64;

    uint32_t addr;
    /** Disassembly text; formatted from instr[] on first read when empty. */
    mutable std::string mnemonic;
    /** Registers by name; built from rawRegs[] on first read when empty. */
    mutable std::map<std::string, uint32_t> regs;
    uint64_t cycles;
    std::vector<MemoryWrite> memWrites;

    // Captured at step time for lazy formatting (TraceBuffer::at/popBack)
    uint8_t  instr[MAX_INSTR_BYTES];
    uint8_t  instrLen    = 0;
    uint32_t rawRegs[MAX_RAW_REGS];   // values of the CPU's non-internal registers, in order
    uint8_t  rawRegCount = 0;
    // Or, cheaper: the CPU's saveState() snapshot (used when it fits)
    uint8_t  cpuState[MAX_CPU_STATE];
    uint8_t  cpuStateLen = 0;
};

class ICore;
class IBus;

class TraceBuffer {
public:
    TraceBuffer(size_t capacity = 1000);

    /** CPU used to format captured instruction bytes and registers on read. */
    void setCpu(ICore* cpu) { m_cpu = cpu; }

    void push(const TraceEntry& entry);

    /**
     * Start a new entry in place: returns the recycled slot with memWrites,
     * text, registers and raw captures cleared (keeping their allocations).
     * The caller sets addr/cycles and either the text fields or the raw ones.
     */
    TraceEntry& pushNew();
    void clear();

    size_t size() const { return m_buffer.size(); }
    size_t capacity() const { return m_buffer.capacity(); }
    const TraceEntry& at(size_t index) const;

    /** Pop the most recent entry (for reverse-step). */
    bool popBack(TraceEntry& out);

    /** Access the current (most recent) entry to append memory writes. */
    TraceEntry* current();

    /** Reverse-step: pop last entry, restore registers and undo memory writes. */
    bool reverseStep(ICore* cpu, IBus* bus);

private:
    /** Fill mnemonic/regs from the captured raw data if not yet done. */
    void materialize(const TraceEntry& e) const;

    CircularBuffer<TraceEntry> m_buffer;
    ICore* m_cpu = nullptr;
};
