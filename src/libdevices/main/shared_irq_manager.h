#pragma once

#include "isignal_line.h"
#include "libcore/main/icore.h"
#include <vector>
#include <cstdint>

/**
 * Manages a shared (wired-OR) IRQ line where multiple devices can assert
 * interrupts independently. If ANY input line is high, the CPU IRQ pin is
 * driven high. Each participating device gets its own DeviceLine.
 *
 * Usage:
 *   auto* mgr = new SharedIrqManager(cpu);
 *   pia1->setIrqALine(mgr->createLine());
 *   pia1->setIrqBLine(mgr->createLine());
 *   via->setIrqLine(mgr->createLine());
 *   desc->deleters.push_back([mgr]() { delete mgr; });
 */
class SharedIrqManager {
public:
    explicit SharedIrqManager(ICore* cpu) : m_cpu(cpu) {}

    ~SharedIrqManager() {
        for (auto* l : m_lines) delete l;
    }

    class DeviceLine : public ISignalLine {
    public:
        DeviceLine(SharedIrqManager* parent, uint32_t bit) : m_parent(parent), m_bit(bit) {}

        bool get()  const override { return m_level; }
        void pulse() override { m_parent->m_cpu->triggerIrq(); }
        void set(bool level) override {
            m_level = level;
            if (level) m_parent->m_state |=  m_bit;
            else       m_parent->m_state &= ~m_bit;
            m_parent->m_cpu->setIrqLine(m_parent->m_state != 0);
        }

    private:
        SharedIrqManager* m_parent;
        uint32_t          m_bit;
        bool              m_level = false;
    };

    /** Allocate a new DeviceLine and return it. Owned by this manager. */
    ISignalLine* createLine() {
        uint32_t bit = 1u << (uint32_t)m_lines.size();
        auto* line = new DeviceLine(this, bit);
        m_lines.push_back(line);
        return line;
    }

private:
    ICore*   m_cpu;
    uint32_t m_state = 0;
    std::vector<DeviceLine*> m_lines;
};

/**
 * NMI variant of SharedIrqManager.  Same wired-OR logic but drives the
 * CPU NMI pin instead of IRQ.
 */
class SharedNmiManager {
public:
    explicit SharedNmiManager(ICore* cpu) : m_cpu(cpu) {}
    ~SharedNmiManager() { for (auto* l : m_lines) delete l; }

    class DeviceLine : public ISignalLine {
    public:
        DeviceLine(SharedNmiManager* parent, uint32_t bit) : m_parent(parent), m_bit(bit) {}
        bool get()  const override { return m_level; }
        void pulse() override { m_parent->m_cpu->triggerNmi(); }
        void set(bool level) override {
            m_level = level;
            if (level) m_parent->m_state |=  m_bit;
            else       m_parent->m_state &= ~m_bit;
            m_parent->m_cpu->setNmiLine(m_parent->m_state != 0);
        }
    private:
        SharedNmiManager* m_parent;
        uint32_t          m_bit;
        bool              m_level = false;
    };

    ISignalLine* createLine() {
        uint32_t bit = 1u << (uint32_t)m_lines.size();
        auto* line = new DeviceLine(this, bit);
        m_lines.push_back(line);
        return line;
    }

private:
    ICore*   m_cpu;
    uint32_t m_state = 0;
    std::vector<DeviceLine*> m_lines;
};
