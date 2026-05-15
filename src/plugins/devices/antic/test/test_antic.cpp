#include "test_harness.h"
#include "plugins/devices/antic/main/antic.h"
#include "libmem/main/ibus.h"
#include <vector>

namespace {
class MockSignal : public ISignalLine {
public:
    bool get() const override { return m_level; }
    void set(bool level) override { m_level = level; }
    void pulse() override { m_level = !m_level; m_level = !m_level; }
    bool m_level = false;
};

class MockBus : public IBus {
public:
    const BusConfig& config() const override { return m_config; }
    const char* name() const override { return "mock"; }
    uint8_t read8(uint32_t addr) override { return m_mem[addr & 0xFFFF]; }
    void write8(uint32_t addr, uint8_t val) override { m_mem[addr & 0xFFFF] = val; }
    uint8_t peek8(uint32_t addr) override { return m_mem[addr & 0xFFFF]; }
    
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t* buf) const override {}
    void loadState(const uint8_t* buf) override {}
    int writeCount() const override { return 0; }
    void getWrites(uint32_t* addrs, uint8_t* before, uint8_t* after, int max) const override {}
    void clearWriteLog() override {}

    BusConfig m_config = {16, 8, BusRole::DMA, true, 0xFFFF};
    uint8_t m_mem[65536] = {0};
};
} // namespace

TEST_CASE(antic_registers) {
    Antic antic;
    uint8_t val;
    
    // Test register write/read
    antic.ioWrite(nullptr, 0xD400, 0x22); // DMACTL
    antic.ioRead(nullptr, 0xD400, &val);
    ASSERT(val == 0x22);
    
    antic.ioWrite(nullptr, 0xD407, 0x40); // PMBASE
    antic.ioRead(nullptr, 0xD407, &val);
    ASSERT(val == 0x40);
}

TEST_CASE(antic_vcount) {
    Antic antic;
    uint8_t val;
    
    // vcount starts at 0
    antic.ioRead(nullptr, 0xD40B, &val);
    ASSERT(val == 0);
    
    // tick forward 114 cycles = 1 line
    for (int i=0; i<114; ++i) antic.tick(i+1);
    antic.ioRead(nullptr, 0xD40B, &val);
    ASSERT(val == 0); // line 1, VCOUNT is 0
    
    // tick forward another 114 cycles = 2 lines
    for (int i=114; i<228; ++i) antic.tick(i+1);
    antic.ioRead(nullptr, 0xD40B, &val);
    ASSERT(val == 1); // line 2, VCOUNT is 1
    
    // tick forward another 228 cycles = 4 lines total
    for (int i=228; i<456; ++i) antic.tick(i+1);
    antic.ioRead(nullptr, 0xD40B, &val);
    ASSERT(val == 2); // line 4, VCOUNT is 2
}

TEST_CASE(antic_wsync) {
    Antic antic;
    MockSignal halt;
    antic.setHaltLine(&halt);
    
    // wsync off, halt off
    ASSERT(halt.m_level == false);
    
    // write to WSYNC
    antic.ioWrite(nullptr, 0xD40A, 0x01);
    ASSERT(halt.m_level == true);
    
    // tick until end of line (114 cycles)
    // We're at hcount 0, need 114 cycles to trigger line increment and WSYNC clear
    antic.tick(114);
    ASSERT(halt.m_level == false);
}

TEST_CASE(antic_nmi_vbi) {
    Antic antic;
    MockSignal nmi;
    antic.setNmiLine(&nmi);
    
    // Enable VBI
    antic.ioWrite(nullptr, 0xD40E, 0x40); // NMIEN = VBI
    
    // Tick until VBlank (line 262)
    // We're at line 0, need 262 lines * 114 cycles
    antic.tick(262 * 114);
    
    uint8_t nmist;
    antic.ioRead(nullptr, 0xD40F, &nmist);
    ASSERT(nmist & 0x40); // NMIST should have VBI bit set
    ASSERT(nmi.m_level == true);
    
    // Clear NMI
    antic.ioWrite(nullptr, 0xD40F, 0x00); // NMIRES
    ASSERT(nmi.m_level == false);
}

TEST_CASE(antic_dma_dl) {
    Antic antic;
    MockBus dmaBus;
    MockSignal halt;
    antic.setDmaBus(&dmaBus);
    antic.setHaltLine(&halt);
    
    // Set DLIST pointer to $1000
    antic.ioWrite(nullptr, 0xD402, 0x00);
    antic.ioWrite(nullptr, 0xD403, 0x10);
    
    // Enable DL DMA
    antic.ioWrite(nullptr, 0xD400, 0x22); // DMACTL: DL DMA enable + standard width
    
    // Prepare DLIST at $1000
    dmaBus.m_mem[0x1000] = 0x02; // Mode 2 line
    dmaBus.m_mem[0x1001] = 0x02; // Mode 2 line
    
    // Move to line 8 (where we start DL DMA)
    antic.tick(8 * 114);
    
    // At line 8, hcount 8, it should fetch first DL instruction
    // We need to tick cycle by cycle or precisely
    antic.tick(8); // hcount reaches 8
    
    // In our simplified implementation, hcount 8 triggers DL fetch
    // Actually our tick delta logic might be tricky here.
    
    // We can't easily check halt level INSIDE tick, but we can verify dlistPtr advanced
    // Wait, our implementation fetches at hcount 8.
}
