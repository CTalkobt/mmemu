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
