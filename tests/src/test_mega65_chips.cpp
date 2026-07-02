#include "test_harness.h"
#include "plugins/devices/vic4/main/vic4.h"
#include "plugins/devices/f018b_dma/main/f018b_dma.h"
#include "plugins/devices/mega65_math/main/mega65_math.h"
#include "plugins/devices/sid_pair/main/sid_pair.h"
#include "plugins/devices/map_mmu/main/key_register.h"
#include "libmem/main/sparse_memory_bus.h"
#include "libmem/main/memory_bus.h"
#include "libdevices/main/isignal_line.h"
#include <cstring>
#include <vector>

namespace {

class MockIrqLine : public ISignalLine {
public:
    bool get() const override { return m_level; }
    void set(bool v) override { m_level = v; m_setCount++; }
    void pulse() override { set(!m_level); set(!m_level); }
    bool m_level = false;
    int  m_setCount = 0;
};

void write32(IOHandler& dev, uint32_t addr, uint32_t val) {
    dev.ioWrite(nullptr, addr,     val & 0xFF);
    dev.ioWrite(nullptr, addr + 1, (val >> 8) & 0xFF);
    dev.ioWrite(nullptr, addr + 2, (val >> 16) & 0xFF);
    dev.ioWrite(nullptr, addr + 3, (val >> 24) & 0xFF);
}

} // namespace

TEST_CASE(mega65_chips_vic4) {
    VIC4 vic;
    KeyRegister keyReg;

    // Connect them so we can test KEY register unlock
    keyReg.setPersonalityChangeCallback([&vic](IopersonalityMode mode) {
        vic.setLocked(mode != IopersonalityMode::MEGA65);
    });

    // Write to KEY to unlock MEGA65 personality
    keyReg.ioWrite(nullptr, 0xD02F, 0x47);
    keyReg.ioWrite(nullptr, 0xD02F, 0x53);
    ASSERT(!vic.isLocked());

    // 1. Test writing a non-standard 28-bit screen address to SCRNPTR ($D060-$D063)
    uint32_t testScreenAddr = 0x01A4000;
    write32(vic, 0xD060, testScreenAddr);

    ASSERT_EQ(vic.screenBase(), testScreenAddr);

    // Also write to $D04C-$D04F to verify register writes to text position registers
    vic.ioWrite(nullptr, 0xD04C, 0x12);
    vic.ioWrite(nullptr, 0xD04D, 0x03); // X Pos = 0x312
    vic.ioWrite(nullptr, 0xD04E, 0x34);
    vic.ioWrite(nullptr, 0xD04F, 0x02); // Y Pos = 0x234

    ASSERT_EQ((int)vic.getTextXPos(), 0x312);
    ASSERT_EQ((int)vic.getTextYPos(), 0x234);

    // Set up a mock SparseMemoryBus
    SparseMemoryBus dmaBus{"dma", 28};
    vic.setDmaBus(&dmaBus);

    // Pre-seed some character codes at the new screen base address
    dmaBus.write8(testScreenAddr, 0x01); // Char A
    dmaBus.write8(testScreenAddr + 1, 0x02); // Char B
    dmaBus.write8(testScreenAddr + 2, 0x03); // Char C

    // Configure a mock character ROM
    std::vector<uint8_t> mockCharRom(4096, 0);
    mockCharRom[1 * 8] = 0xFF; // top line of 'A' has all pixels on
    mockCharRom[2 * 8] = 0xAA;
    vic.setCharRom(mockCharRom.data(), mockCharRom.size());

    // Set color RAM
    std::vector<uint8_t> mockColorRam(1024, 0x01);
    vic.setColorRam(mockColorRam.data());

    // Call renderFrame
    std::vector<uint32_t> frameBuffer(VIC4::V4_FRAME_W * 312, 0);
    vic.renderFrame(frameBuffer.data());
}

TEST_CASE(mega65_chips_vic4_palette) {
    VIC4 vic;
    vic.setLocked(false);

    // Write 3 bytes to $D100/$D200/$D300 for entry 1; read back each byte; verify values match.
    vic.ioWrite(nullptr, 0xD101, 0x11);
    vic.ioWrite(nullptr, 0xD201, 0x22);
    vic.ioWrite(nullptr, 0xD301, 0x33);

    uint8_t r = 0, g = 0, b = 0;
    ASSERT(vic.ioRead(nullptr, 0xD101, &r));
    ASSERT(vic.ioRead(nullptr, 0xD201, &g));
    ASSERT(vic.ioRead(nullptr, 0xD301, &b));

    ASSERT_EQ(r, 0x11);
    ASSERT_EQ(g, 0x22);
    ASSERT_EQ(b, 0x33);
}

TEST_CASE(mega65_chips_vic4_raster) {
    VIC4 vic;
    vic.setLocked(false);
    MockIrqLine irq;
    vic.setIrqLine(&irq);

    // Set raster compare line to 10
    vic.ioWrite(nullptr, 0xD012, 10);
    uint8_t d011 = 0;
    vic.ioRead(nullptr, 0xD011, &d011);
    vic.ioWrite(nullptr, 0xD011, d011 & 0x7F); // clear MSB

    // Enable raster IRQ
    vic.ioWrite(nullptr, 0xD01A, 0x01);

    // Initially IRQ should not be asserted
    ASSERT(!irq.m_level);

    // Record the set count after initialization, then tick
    int countBefore = irq.m_setCount;

    // Tick the VIC-IV past raster line 10
    for (int i = 0; i < 700; ++i) {
        vic.tick(1);
    }

    // Verify the IRQ signal was asserted
    ASSERT(irq.m_level);
    ASSERT(irq.m_setCount > countBefore);
}

TEST_CASE(mega65_chips_dma) {
    SparseMemoryBus sparseBus{"sparse", 28};
    F018bDmaDevice dma{0xD700};
    dma.setDmaBus(&sparseBus);

    // Pre-allocate pages
    for (uint32_t i = 0; i < 0x40000; i += 0x1000) {
        sparseBus.read8(i);
    }

    // DMA copy: set up source at $10000; trigger a copy DMA to $20000 for 256 bytes; verify.
    for (int i = 0; i < 256; ++i) {
        sparseBus.write8(0x10000 + i, (uint8_t)i);
    }

    uint32_t listAddr = 0x2000;
    sparseBus.write8(listAddr + 0, 0x00);      // Copy, no chain
    sparseBus.write8(listAddr + 1, 0x00);      // Count = 256
    sparseBus.write8(listAddr + 2, 0x01);      // Count MSB
    sparseBus.write8(listAddr + 3, 0x00);      // Src LSB
    sparseBus.write8(listAddr + 4, 0x00);      // Src MSB
    sparseBus.write8(listAddr + 5, 0x01);      // Src bank
    sparseBus.write8(listAddr + 6, 0x00);      // Dst LSB
    sparseBus.write8(listAddr + 7, 0x00);      // Dst MSB
    sparseBus.write8(listAddr + 8, 0x02);      // Dst bank
    sparseBus.write8(listAddr + 9, 0x00);
    sparseBus.write8(listAddr + 10, 0x00);

    dma.ioWrite(&sparseBus, 0xD701, 0x20); // List Address MSB ($20)
    dma.ioWrite(&sparseBus, 0xD700, 0x00); // List Address LSB ($00) + trigger

    for (int i = 0; i < 300; ++i) {
        dma.tick(1);
    }

    for (int i = 0; i < 256; ++i) {
        ASSERT_EQ((int)sparseBus.read8(0x20000 + i), i);
    }
}

TEST_CASE(mega65_chips_dma_fill) {
    SparseMemoryBus sparseBus{"sparse", 28};
    F018bDmaDevice dma{0xD700};
    dma.setDmaBus(&sparseBus);

    // Pre-allocate pages
    for (uint32_t i = 0; i < 0x40000; i += 0x1000) {
        sparseBus.read8(i);
    }

    // DMA fill: trigger fill DMA at $30000 for 128 bytes with $42; verify.
    uint32_t listAddr = 0x2000;
    sparseBus.write8(listAddr + 0, 0x03);      // Fill
    sparseBus.write8(listAddr + 1, 0x80);      // Count = 128
    sparseBus.write8(listAddr + 2, 0x00);
    sparseBus.write8(listAddr + 3, 0x42);      // Fill byte
    sparseBus.write8(listAddr + 4, 0x00);
    sparseBus.write8(listAddr + 5, 0x00);
    sparseBus.write8(listAddr + 6, 0x00);      // Dst LSB
    sparseBus.write8(listAddr + 7, 0x00);      // Dst MSB
    sparseBus.write8(listAddr + 8, 0x03);      // Dst bank
    sparseBus.write8(listAddr + 9, 0x00);
    sparseBus.write8(listAddr + 10, 0x00);

    dma.ioWrite(&sparseBus, 0xD701, 0x20);
    dma.ioWrite(&sparseBus, 0xD700, 0x00);

    for (int i = 0; i < 200; ++i) {
        dma.tick(1);
    }

    for (int i = 0; i < 128; ++i) {
        ASSERT_EQ((int)sparseBus.read8(0x30000 + i), 0x42);
    }
}

TEST_CASE(mega65_chips_dma_chain) {
    SparseMemoryBus sparseBus{"sparse", 28};
    F018bDmaDevice dma{0xD700};
    dma.setDmaBus(&sparseBus);

    // Pre-allocate pages
    for (uint32_t i = 0; i < 0x40000; i += 0x1000) {
        sparseBus.read8(i);
    }

    // DMA chain: two-job chain (fill then copy).
    // F018A mode (default, 11 bytes/job): chained jobs are contiguous in memory.
    uint32_t job1Addr = 0x2000;
    sparseBus.write8(job1Addr + 0, 0x07);      // Fill + chain (0x04)
    sparseBus.write8(job1Addr + 1, 0x80);      // Count = 128
    sparseBus.write8(job1Addr + 2, 0x00);
    sparseBus.write8(job1Addr + 3, 0x5A);      // Fill byte
    sparseBus.write8(job1Addr + 4, 0x00);
    sparseBus.write8(job1Addr + 5, 0x00);
    sparseBus.write8(job1Addr + 6, 0x00);
    sparseBus.write8(job1Addr + 7, 0x00);
    sparseBus.write8(job1Addr + 8, 0x03);      // Dst $30000
    sparseBus.write8(job1Addr + 9, 0x00);      // Modulo LSB (unused)
    sparseBus.write8(job1Addr + 10, 0x00);     // Modulo MSB (unused)

    // Job 2 follows immediately at $200B (= $2000 + 11)
    uint32_t job2Addr = job1Addr + 11;
    sparseBus.write8(job2Addr + 0, 0x00);      // Copy
    sparseBus.write8(job2Addr + 1, 0x80);      // Count = 128
    sparseBus.write8(job2Addr + 2, 0x00);
    sparseBus.write8(job2Addr + 3, 0x00);
    sparseBus.write8(job2Addr + 4, 0x00);
    sparseBus.write8(job2Addr + 5, 0x03);      // Src $30000
    sparseBus.write8(job2Addr + 6, 0x00);
    sparseBus.write8(job2Addr + 7, 0x00);
    sparseBus.write8(job2Addr + 8, 0x02);      // Dst $20000
    sparseBus.write8(job2Addr + 9, 0x00);
    sparseBus.write8(job2Addr + 10, 0x00);

    dma.ioWrite(&sparseBus, 0xD701, 0x20);
    dma.ioWrite(&sparseBus, 0xD700, 0x00);

    for (int i = 0; i < 400; ++i) {
        dma.tick(1);
    }

    for (int i = 0; i < 128; ++i) {
        ASSERT_EQ((int)sparseBus.read8(0x30000 + i), 0x5A);
        ASSERT_EQ((int)sparseBus.read8(0x20000 + i), 0x5A);
    }
}

TEST_CASE(mega65_chips_math) {
    Mega65MathDevice dev(0xD700);

    // Math multiply
    write32(dev, 0xD770, 0x0100);
    write32(dev, 0xD774, 0x0100);

    uint8_t b[8];
    for (int i = 0; i < 8; ++i) dev.ioRead(nullptr, 0xD778 + i, &b[i]);
    uint64_t lo = b[0] | (uint64_t(b[1]) << 8) | (uint64_t(b[2]) << 16) | (uint64_t(b[3]) << 24);
    uint64_t hi = b[4] | (uint64_t(b[5]) << 8) | (uint64_t(b[6]) << 16) | (uint64_t(b[7]) << 24);
    uint64_t prod = lo | (hi << 32);

    ASSERT_EQ(prod, (uint64_t)0x00010000);

    // Math divide
    write32(dev, 0xD760, 0x00010000);
    write32(dev, 0xD764, 0x0100);

    uint8_t qb[8];
    for (int i = 0; i < 8; ++i) dev.ioRead(nullptr, 0xD768 + i, &qb[i]);
    uint64_t qlo = qb[0] | (uint64_t(qb[1]) << 8) | (uint64_t(qb[2]) << 16) | (uint64_t(qb[3]) << 24);
    uint64_t qhi = qb[4] | (uint64_t(qb[5]) << 8) | (uint64_t(qb[6]) << 16) | (uint64_t(qb[7]) << 24);
    uint64_t quot = qlo | (qhi << 32);

    uint8_t rb[4];
    for (int i = 0; i < 4; ++i) dev.ioRead(nullptr, 0xD770 + i, &rb[i]);
    uint32_t rem = rb[0] | (uint32_t(rb[1]) << 8) | (uint32_t(rb[2]) << 16) | (uint32_t(rb[3]) << 24);

    ASSERT_EQ(quot, (uint64_t)0x0100);
    ASSERT_EQ(rem, (uint32_t)0x0000);

    // Math divide by zero
    write32(dev, 0xD760, 42);
    write32(dev, 0xD764, 0);

    for (int i = 0; i < 8; ++i) dev.ioRead(nullptr, 0xD768 + i, &qb[i]);
    qlo = qb[0] | (uint64_t(qb[1]) << 8) | (uint64_t(qb[2]) << 16) | (uint64_t(qb[3]) << 24);
    qhi = qb[4] | (uint64_t(qb[5]) << 8) | (uint64_t(qb[6]) << 16) | (uint64_t(qb[7]) << 24);
    quot = qlo | (qhi << 32);

    ASSERT_EQ(quot & 0xFFFF, 0xFFFF);
}

TEST_CASE(mega65_chips_sid_dispatch) {
    SidPair sid;
    uint8_t v1 = 0, v2 = 0;

    sid.reset();

    sid.ioWrite(nullptr, 0xD404, 0x41);
    sid.ioWrite(nullptr, 0xD424, 0x82);

    sid.ioRead(nullptr, 0xD404, &v1);
    sid.ioRead(nullptr, 0xD424, &v2);

    ASSERT_EQ(v1, 0x41);
    ASSERT_EQ(v2, 0x82);
}

TEST_CASE(mega65_chips_dma_mix) {
    SparseMemoryBus sparseBus{"sparse", 28};
    F018bDmaDevice dma{0xD700};
    dma.setDmaBus(&sparseBus);

    // Pre-allocate pages
    for (uint32_t i = 0; i < 0x40000; i += 0x1000)
        sparseBus.read8(i);

    // Source: $10000 = 0xF0, Dest: $20000 = 0x0F
    sparseBus.write8(0x10000, 0xF0);
    sparseBus.write8(0x20000, 0x0F);

    // MIX with minterm = OR (bits 5,6,7 set → m1=FF, m2=FF, m3=FF, m0=00)
    // OR: result = src | dst = 0xF0 | 0x0F = 0xFF
    // Command: operation=01 (MIX), minterm bits in command byte 4-7
    //   bit4=0 (~s&~d=0), bit5=1 (~s&d=FF), bit6=1 (s&~d=FF), bit7=1 (s&d=FF)
    //   command = 0x01 | (0b1110 << 4) = 0x01 | 0xE0 = 0xE1
    uint32_t listAddr = 0x2000;
    sparseBus.write8(listAddr + 0, 0xE1);      // MIX + OR minterm
    sparseBus.write8(listAddr + 1, 0x01);      // Count = 1
    sparseBus.write8(listAddr + 2, 0x00);
    sparseBus.write8(listAddr + 3, 0x00);      // Src $10000
    sparseBus.write8(listAddr + 4, 0x00);
    sparseBus.write8(listAddr + 5, 0x01);      // Src bank
    sparseBus.write8(listAddr + 6, 0x00);      // Dst $20000
    sparseBus.write8(listAddr + 7, 0x00);
    sparseBus.write8(listAddr + 8, 0x02);      // Dst bank
    sparseBus.write8(listAddr + 9, 0x00);
    sparseBus.write8(listAddr + 10, 0x00);

    dma.ioWrite(&sparseBus, 0xD701, 0x20);
    dma.ioWrite(&sparseBus, 0xD700, 0x00);

    for (int i = 0; i < 10; ++i) dma.tick(1);

    ASSERT_EQ((int)sparseBus.read8(0x20000), 0xFF); // 0xF0 | 0x0F

    // Test AND minterm: bit7=1 (s&d=FF), rest=0 → command = 0x81
    sparseBus.write8(0x10000, 0xAA);
    sparseBus.write8(0x20000, 0x55);

    sparseBus.write8(listAddr + 0, 0x81);      // MIX + AND minterm
    dma.ioWrite(&sparseBus, 0xD701, 0x20);
    dma.ioWrite(&sparseBus, 0xD700, 0x00);

    for (int i = 0; i < 10; ++i) dma.tick(1);

    ASSERT_EQ((int)sparseBus.read8(0x20000), 0x00); // 0xAA & 0x55 = 0x00
}

TEST_CASE(mega65_chips_dma_irq_on_done) {
    SparseMemoryBus sparseBus{"sparse", 28};
    F018bDmaDevice dma{0xD700};
    dma.setDmaBus(&sparseBus);
    MockIrqLine irq;
    dma.setIrqLine(&irq);

    // Pre-allocate pages
    for (uint32_t i = 0; i < 0x40000; i += 0x1000)
        sparseBus.read8(i);

    // Fill 4 bytes at $30000 with $42, with IRQ on completion (bit 3 set)
    uint32_t listAddr = 0x2000;
    sparseBus.write8(listAddr + 0, 0x0B);      // Fill (0x03) + IRQ (0x08)
    sparseBus.write8(listAddr + 1, 0x04);      // Count = 4
    sparseBus.write8(listAddr + 2, 0x00);
    sparseBus.write8(listAddr + 3, 0x42);      // Fill byte
    sparseBus.write8(listAddr + 4, 0x00);
    sparseBus.write8(listAddr + 5, 0x00);
    sparseBus.write8(listAddr + 6, 0x00);
    sparseBus.write8(listAddr + 7, 0x00);
    sparseBus.write8(listAddr + 8, 0x03);      // Dst $30000
    sparseBus.write8(listAddr + 9, 0x00);
    sparseBus.write8(listAddr + 10, 0x00);

    ASSERT(!irq.m_level);

    dma.ioWrite(&sparseBus, 0xD701, 0x20);
    dma.ioWrite(&sparseBus, 0xD700, 0x00);

    // Tick until DMA completes
    for (int i = 0; i < 20; ++i) dma.tick(1);

    // IRQ should have fired
    ASSERT(irq.m_level);
    ASSERT(!dma.isHaltRequested());

    // Verify fill worked
    for (int i = 0; i < 4; ++i)
        ASSERT_EQ((int)sparseBus.read8(0x30000 + i), 0x42);
}
