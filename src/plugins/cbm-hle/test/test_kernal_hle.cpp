#include "test_harness.h"
#include "plugins/cbm-hle/main/kernal_hle.h"
#include "plugins/6502/main/cpu6502.h"
#include "libmem/main/memory_bus.h"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

TEST_CASE(kernal_hle_load) {
    MOS6502 cpu;
    FlatMemoryBus bus("system", 16);
    cpu.setDataBus(&bus);
    
    KernalHLE hle;
    hle.setHostPath(".");
    
    // Create a dummy file
    std::string filename = "test.prg";
    {
        std::ofstream f(filename, std::ios::binary);
        uint8_t header[] = { 0x00, 0x20 }; // Load at $2000
        f.write((char*)header, 2);
        uint8_t data[] = { 0xAD, 0x00, 0x20, 0x60 }; // LDA $2000, RTS
        f.write((char*)data, sizeof(data));
    }
    
    // Set up KERNAL variables in RAM
    bus.write8(0xBA, 8); // Device 8
    bus.write8(0xB9, 1); // Secondary Address 1 (use header)
    bus.write8(0xB7, filename.length()); // Filename length
    uint16_t namePtr = 0x0200;
    bus.write8(0xBB, namePtr & 0xFF);
    bus.write8(0xBC, namePtr >> 8);
    for (size_t i = 0; i < filename.length(); ++i) {
        bus.write8(namePtr + i, filename[i]);
    }
    
    // Set up stack for RTS simulation
    cpu.regWriteByName("SP", 0xFD);
    bus.write8(0x01FE, 0x00); // Return address lo
    bus.write8(0x01FF, 0x10); // Return address hi ($1000)
    
    // Run the LOAD intercept at $FFD5
    ASSERT(hle.handle(cpu, bus, 0xFFD5));
    
    // Verify data injected at $2000
    ASSERT(bus.read8(0x2000) == 0xAD);
    ASSERT(bus.read8(0x2001) == 0x00);
    ASSERT(bus.read8(0x2002) == 0x20);
    ASSERT(bus.read8(0x2003) == 0x60);
    
    // Verify registers updated
    // New X/Y should be end address ($2004)
    ASSERT(cpu.regReadByName("X") == 0x04);
    ASSERT(cpu.regReadByName("Y") == 0x20);
    
    // Verify Carry flag cleared (success)
    ASSERT((cpu.regReadByName("P") & 0x01) == 0);
    
    // Verify PC updated to return address ($1000 + 1 = $1001)
    ASSERT(cpu.pc() == 0x1001);
    
    // Cleanup
    fs::remove(filename);
}

// Secondary address 0 loads to X/Y and still skips the file's load address
TEST_CASE(kernal_hle_load_sa0_skips_header) {
    MOS6502 cpu;
    FlatMemoryBus bus("system", 16);
    cpu.setDataBus(&bus);
    KernalHLE hle;
    hle.setHostPath(".");
    std::string filename = "test_sa0.prg";
    {
        std::ofstream f(filename, std::ios::binary);
        uint8_t bytes[] = { 0x01, 0x08, 0x11, 0x22, 0x33 };
        f.write((char*)bytes, sizeof bytes);
    }
    bus.write8(0xBA, 9);
    bus.write8(0xB9, 0);
    bus.write8(0xB7, filename.length());
    bus.write8(0xBB, 0x00);
    bus.write8(0xBC, 0x02);
    for (size_t i = 0; i < filename.length(); ++i) bus.write8(0x0200 + i, filename[i]);
    cpu.regWriteByName("X", 0x00);
    cpu.regWriteByName("Y", 0x30);
    cpu.regWriteByName("SP", 0xFD);
    ASSERT(hle.handle(cpu, bus, 0xFFD5));
    ASSERT_EQ((int)bus.read8(0x3000), 0x11);
    ASSERT_EQ((int)bus.read8(0x3002), 0x33);
    ASSERT_EQ((int)cpu.regReadByName("X"), 0x03);
    fs::remove(filename);
}

// Units below 8 (tape) are left to the KERNAL
TEST_CASE(kernal_hle_leaves_tape_to_kernal) {
    MOS6502 cpu;
    FlatMemoryBus bus("system", 16);
    cpu.setDataBus(&bus);
    KernalHLE hle;
    bus.write8(0xBA, 1);
    ASSERT(!hle.handle(cpu, bus, 0xFFD5));
}
