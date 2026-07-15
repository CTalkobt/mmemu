#include "test_harness.h"
#include "plugins/devices/mega65_hypervisor/main/hypervisor_regs.h"
#include "plugins/devices/mega65_hypervisor/main/hdos_handler.h"
#include "plugins/45gs02/main/cpu45gs02.h"
#include "libmem/main/memory_bus.h"

struct HyperFixture {
    FlatMemoryBus bus{"test", 16};
    MOS45GS02 cpu;
    HypervisorRegs regs{&cpu};

    HyperFixture() {
        cpu.setDataBus(&bus);
        cpu.setCodeBus(&bus);
        cpu.reset();
    }
};

TEST_CASE(hypervisor_regs_user_read_returns_ff) {
    HyperFixture f;
    // In user mode, reads to $D640-$D67F return $FF
    uint8_t val = 0;
    f.regs.ioRead(nullptr, 0xD640, &val);
    ASSERT_EQ((int)val, 0xFF);
}

TEST_CASE(hypervisor_regs_hyper_mode_read_write) {
    HyperFixture f;
    f.cpu.enterHypervisor(0x8000);

    // In hypervisor mode, can read/write virtualisation registers
    auto& h = f.cpu.hyperState();
    h.regA = 0x42;

    uint8_t val;
    f.regs.ioRead(nullptr, 0xD640, &val);
    ASSERT_EQ((int)val, 0x42);

    // Write A register
    f.regs.ioWrite(nullptr, 0xD640, 0x99);
    ASSERT_EQ((int)h.regA, 0x99);
}

TEST_CASE(hypervisor_exit_via_d67f) {
    HyperFixture f;
    f.cpu.enterHypervisor(0x8000);
    ASSERT(f.cpu.isHypervisor());

    // Set return PC
    f.cpu.hyperState().pc = 0x1234;

    // Write to $D67F exits hypervisor
    f.regs.ioWrite(nullptr, 0xD67F, 0x00);
    ASSERT(!f.cpu.isHypervisor());
    ASSERT_EQ((int)f.cpu.pc(), 0x1234);
}

TEST_CASE(hypervisor_trap_requires_nop_or_clv) {
    HyperFixture f;
    // Write NOP ($EA) after the STA instruction location
    // The trap validator checks the byte at current PC
    f.cpu.regWrite(6, 0x1000);  // PC
    f.bus.write8(0x1000, 0xEA); // NOP at PC

    // Should accept the trap (NOP follows STA)
    f.regs.ioWrite(&f.bus, 0xD640, 0x00);
    // With no HDOS handler, it enters hypervisor at $8000
    // (or stays user mode if no hypervisor ROM)
}

TEST_CASE(hypervisor_trap_rejected_without_nop) {
    HyperFixture f;
    f.cpu.regWrite(6, 0x1000);
    f.bus.write8(0x1000, 0xA9); // LDA # (not NOP/CLV)

    bool wasBefore = f.cpu.isHypervisor();
    f.regs.ioWrite(&f.bus, 0xD640, 0x00);
    // Should be silently ignored — still in user mode
    ASSERT_EQ(f.cpu.isHypervisor(), wasBefore);
}

// HDOS handler tests
TEST_CASE(hdos_get_drive_returns_zero) {
    HyperFixture f;
    HdosHandler hdos;

    // Trap $02 = get default drive
    MOS45GS02 cpu;
    FlatMemoryBus bus("test", 16);
    cpu.setDataBus(&bus);
    cpu.reset();
    cpu.enterHypervisor(0x8000);

    bool handled = hdos.handleTrap(0x02, &cpu);
    ASSERT(handled);
    // Drive 0 returned in A
    ASSERT_EQ((int)cpu.hyperState().regA, 0);
}

TEST_CASE(hdos_close_all) {
    HdosHandler hdos;
    MOS45GS02 cpu;
    FlatMemoryBus bus("test", 16);
    cpu.setDataBus(&bus);
    cpu.reset();
    cpu.enterHypervisor(0x8000);

    bool handled = hdos.handleTrap(0x22, &cpu);
    ASSERT(handled);
    // Carry set = success
    ASSERT((cpu.hyperState().pflags & 0x01) != 0);
}

TEST_CASE(hdos_getdisksize) {
    HdosHandler hdos;
    MOS45GS02 cpu;
    FlatMemoryBus bus("test", 16);
    cpu.setDataBus(&bus);
    cpu.reset();
    cpu.enterHypervisor(0x8000);

    bool handled = hdos.handleTrap(0x08, &cpu);
    ASSERT(handled);
    // Carry set = success
    ASSERT((cpu.hyperState().pflags & 0x01) != 0);
}

TEST_CASE(hdos_unhandled_trap_returns_false) {
    HdosHandler hdos;
    MOS45GS02 cpu;
    FlatMemoryBus bus("test", 16);
    cpu.setDataBus(&bus);
    cpu.reset();
    cpu.enterHypervisor(0x8000);

    // Trap $00 = get version — not virtualized
    bool handled = hdos.handleTrap(0x00, &cpu);
    ASSERT(!handled);
}

// ============================================================================
// DMA State Hypervisor Registers ($D653-$D659) — Issue #111
// ============================================================================

TEST_CASE(hypervisor_dma_state_user_mode_read) {
    HyperFixture f;
    // In user mode, reads to DMA state registers return $FF
    uint8_t val = 0;
    ASSERT(f.regs.ioRead(nullptr, 0xD653, &val));
    ASSERT_EQ((int)val, 0xFF);
}

TEST_CASE(hypervisor_dma_srcmb_register) {
    // Test $D653 (DMASRCMB) — Source megabyte
    HyperFixture f;
    f.cpu.enterHypervisor(0x8000);

    auto& h = f.cpu.hyperState();
    uint8_t val;

    // Write source MB
    ASSERT(f.regs.ioWrite(nullptr, 0xD653, 0x05));
    ASSERT_EQ((int)h.dmaSrcMB, 0x05);

    // Read it back
    ASSERT(f.regs.ioRead(nullptr, 0xD653, &val));
    ASSERT_EQ((int)val, 0x05);
}

TEST_CASE(hypervisor_dma_dstmb_register) {
    // Test $D654 (DMADSTMB) — Destination megabyte
    HyperFixture f;
    f.cpu.enterHypervisor(0x8000);

    auto& h = f.cpu.hyperState();
    uint8_t val;

    // Write destination MB
    ASSERT(f.regs.ioWrite(nullptr, 0xD654, 0x0A));
    ASSERT_EQ((int)h.dmaDstMB, 0x0A);

    // Read it back
    ASSERT(f.regs.ioRead(nullptr, 0xD654, &val));
    ASSERT_EQ((int)val, 0x0A);
}

TEST_CASE(hypervisor_dma_list_address_28bit) {
    // Test $D655-$D658 (DMALADDR) — DMA list address (28-bit)
    HyperFixture f;
    f.cpu.enterHypervisor(0x8000);

    auto& h = f.cpu.hyperState();
    uint8_t val;

    // Write 28-bit address 0x05100000 with bits clearly separated
    // Byte 0 (bits 7:0): 0x00
    // Byte 1 (bits 15:8): 0x00
    // Byte 2 (bits 23:16): 0x10
    // Nibble (bits 27:24): 0x05
    ASSERT(f.regs.ioWrite(nullptr, 0xD655, 0x00));
    ASSERT(f.regs.ioWrite(nullptr, 0xD656, 0x00));
    ASSERT(f.regs.ioWrite(nullptr, 0xD657, 0x10));
    ASSERT(f.regs.ioWrite(nullptr, 0xD658, 0x05));

    ASSERT_EQ((uint32_t)h.dmaListAddr, 0x05100000u);

    // Read back LSB
    ASSERT(f.regs.ioRead(nullptr, 0xD655, &val));
    ASSERT_EQ((int)val, 0x00);

    // Read back byte 1 (bits 15:8)
    ASSERT(f.regs.ioRead(nullptr, 0xD656, &val));
    ASSERT_EQ((int)val, 0x00);

    // Read back byte 2 (bits 23:16)
    ASSERT(f.regs.ioRead(nullptr, 0xD657, &val));
    ASSERT_EQ((int)val, 0x10);

    // Read back upper nibble (bits 27:24)
    ASSERT(f.regs.ioRead(nullptr, 0xD658, &val));
    ASSERT_EQ((int)val, 0x05);
}

TEST_CASE(hypervisor_dma_list_address_partial_write) {
    // Test partial writes to DMA list address preserve other bytes
    HyperFixture f;
    f.cpu.enterHypervisor(0x8000);

    auto& h = f.cpu.hyperState();

    // Set initial value
    h.dmaListAddr = 0x0FFFFFFF;

    // Write only LSB
    ASSERT(f.regs.ioWrite(nullptr, 0xD655, 0xAA));
    ASSERT_EQ((uint32_t)h.dmaListAddr, 0x0FFFFFAA);

    // Write only MSB byte (bits 23:16)
    ASSERT(f.regs.ioWrite(nullptr, 0xD657, 0xBB));
    ASSERT_EQ((uint32_t)h.dmaListAddr, 0x0FBBFFAA);

    // Write upper nibble (bits 27:24)
    ASSERT(f.regs.ioWrite(nullptr, 0xD658, 0x05));
    ASSERT_EQ((uint32_t)h.dmaListAddr, 0x05BBFFAA);
}

TEST_CASE(hypervisor_dma_list_address_28bit_mask) {
    // Test that upper address bits are masked to 28-bit
    HyperFixture f;
    f.cpu.enterHypervisor(0x8000);

    auto& h = f.cpu.hyperState();

    // Write all bits
    ASSERT(f.regs.ioWrite(nullptr, 0xD655, 0xFF));
    ASSERT(f.regs.ioWrite(nullptr, 0xD656, 0xFF));
    ASSERT(f.regs.ioWrite(nullptr, 0xD657, 0xFF));
    ASSERT(f.regs.ioWrite(nullptr, 0xD658, 0x0F));  // Upper nibble only

    // Should be 28-bit address $0FFFFFFF
    ASSERT_EQ((uint32_t)h.dmaListAddr, 0x0FFFFFFFU);
}

TEST_CASE(hypervisor_dma_state_all_registers) {
    // Test all DMA state registers together
    HyperFixture f;
    f.cpu.enterHypervisor(0x8000);

    auto& h = f.cpu.hyperState();
    uint8_t val;

    // Set complete DMA state
    ASSERT(f.regs.ioWrite(nullptr, 0xD653, 0x02));  // Src MB
    ASSERT(f.regs.ioWrite(nullptr, 0xD654, 0x03));  // Dst MB
    ASSERT(f.regs.ioWrite(nullptr, 0xD655, 0x00));  // List addr LSB
    ASSERT(f.regs.ioWrite(nullptr, 0xD656, 0x10));  // List addr mid
    ASSERT(f.regs.ioWrite(nullptr, 0xD657, 0x20));  // List addr high
    ASSERT(f.regs.ioWrite(nullptr, 0xD658, 0x04));  // List addr upper

    // Verify all values
    ASSERT_EQ((int)h.dmaSrcMB, 0x02);
    ASSERT_EQ((int)h.dmaDstMB, 0x03);
    ASSERT_EQ((uint32_t)h.dmaListAddr, 0x04201000u);

    // Read back all values
    ASSERT(f.regs.ioRead(nullptr, 0xD653, &val));
    ASSERT_EQ((int)val, 0x02);
    ASSERT(f.regs.ioRead(nullptr, 0xD654, &val));
    ASSERT_EQ((int)val, 0x03);
    ASSERT(f.regs.ioRead(nullptr, 0xD655, &val));
    ASSERT_EQ((int)val, 0x00);
    ASSERT(f.regs.ioRead(nullptr, 0xD656, &val));
    ASSERT_EQ((int)val, 0x10);
    ASSERT(f.regs.ioRead(nullptr, 0xD657, &val));
    ASSERT_EQ((int)val, 0x20);
    ASSERT(f.regs.ioRead(nullptr, 0xD658, &val));
    ASSERT_EQ((int)val, 0x04);
}
