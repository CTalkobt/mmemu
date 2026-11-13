#include "test_harness.h"
#include "plugins/6502/main/cpu6510.h"
#include "libmem/main/memory_bus.h"

TEST_CASE(cpu6510_io_port_logic) {
    MOS6510 cpu;
    FlatMemoryBus bus("system", 16);
    cpu.setDataBus(&bus);
    
    // Default state: DDR=0, DATA=0x3F, as VICE: bits 0-2 and 4 pulled up,
    // bit 3 what was last driven (1), bit 5 low as an input, bits 6-7
    // (unconnected) uncharged
    EXPECT_EQ(cpu.portRead(), 0x1F);
    EXPECT_EQ(cpu.getPortBus()->read8(0x0000), 0x00);

    // Set all as outputs
    cpu.ddrWrite(0x3F);
    cpu.dataWrite(0x00);
    EXPECT_EQ(cpu.portRead(), 0x00);

    cpu.dataWrite(0x15); // 010101
    EXPECT_EQ(cpu.portRead(), 0x15);

    // All 8 DDR bits read back; bits 6-7 as outputs charge their pins, and
    // keep the charge as inputs (for a while: VICE's 350000 cycles)
    cpu.ddrWrite(0xFF);
    EXPECT_EQ(cpu.getPortBus()->read8(0x0000), 0xFF);
    cpu.dataWrite(0xD5);
    EXPECT_EQ(cpu.portRead(), 0xD5);
    cpu.ddrWrite(0x3F);
    EXPECT_EQ(cpu.portRead() & 0xC0, 0xC0);
    cpu.ddrWrite(0x00);                  // bit 3 keeps 0, bit 5 reads 0
    EXPECT_EQ(cpu.portRead(), 0xD7);
    
    // Test signal mapping
    // Bit 0 = LORAM, Bit 1 = HIRAM, Bit 2 = CHAREN
    cpu.ddrWrite(0x3F);
    cpu.dataWrite(0x00); // All signals low
    EXPECT_EQ(cpu.getSignalLine("loram")->get(), false);
    EXPECT_EQ(cpu.getSignalLine("hiram")->get(), false);
    EXPECT_EQ(cpu.getSignalLine("charen")->get(), false);
    
    cpu.dataWrite(0x07); // All signals high
    EXPECT_EQ(cpu.getSignalLine("loram")->get(), true);
    EXPECT_EQ(cpu.getSignalLine("hiram")->get(), true);
    EXPECT_EQ(cpu.getSignalLine("charen")->get(), true);
}

// Cartridges attach through the CPU's data bus (the CLI, GUI and MCP):
// the 6510's port wrapper passes ROM overlays on to the real bus
TEST_CASE(cpu6510_port_bus_forwards_rom_overlays) {
    MOS6510 cpu;
    FlatMemoryBus bus("system", 16);
    cpu.setDataBus(&bus);
    IBus* data = cpu.getDataBus();
    ASSERT(data != &bus);                          // the wrapper
    static const uint8_t rom[4] = {0x09, 0x80, 0xC3, 0xC2};
    data->addRomOverlay(0x8000, sizeof rom, rom);
    EXPECT_EQ(bus.peek8(0x8002), 0xC3);
    EXPECT_EQ(data->read8(0x8003), 0xC2);
    data->removeRomOverlay(0x8000);
    EXPECT_EQ(bus.peek8(0x8002), 0x00);
}

// A write to $00/$01 sets the port and puts the VIC-II's Phi1 bus value (not
// the CPU's data) in the RAM under it (VICE zero_store; testprogs REU/cpuport)
TEST_CASE(cpu6510_port_write_stores_phi1_in_ram) {
    MOS6510 cpu;
    FlatMemoryBus bus("system", 16);
    cpu.setDataBus(&bus);
    cpu.getPortBus()->write8(0x0001, 0x37);
    EXPECT_EQ(bus.read8(0x0001), 0x00);              // no Phi1 source: RAM left alone
    cpu.setPhi1Source([] { return 0x18; });
    cpu.getPortBus()->write8(0x0000, 0x2F);
    cpu.getPortBus()->write8(0x0001, 0x35);
    EXPECT_EQ(cpu.portDDR(), 0x2F);
    EXPECT_EQ(bus.read8(0x0000), 0x18);
    EXPECT_EQ(bus.read8(0x0001), 0x18);
}
