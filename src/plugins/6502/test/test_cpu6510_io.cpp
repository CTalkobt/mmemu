#include "test_harness.h"
#include "plugins/6502/main/cpu6510.h"
#include "libmem/main/memory_bus.h"

TEST_CASE(cpu6510_io_port_logic) {
    MOS6510 cpu;
    FlatMemoryBus bus("system", 16);
    cpu.setDataBus(&bus);
    
    // Default state: DDR=0, DATA=0x3F. Effective level should be all 1s (pull-ups).
    // Bits 6-7 are hardcoded to 1.
    EXPECT_EQ(cpu.portRead(), 0xFF);
    
    // Set all as outputs
    cpu.ddrWrite(0x3F);
    cpu.dataWrite(0x00);
    // Port should read 0xC0 (bits 0-5 are 0, bits 6-7 are 1)
    EXPECT_EQ(cpu.portRead(), 0xC0);
    
    cpu.dataWrite(0x15); // 010101
    EXPECT_EQ(cpu.portRead(), 0xD5);
    
    // Test signal mapping
    // Bit 0 = LORAM, Bit 1 = HIRAM, Bit 2 = CHAREN
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
