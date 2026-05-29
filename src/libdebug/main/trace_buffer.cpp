#include "trace_buffer.h"
#include "libcore/main/icore.h"
#include "libmem/main/ibus.h"
#include <stdexcept>

TraceBuffer::TraceBuffer(size_t capacity)
    : m_buffer(capacity) {
}

void TraceBuffer::push(const TraceEntry& entry) {
    m_buffer.push(entry);
}

void TraceBuffer::clear() {
    m_buffer.clear();
}

const TraceEntry& TraceBuffer::at(size_t index) const {
    return m_buffer[index];
}

bool TraceBuffer::popBack(TraceEntry& out) {
    return m_buffer.popBack(out);
}

TraceEntry* TraceBuffer::current() {
    if (m_buffer.size() == 0) return nullptr;
    return &m_buffer.back();
}

bool TraceBuffer::reverseStep(ICore* cpu, IBus* bus) {
    TraceEntry entry;
    if (!popBack(entry)) return false;

    // Undo memory writes in reverse order
    for (auto it = entry.memWrites.rbegin(); it != entry.memWrites.rend(); ++it) {
        bus->write8(it->addr, it->before);
    }

    // Restore registers
    for (const auto& [name, val] : entry.regs) {
        cpu->regWriteByName(name.c_str(), val);
    }

    // Restore PC to the entry's address (the instruction we're undoing)
    cpu->setPc(entry.addr);

    return true;
}
