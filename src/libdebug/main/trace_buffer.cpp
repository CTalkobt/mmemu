#include "trace_buffer.h"
#include "libcore/main/icore.h"
#include "libmem/main/ibus.h"
#include "libmem/main/byte_window_bus.h"
#include <stdexcept>

TraceBuffer::TraceBuffer(size_t capacity)
    : m_buffer(capacity) {
}

void TraceBuffer::push(const TraceEntry& entry) {
    m_buffer.push(entry);
}

TraceEntry& TraceBuffer::pushNew() {
    TraceEntry& te = m_buffer.pushSlot();
    te.memWrites.clear();
    te.mnemonic.clear();
    te.regs.clear();
    te.instrLen = 0;
    te.rawRegCount = 0;
    return te;
}

void TraceBuffer::clear() {
    m_buffer.clear();
}

void TraceBuffer::materialize(const TraceEntry& e) const {
    if (!m_cpu) return;
    if (e.mnemonic.empty() && e.instrLen > 0) {
        ByteWindowBus win(e.addr, e.instr, e.instrLen);
        char buf[128];
        buf[0] = '\0';
        m_cpu->disassembleOne(&win, e.addr, buf, sizeof(buf));
        e.mnemonic = buf;
    }
    if (e.regs.empty() && e.rawRegCount > 0) {
        int k = 0;
        for (int i = 0; i < m_cpu->regCount() && k < e.rawRegCount; ++i) {
            const auto* desc = m_cpu->regDescriptor(i);
            if (desc->flags & REGFLAG_INTERNAL) continue;
            e.regs[desc->name] = e.rawRegs[k++];
        }
    }
}

const TraceEntry& TraceBuffer::at(size_t index) const {
    const TraceEntry& e = m_buffer[index];
    materialize(e);
    return e;
}

bool TraceBuffer::popBack(TraceEntry& out) {
    if (!m_buffer.popBack(out)) return false;
    materialize(out);
    return true;
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
