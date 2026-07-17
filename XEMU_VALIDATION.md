# Cross-Validation with xemu-xmega65

## Overview

Three-way cross-validation enables comparing test results across:
1. **mmsim** — Our MEGA65 emulator
2. **xemu-xmega65** — Alternative MEGA65 emulator (Zeda)
3. **Real MEGA65 hardware** — Actual physical system via serial

This validates emulation accuracy and identifies bugs specific to one implementation.

## Installation

### xemu-xmega65

```bash
# Ubuntu/Debian
sudo apt-get install xemu-xmega65

# Or build from source
git clone https://github.com/LGB/xemu.git
cd xemu
make
sudo make install  # Installs to /usr/local/bin/xemu-xmega65
```

### Verify Installation

```bash
which xemu-xmega65
xemu-xmega65 -h
```

## Quick Start: Compare Two Emulators

### Step 1: Assemble test program

```bash
cd /home/duck/m65/inpg/mmsim
ca45 tests/45gs02/arithmetic.s -o tests/45gs02/arithmetic.bin
```

### Step 2: Create validation script

Save as `validate_emulators.cpp`:

```cpp
#include <cstdio>
#include "cli/main/cross_validation_runner.h"

int main() {
    // Compare mmsim vs xemu-xmega65
    auto runner = CrossValidationRunner::withXemu(
        "127.0.0.1", 6502,                       // mmsim
        "/usr/local/bin/xemu-xmega65"            // xemu
    );
    
    if (!runner->hasEmulator() || !runner->hasXemu()) {
        fprintf(stderr, "Error: Emulator or xemu not available\n");
        fprintf(stderr, "- Start mmsim: ./bin/mmemu-cli -m rawMega65\n");
        fprintf(stderr, "- Install xemu: apt-get install xemu-xmega65\n");
        return 1;
    }
    
    // Define tests
    std::vector<CrossValidationRunner::TestCase> tests = {
        {
            .name = "arithmetic",
            .programPath = "tests/45gs02/arithmetic.bin",
            .programAddr = 0x2000,
            .resultAddr = 0x0400,
            .resultSize = 16,
            .timeoutMs = 5000
        },
        {
            .name = "transfers",
            .programPath = "tests/45gs02/transfers.bin",
            .programAddr = 0x2000,
            .resultAddr = 0x0400,
            .resultSize = 16,
            .timeoutMs = 5000
        },
        {
            .name = "quad",
            .programPath = "tests/45gs02/quad.bin",
            .programAddr = 0x2000,
            .resultAddr = 0x0400,
            .resultSize = 16,
            .timeoutMs = 5000
        }
    };
    
    auto results = runner->runTests(tests);
    
    // Display results
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  EMULATOR CROSS-VALIDATION (mmsim vs xemu)      ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");
    
    int matches = 0, differs = 0;
    
    for (const auto& [name, result] : results) {
        printf("Test: %s\n", name.c_str());
        printf("  mmsim:  %s\n", result.emulatorPass ? "✓ PASS" : "✗ FAIL");
        printf("  xemu:   %s\n", result.xemuPass ? "✓ PASS" : "✗ FAIL");
        
        if (result.emulatorPass && result.xemuPass) {
            if (result.resultsMatch) {
                printf("  Result: ✓ MATCH\n");
                matches++;
            } else {
                printf("  Result: ✗ DIFFER\n");
                printf("    mmsim:  %02X %02X %02X %02X\n",
                       result.emulatorMemory[0], result.emulatorMemory[1],
                       result.emulatorMemory[2], result.emulatorMemory[3]);
                printf("    xemu:   %02X %02X %02X %02X\n",
                       result.xemuMemory[0], result.xemuMemory[1],
                       result.xemuMemory[2], result.xemuMemory[3]);
                differs++;
            }
        }
        printf("\n");
    }
    
    printf("─────────────────────────────────────────────────\n");
    printf("Summary: %d match, %d differ\n", matches, differs);
    printf("═════════════════════════════════════════════════\n");
    
    return differs > 0 ? 1 : 0;
}
```

### Step 3: Compile and run

```bash
# Terminal 1: Start mmsim
./bin/mmemu-cli -m rawMega65

# Terminal 2: Assemble tests and validate
ca45 tests/45gs02/arithmetic.s -o tests/45gs02/arithmetic.bin
ca45 tests/45gs02/transfers.s -o tests/45gs02/transfers.bin
ca45 tests/45gs02/quad.s -o tests/45gs02/quad.bin

g++ -std=c++17 -I./src validate_emulators.cpp \
    -L./lib/internal -L./lib \
    -o validate_emulators \
    src/cli/main/cross_validation_runner.o \
    src/cli/main/hardware_test_bridge.o \
    -ldl -lspdlog -lfmt

./validate_emulators
```

## Three-Way Validation: All Emulators + Hardware

### Complete validation script

Save as `validate_all.cpp`:

```cpp
#include <cstdio>
#include "cli/main/cross_validation_runner.h"

int main() {
    // Three-way validation: mmsim vs xemu vs real hardware
    auto runner = CrossValidationRunner::withAll(
        "127.0.0.1", 6502,                       // mmsim
        "/usr/local/bin/xemu-xmega65",           // xemu
        "/dev/ttyUSB0", 2000000                  // real hardware
    );
    
    if (!runner->hasEmulator()) {
        fprintf(stderr, "Error: mmsim not running\n");
        fprintf(stderr, "Start with: ./bin/mmemu-cli -m rawMega65\n");
        return 1;
    }
    
    if (!runner->hasXemu()) {
        fprintf(stderr, "Error: xemu-xmega65 not installed\n");
        fprintf(stderr, "Install with: apt-get install xemu-xmega65\n");
        return 1;
    }
    
    printf("Available targets:\n");
    printf("  ✓ mmsim (Emulator)\n");
    printf("  ✓ xemu-xmega65 (Emulator)\n");
    
    if (runner->hasHardware()) {
        printf("  ✓ Real MEGA65 Hardware\n");
    } else {
        printf("  ○ Real MEGA65 Hardware (not connected)\n");
    }
    printf("\n");
    
    // Run tests
    std::vector<CrossValidationRunner::TestCase> tests = {
        {
            .name = "arithmetic",
            .programPath = "tests/45gs02/arithmetic.bin",
            .programAddr = 0x2000,
            .resultAddr = 0x0400,
            .resultSize = 16,
            .timeoutMs = 5000
        }
    };
    
    auto results = runner->runTests(tests);
    
    // Detailed report
    printf("\n╔════════════════════════════════════════════╗\n");
    printf("║  THREE-WAY CROSS-VALIDATION REPORT         ║\n");
    printf("╚════════════════════════════════════════════╝\n\n");
    
    for (const auto& [name, result] : results) {
        printf("Test: %s\n", name.c_str());
        printf("├─ mmsim:   %s\n", result.emulatorPass ? "✓ PASS" : "✗ FAIL");
        printf("├─ xemu:    %s\n", result.xemuPass ? "✓ PASS" : "✗ FAIL");
        printf("└─ hardware:%s\n", result.hardwarePass ? "✓ PASS" : "✗ FAIL");
        
        if (result.emulatorPass && result.xemuPass && result.hardwarePass) {
            if (result.resultsMatch) {
                printf("\n✓✓✓ PERFECT: All three match!\n");
            } else {
                printf("\n⚠ Results diverge between targets:\n");
                if (!result.emulatorMemory.empty() && !result.xemuMemory.empty()) {
                    bool emu_xemu_match = (result.emulatorMemory[0] == result.xemuMemory[0]);
                    printf("  mmsim vs xemu: %s\n", emu_xemu_match ? "MATCH" : "DIFFER");
                }
                if (!result.emulatorMemory.empty() && !result.hardwareMemory.empty()) {
                    bool emu_hw_match = (result.emulatorMemory[0] == result.hardwareMemory[0]);
                    printf("  mmsim vs hw:   %s\n", emu_hw_match ? "MATCH" : "DIFFER");
                }
                if (!result.xemuMemory.empty() && !result.hardwareMemory.empty()) {
                    bool xemu_hw_match = (result.xemuMemory[0] == result.hardwareMemory[0]);
                    printf("  xemu vs hw:    %s\n", xemu_hw_match ? "MATCH" : "DIFFER");
                }
            }
        } else if (result.emulatorPass && result.xemuPass) {
            printf("\n✓ Both emulators match (hardware not tested)\n");
        }
        printf("\n");
    }
    
    return 0;
}
```

## Interpreting Results

### Three-way match ✓✓✓
```
Test: arithmetic
├─ mmsim:   ✓ PASS
├─ xemu:    ✓ PASS
└─ hardware: ✓ PASS

✓✓✓ PERFECT: All three match!
```

→ **Emulation is accurate.** Continue testing other features.

### Emulators match, hardware differs
```
Test: arithmetic
├─ mmsim:   ✓ PASS
├─ xemu:    ✓ PASS
└─ hardware: ✓ PASS

Results diverge between targets:
  mmsim vs xemu: MATCH
  mmsim vs hw:   DIFFER
```

→ **Possible hardware quirk** or feature not implemented. 
- Compare both emulator outputs carefully
- May need MEGA65 documentation clarification

### mmsim differs from both others
```
Test: arithmetic
├─ mmsim:   ✓ PASS
├─ xemu:    ✓ PASS
└─ hardware: ✓ PASS

Results diverge between targets:
  mmsim vs xemu: DIFFER
  mmsim vs hw:   DIFFER
  xemu vs hw:    MATCH
```

→ **mmsim emulation bug!**
- xemu and hardware agree
- Investigate mmsim implementation of tested feature
- Create minimal test case to debug

### Only mmsim passes
```
Test: arithmetic
├─ mmsim:   ✓ PASS
├─ xemu:    ✗ FAIL
└─ hardware: ✗ FAIL
```

→ **Likely test infrastructure issue** (not a real mmsim feature)
- Verify test program is correct
- Check memory addresses and formats
- May be xemu/hardware limitation

## Factory Methods Reference

```cpp
// Compare mmsim vs xemu-xmega65
auto runner = CrossValidationRunner::withXemu();

// Compare mmsim vs real hardware
auto runner = CrossValidationRunner::withHardware("/dev/ttyUSB0");

// Compare mmsim vs xemu vs real hardware
auto runner = CrossValidationRunner::withAll();

// Custom configuration
auto runner = CrossValidationRunner::withAll(
    "127.0.0.1", 6502,                  // mmsim host:port
    "/usr/local/bin/xemu-xmega65",      // xemu path
    "/dev/ttyUSB0", 2000000             // hardware port and baud
);
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| xemu not found | Install: `apt-get install xemu-xmega65` |
| xemu timeout | May be too slow; increase `timeoutMs` parameter |
| mmsim connection refused | Start mmsim: `./bin/mmemu-cli -m rawMega65` |
| Hardware not responding | Check `/dev/ttyUSB0` connection and MEGA65 serial monitor |
| Results all differ | Verify test program is assembled correctly with `ca45` |
| xemu uses wrong BIOS | Place MEGA65 ROMs in xemu default location (~/.xemu/mega65/) |

## Performance Notes

- **mmsim**: ~100ms per test (fast, scriptable)
- **xemu**: ~2-3 seconds per test (slower, more accurate)
- **hardware**: ~5-10 seconds per test (slowest, most reliable)

For iterative debugging, test with mmsim + xemu first, then validate with hardware.

## See Also

- **HARDWARE_VALIDATION.md** — Full hardware integration guide
- **tests/45gs02/validate.py** — Original cross-validation script (xemu vs mmsim only)
- **src/cli/main/hardware_test_bridge.h** — API reference
