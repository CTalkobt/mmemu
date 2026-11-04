#include "test_harness.h"
#include "plugins/cbm-hle/main/kernal_hle.h"
#include "plugins/6502/main/cpu6502.h"
#include "libmem/main/memory_bus.h"
#include <fstream>
#include <filesystem>
#include <iterator>
#include <vector>

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

namespace {
// Set up a KERNAL SAVE of $C000-$C003 (A = $FB holds the start, X/Y = end + 1)
void setupSave(MOS6502& cpu, FlatMemoryBus& bus, const std::string& name) {
    bus.write8(0xBA, 9);
    bus.write8(0xB9, 1);
    bus.write8(0xB7, (uint8_t)name.size());
    bus.write8(0xBB, 0x00);
    bus.write8(0xBC, 0x02);
    for (size_t i = 0; i < name.size(); ++i) bus.write8(0x0200 + i, name[i]);
    bus.write8(0xFB, 0x00);
    bus.write8(0xFC, 0xC0);
    cpu.regWriteByName("A", 0xFB);
    cpu.regWriteByName("X", 0x04);
    cpu.regWriteByName("Y", 0xC0);
    cpu.regWriteByName("SP", 0xFD);
    bus.write8(0x01FE, 0x00);
    bus.write8(0x01FF, 0x10);
}
std::vector<uint8_t> slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
}

TEST_CASE(kernal_hle_save_writes_host_file) {
    std::string dir = "hle_save_test_dir";
    fs::create_directories(dir);
    MOS6502 cpu;
    FlatMemoryBus bus("system", 16);
    cpu.setDataBus(&bus);
    KernalHLE hle;
    hle.setHostPath(dir);
    for (int i = 0; i < 4; i++) bus.write8(0xC000 + i, (uint8_t)(0xA0 + i));

    setupSave(cpu, bus, "PROG");
    ASSERT(hle.handle(cpu, bus, 0xFFD8));
    ASSERT_EQ((int)(cpu.regReadByName("P") & 1), 0);
    ASSERT_EQ((int)cpu.pc(), 0x1001);
    auto f = slurp(dir + "/PROG");
    ASSERT_EQ((int)f.size(), 6);
    ASSERT_EQ((int)f[0], 0x00);
    ASSERT_EQ((int)f[1], 0xC0);
    ASSERT_EQ((int)f[2], 0xA0);
    ASSERT_EQ((int)f[5], 0xA3);

    // An existing file is not replaced without "@:" ...
    bus.write8(0xC000, 0x55);
    setupSave(cpu, bus, "PROG");
    ASSERT(hle.handle(cpu, bus, 0xFFD8));
    ASSERT_EQ((int)(cpu.regReadByName("P") & 1), 0);
    ASSERT_EQ((int)slurp(dir + "/PROG")[2], 0xA0);
    // ... and is with it
    setupSave(cpu, bus, "@0:PROG");
    ASSERT(hle.handle(cpu, bus, 0xFFD8));
    ASSERT_EQ((int)slurp(dir + "/PROG")[2], 0x55);

    // No name: KERNAL error 8 (MISSING FILE NAME)
    setupSave(cpu, bus, "");
    ASSERT(hle.handle(cpu, bus, 0xFFD8));
    ASSERT_EQ((int)(cpu.regReadByName("P") & 1), 1);
    ASSERT_EQ((int)cpu.regReadByName("A"), 8);
    fs::remove_all(dir);
}
