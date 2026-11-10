#pragma once

#include "io_handler.h"

/**
 * VICE's debug cartridge (`-debugcart`): a write to $D7FF ends the run with
 * that value as the exit code, 0 meaning pass. VICE's test programs report
 * their result this way (testprogs/testbench, "exitcode" tests). Reads fall
 * through to whatever else answers the address.
 */
class DebugCart : public IOHandler {
public:
    explicit DebugCart(uint32_t addr = 0xD7FF) : m_addr(addr) {}

    const char* name()     const override { return "DebugCart"; }
    uint32_t    baseAddr() const override { return m_addr; }
    uint32_t    addrMask() const override { return 0; }

    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t addr, uint8_t val) override {
        if (addr != m_addr) return false;
        if (!m_written) {               // the first code counts, as VICE exits on it
            m_written = true;
            m_code = val;
        }
        return true;
    }
    void reset() override {}
    void tick(uint64_t) override {}

    /** True once a program has written its exit code. */
    bool written() const { return m_written; }
    /** The exit code written (0 = pass). */
    uint8_t code() const { return m_code; }
    /** Arm it for another run. */
    void clear() { m_written = false; m_code = 0; }

private:
    uint32_t m_addr;
    bool     m_written = false;
    uint8_t  m_code = 0;
};
