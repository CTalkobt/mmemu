# Hardware Validation & Cross-Validation Testing

## Overview

The **Hardware Test Runner Bridge** provides a unified interface to run identical test programs on both the mmsim emulator and real MEGA65 hardware, enabling comprehensive cross-validation to verify emulation accuracy.

## Architecture

### Components

#### 1. HardwareTestBridge (Abstract Base)
Provides unified API for communicating with MEGA65 systems via the serial monitor protocol.

**Implementations:**
- **EmulatorTestBridge** — TCP connection to `SerialMonitorServer`
  - Connects to localhost:6502 by default
  - Used when testing emulator accuracy
  
- **HardwarePortBridge** — Direct serial port (USB-UART adapter)
  - Connects to `/dev/ttyUSB0` or other serial device
  - Used for testing real MEGA65 hardware

#### 2. CrossValidationRunner
High-level orchestration layer for running tests on one or both targets, with automatic result comparison.

### Serial Monitor Protocol

The bridge implements the MEGA65 serial monitor text protocol:

| Command | Syntax | Purpose |
|---------|--------|---------|
| **M** | `M addr [size]` | Read memory region (hex output) |
| **S** | `S addr value` | Write single byte to memory |
| **R** | `R` | Read all CPU registers (format: `A:XX X:XX Y:XX PC:XXXX SP:XX SR:XX`) |
| **D** | `D addr [count]` | Disassemble N instructions from address |
| **G** | `G addr` | Set program counter (jump to address) |
| **T** | `T` | Single-step CPU by one instruction |

## Usage

### Basic Emulator Testing

```cpp
#include "cli/main/hardware_test_bridge.h"
#include "cli/main/cross_validation_runner.h"

// Connect to running emulator
auto bridge = HardwareTestBridge::connectEmulator("127.0.0.1", 6502);
if (!bridge->isConnected()) {
    // Emulator not running
}

// Load test program
bridge->loadMemoryFile(0x0800, "test.bin");

// Execute test
auto result = bridge->runTest(
    0x0800,   // Program address
    0x2000,   // Result memory address
    256,      // Result size (bytes)
    5000      // Timeout (ms)
);

if (result.success) {
    printf("Test passed, execution took %llu cycles\n", result.executionCycles);
    // Access result memory
    for (auto byte : result.memorySnapshot) {
        printf("%02X ", byte);
    }
}
```

### Hardware Testing

```cpp
// Connect to real MEGA65 via serial port
auto bridge = HardwareTestBridge::connectHardware("/dev/ttyUSB0", 2000000);

// Same API as emulator
bridge->loadMemoryFile(0x0800, "test.bin");
auto result = bridge->runTest(0x0800, 0x2000, 256, 5000);
```

### Cross-Validation

```cpp
// Create runner connecting to both emulator and hardware
auto runner = CrossValidationRunner::withBoth(
    "127.0.0.1", 6502,              // Emulator
    "/dev/ttyUSB0", 2000000         // Hardware
);

if (!runner->canCrossValidate()) {
    fprintf(stderr, "Both emulator and hardware required for cross-validation\n");
    return;
}

// Define test cases
std::vector<CrossValidationRunner::TestCase> tests = {
    {
        .name = "arithmetic",
        .programPath = "tests/arithmetic.bin",
        .programAddr = 0x0800,
        .resultAddr = 0x2000,
        .resultSize = 256,
        .timeoutMs = 5000
    },
    {
        .name = "dma_copy",
        .programPath = "tests/dma_copy.bin",
        .programAddr = 0x0800,
        .resultAddr = 0x2000,
        .resultSize = 512,
        .timeoutMs = 10000
    }
};

// Run all tests and compare results
auto results = runner->runTests(tests);

// Analyze results
for (const auto& [name, result] : results) {
    if (result.overallPass()) {
        printf("✓ %s: PASS (emulator and hardware match)\n", name.c_str());
    } else if (!result.emulatorPass) {
        printf("✗ %s: Emulator failed: %s\n", name.c_str(), result.emulatorError.c_str());
    } else if (!result.hardwarePass) {
        printf("✗ %s: Hardware failed: %s\n", name.c_str(), result.hardwareError.c_str());
    } else if (!result.resultsMatch) {
        printf("✗ %s: Results differ (emulator vs hardware)\n", name.c_str());
    }
}
```

## Test Structure

### Expected Test Program Format

Test programs should:
1. Be loaded at a known address (typically 0x0800)
2. Write results to a known memory region (e.g., 0x2000-0x20FF)
3. Halt or loop when complete (detected by timeout or breakpoint)
4. Optionally write status to serial port ($D6C1) for logging

### Example Test Program (6502 ASM)

```asm
* = $0800
    ; Test: Add two numbers and store result
    LDA #$42        ; Load 0x42 into accumulator
    ADC #$08        ; Add 0x08
    STA $2000       ; Store result at 0x2000
    
    ; Write completion marker
    LDA #$00
    STA $D6C1       ; Write to serial port
    
    JMP *           ; Halt (infinite loop)
```

## Hardware Setup

### Required Hardware

- MEGA65 with working serial monitor support
- USB-to-UART adapter (FTDI or CP2102 recommended)
- USB cable to connect adapter to development machine

### Wiring (JTAG/Serial Connector)

| Pin | Function |
|-----|----------|
| 1   | GND |
| 2   | RX (from MEGA65) → UART RX |
| 3   | TX (to MEGA65) → UART TX |
| 4   | +5V (optional, if adapter powered by MEGA65) |

### Driver Installation (Linux)

Most systems auto-detect FTDI/CP2102 adapters:

```bash
# Check if device is recognized
ls -l /dev/ttyUSB*

# Grant user access (if needed)
sudo usermod -a -G dialout $USER
# Logout and login to take effect
```

## Integration with Existing Tests

The bridge infrastructure is available in test code:

```cpp
#include "cli/main/cross_validation_runner.h"

TEST_CASE(my_cross_validation_test) {
    auto runner = CrossValidationRunner::withBoth();
    
    if (runner->hasEmulator()) {
        // Run tests against emulator
    }
    
    if (runner->hasHardware()) {
        // Run tests against hardware
    }
}
```

## API Reference

### HardwareTestBridge

```cpp
class HardwareTestBridge {
    // Factory methods
    static std::unique_ptr<HardwareTestBridge> 
        connectEmulator(const std::string& host = "127.0.0.1", uint16_t port = 6502);
    
    static std::unique_ptr<HardwareTestBridge> 
        connectHardware(const std::string& portPath, uint32_t baudRate = 2000000);
    
    // Connection management
    bool isConnected() const;
    Mode getMode() const;
    
    // Memory operations
    bool loadMemory(uint32_t addr, const std::vector<uint8_t>& data);
    bool loadMemoryFile(uint32_t addr, const std::string& filePath);
    std::vector<uint8_t> readMemory(uint32_t addr, uint32_t size);
    bool writeMemory(uint32_t addr, uint8_t value);
    
    // CPU control
    bool setPC(uint32_t addr);
    uint32_t readRegister(const std::string& regName);  // A, X, Y, PC, SP, SR
    bool step(int count = 1);
    bool run();
    
    // Test execution
    struct TestResult {
        bool success;
        std::string error;
        uint64_t executionCycles;
        std::vector<uint8_t> memorySnapshot;
        std::string output;
    };
    
    TestResult runTest(uint32_t programAddr, uint32_t resultAddr, 
                       uint32_t resultSize, uint32_t timeoutMs = 5000);
    
    // Serial communication
    std::string getSerialOutput() const;
    void clearSerialOutput();
    uint64_t getCycles() const;  // Emulator only
};
```

### CrossValidationRunner

```cpp
class CrossValidationRunner {
    struct TestCase {
        std::string name;
        std::string programPath;
        uint32_t programAddr = 0x0800;
        uint32_t resultAddr = 0x2000;
        uint32_t resultSize = 256;
        uint32_t timeoutMs = 5000;
    };
    
    struct ComparisonResult {
        std::string testName;
        bool emulatorPass = false;
        bool hardwarePass = false;
        bool resultsMatch = false;
        
        std::string emulatorError;
        std::string hardwareError;
        std::vector<uint8_t> emulatorMemory;
        std::vector<uint8_t> hardwareMemory;
        std::string emulatorOutput;
        std::string hardwareOutput;
        
        bool overallPass() const;
    };
    
    // Factory methods
    static std::unique_ptr<CrossValidationRunner> 
        withEmulator(const std::string& host = "127.0.0.1", uint16_t port = 6502);
    
    static std::unique_ptr<CrossValidationRunner> 
        withHardware(const std::string& portPath, uint32_t baudRate = 2000000);
    
    static std::unique_ptr<CrossValidationRunner> 
        withBoth(const std::string& emuHost = "127.0.0.1", uint16_t emuPort = 6502,
                 const std::string& hwPort = "", uint32_t hwBaudRate = 2000000);
    
    // Test execution
    ComparisonResult runTest(const TestCase& test);
    std::map<std::string, ComparisonResult> runTests(const std::vector<TestCase>& tests);
    
    // Capability queries
    bool canCrossValidate() const;
    bool hasEmulator() const;
    bool hasHardware() const;
};
```

## Debugging Failed Tests

### Emulator-Only Failures

If a test passes on real hardware but fails in the emulator, the discrepancy indicates an emulation bug:

1. **Check CPU state**: Use `readRegister()` to inspect registers after test
2. **Memory contents**: Compare result regions byte-by-byte
3. **Timing**: Check if `executionCycles` matches expected instruction count
4. **Serial output**: Examine `getSerialOutput()` for debug messages

### Hardware-Only Failures

If a test passes in the emulator but fails on hardware:

1. **Verify connection**: Confirm serial cable is secure and baud rate matches
2. **Check ROM/loader**: Verify MEGA65's ROM and boot loader are functional
3. **Timeout issues**: Increase `timeoutMs` if hardware is slower
4. **Memory access**: Verify test doesn't access protected or invalid memory regions

### Divergent Results

If both pass but memory snapshots differ:

1. Run with `resultSize` larger to capture more context
2. Add intermediate result checkpoints during test program
3. Enable serial logging to correlate emulator vs hardware execution

## Future Enhancements

- [ ] Windows serial port support (currently Linux only)
- [ ] Timeout detection via hardware breakpoint (instead of elapsed time)
- [ ] Binary diff visualization for large memory regions
- [ ] Regression test tracking (store historical results)
- [ ] Performance profiling (track cycle count trends)
- [ ] Snapshot/restore for multi-segment tests

## References

- [MEGA65 Serial Monitor Protocol](https://github.com/MEGA65/mega65-core/wiki/Serial-Monitor)
- [mmsim CLI Documentation](README.md)
- [Cross-Validation Tests](tests/src/test_cross_validation.cpp)
