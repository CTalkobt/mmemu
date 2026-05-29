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
    uint32_t addr;
    std::string mnemonic;
    std::map<std::string, uint32_t> regs;
    uint64_t cycles;
    std::vector<MemoryWrite> memWrites;
};

class ICore;
class IBus;

class TraceBuffer {
public:
    TraceBuffer(size_t capacity = 1000);

    void push(const TraceEntry& entry);
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
    CircularBuffer<TraceEntry> m_buffer;
};
