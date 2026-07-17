# Unified Test Runner - Dynamic Multi-Backend Testing

The **Unified Test Runner** provides a single command-line interface to run test programs against any combination of backends:
- **mmsim** — Our MEGA65 emulator (default)
- **xemu-xmega65** — Alternative MEGA65 emulator
- **Real MEGA65 hardware** — Physical system via serial port

## Quick Start

### Single Backend (mmsim only)

**Important:** The emulator must be running with serial monitor support enabled.

```bash
# Terminal 1: Start emulator with serial monitor server
./bin/mmemu-cli -m c64 --serial-monitor-port 6502

# Terminal 2: Run tests
./bin/mmemu-test-runner tests/45gs02/arithmetic.bin
```

### Multiple Backends
```bash
# Test on both mmsim and xemu
./bin/mmemu-test-runner -mmemu -xmega65 tests/45gs02/arithmetic.bin

# Test on all available backends
./bin/mmemu-test-runner -all tests/45gs02/arithmetic.bin

# Test with real hardware
./bin/mmemu-test-runner -real tests/45gs02/arithmetic.bin
```

### Batch Testing
```bash
# Test all programs in directory
./bin/mmemu-test-runner -mmemu tests/45gs02/*.bin

# Test with specific machine
./bin/mmemu-test-runner -mmemu -machine mega65 tests/45gs02/arithmetic.bin
```

## Command-Line Options

| Option | Description | Default |
|--------|-------------|---------|
| `-mmemu` | Test on mmsim emulator | ✓ (default) |
| `-xmega65` | Test on xemu-xmega65 | — |
| `-real` | Test on real MEGA65 hardware | — |
| `-all` | Test on all available backends | — |
| `-machine <type>` | Machine preset (c64, vic20, mega65, etc.) | c64 |
| `-host <host>` | Emulator host | 127.0.0.1 |
| `-port <port>` | Emulator port | 6502 |
| `-serial <dev>` | Serial port for hardware | /dev/ttyUSB0 |
| `-baud <rate>` | Serial baud rate | 2000000 |
| `-timeout <ms>` | Test timeout in milliseconds | 5000 |
| `-json` | Output results in JSON format | — |
| `-verbose` | Verbose output with memory dumps | — |
| `-help` | Show help message | — |

## Usage Examples

### Example 1: Compare Two Emulators
```bash
# Terminal 1: Start mmsim with serial monitor server
./bin/mmemu-cli -m rawMega65 --serial-monitor-port 6502

# Terminal 2: Run cross-validation
./bin/mmemu-test-runner -mmemu -xmega65 tests/45gs02/arithmetic.bin
```

Output:
```
╔════════════════════════════════════════════════════════╗
║         Unified Test Runner - Cross-Validation         ║
╚════════════════════════════════════════════════════════╝

Test: arithmetic (tests/45gs02/arithmetic.bin)
──────────────────────────────────────────────────────
  mmsim:        ✅ PASS
  xemu-xmega65: ✅ PASS

  Consistency: ✅ All backends match!

╭────────────────────────────────────────────────────────╮
│ Results: 1/1 passed, 1/1 consistent              │
╰────────────────────────────────────────────────────────╯
```

### Example 2: Test Real Hardware
```bash
# Terminal 1: Start mmsim with serial monitor server
./bin/mmemu-cli -m rawMega65 --serial-monitor-port 6502

# Terminal 2: Connect USB-UART adapter to MEGA65 JTAG pins 2-3

# Terminal 3: Run three-way validation
./bin/mmemu-test-runner -all \
  -serial /dev/ttyUSB0 \
  -baud 2000000 \
  tests/45gs02/transfers.bin
```

### Example 3: Batch Testing with Verbose Output
```bash
./bin/mmemu-test-runner \
  -mmemu -xmega65 \
  -verbose \
  -machine c64 \
  tests/45gs02/*.bin
```

### Example 4: JSON Output for CI/CD
```bash
./bin/mmemu-test-runner \
  -all \
  -json \
  tests/45gs02/*.bin > test-results.json
```

Result format:
```json
[
  {
    "testName": "arithmetic",
    "testFile": "tests/45gs02/arithmetic.bin",
    "overallPass": true,
    "consistent": true,
    "results": [
      {
        "backend": "mmsim",
        "passed": true,
        "error": "",
        "memory": [1, 1, 32, 32, ...],
        "output": ""
      },
      {
        "backend": "xemu-xmega65",
        "passed": true,
        "error": "",
        "memory": [1, 1, 32, 32, ...],
        "output": ""
      }
    ]
  }
]
```

## Available Test Programs

Located in `tests/45gs02/`:

| Program | Tests | Description |
|---------|-------|-------------|
| `arithmetic.bin` | ADC, SBC | Basic arithmetic operations |
| `transfers.bin` | Transfer ops | Register/memory transfers |
| `quad.bin` | Quad immediate | 16-bit immediate addressing |
| `all_opcodes.bin` | All opcodes | Full 45GS02 instruction set |
| `advanced.bin` | Complex sequences | Advanced instruction patterns |
| `hello.bin` | I/O operations | Basic I/O and string output |

Each test writes results to memory `$0400-$040F`:
- `$01` = test passed
- `$FF` = test failed
- `$20` = unused/padding

## Architecture

### UnifiedTestRunner Class

The main orchestration class:

```cpp
class UnifiedTestRunner {
public:
    enum class Backend { MMEMU, XMEGA65, REAL };
    
    struct Config {
        std::vector<Backend> backends;
        std::string machine;
        std::string emuHost;
        uint16_t emuPort;
        std::string serialPort;
        uint32_t serialBaud;
        uint32_t timeoutMs;
        bool jsonOutput;
        bool verbose;
        std::vector<std::string> testFiles;
    };
    
    struct TestResult {
        std::string testName;
        std::vector<BackendResult> results;
        bool allMatch;
    };
    
    // Parse command-line arguments
    static Config parseArgs(int argc, char* argv[]);
    
    // Discover test programs
    static std::vector<std::string> discoverTests(const std::string& dir);
    
    // Run tests
    std::vector<TestResult> runTests(const std::vector<std::string>& testFiles);
};
```

### Execution Flow

```
┌─────────────────────────────────────┐
│  Parse command-line arguments       │
│  Identify backends to test on       │
└────────────┬────────────────────────┘
             │
             ├─ -mmemu ────────► mmsim
             │
             ├─ -xmega65 ───► xemu-xmega65
             │
             └─ -real ───────► MEGA65 hardware
                                    │
                    ┌───────────────┴────────────────┐
                    │                                │
            ┌───────▼────────┐          ┌───────────▼────┐
            │ Create runners │          │ Load test files│
            │ for selected   │          │                │
            │ backends       │          │ (binary data)  │
            └───────┬────────┘          └────────────────┘
                    │
                    └──────────────────┬─────────────────┐
                                       │                 │
                             ┌─────────▼─────┐  ┌────────▼──────┐
                             │ Run on each    │  │ Compare       │
                             │ backend:      │  │ results       │
                             │ - Load prog   │  │ across        │
                             │ - Execute     │  │ backends      │
                             │ - Read memory │  │               │
                             └────────┬──────┘  └────────┬───────┘
                                      │                  │
                                      └──────────┬───────┘
                                                 │
                                      ┌──────────▼──────────┐
                                      │ Output results:    │
                                      │ - Human-readable   │
                                      │ - JSON (CI/CD)     │
                                      │ - Exit code        │
                                      └────────────────────┘
```

## Test Result Interpretation

### All Backends Match
```
Consistency: ✅ All backends match!
```
→ Emulation is accurate across all targets

### Some Backends Differ
```
Consistency: ⚠ Results diverge
```
→ Investigate discrepancies:
- If mmsim ≠ xemu/hardware: Bug in mmsim
- If xemu ≠ hardware: Bug in xemu (hardware is authoritative)
- If all differ: Test infrastructure issue

### Backend Unavailable
```
Error: xemu-xmega65 not available
```
→ Backend not reachable; continue with others

## Hardware Setup

For testing with real MEGA65:

1. **USB-to-UART Adapter** (FTDI FT232RL or CP2102)
   ```
   MEGA65 JTAG Pins:
   Pin 1 (GND)  ──► GND
   Pin 2 (RX)   ──► TXD (adapter)
   Pin 3 (TX)   ──► RXD (adapter)
   ```

2. **Linux Permissions**
   ```bash
   sudo usermod -a -G dialout $USER
   ```

3. **Verify Connection**
   ```bash
   ls -la /dev/ttyUSB0  # or ttyACM0 depending on adapter
   ```

## Performance Notes

Typical execution times per test:

| Backend | Time | Notes |
|---------|------|-------|
| mmsim | ~100ms | Fastest; in-process |
| xemu-xmega65 | ~2-3s | Subprocess; subprocess overhead |
| Real hardware | ~5-10s | Serial communication; slowest |

## Troubleshooting

### "mmsim not running on localhost:6502" or "Failed to create cross-validation runner"
The emulator must be started with **`--serial-monitor-port`** flag:
```bash
# Terminal 1: Correct way
./bin/mmemu-cli -m c64 --serial-monitor-port 6502

# Terminal 2: Run tests
./bin/mmemu-test-runner -mmemu tests/45gs02/arithmetic.bin
```

The default port is `6502` (matches `-port` option in test runner).

### "xemu-xmega65 not available"
```bash
sudo apt-get install xemu-xmega65
```

### Serial port permission denied
```bash
sudo usermod -a -G dialout $USER
# Logout and log back in
```

### Test timeout
Increase with `-timeout <ms>`:
```bash
./bin/mmemu-test-runner -timeout 10000 tests/45gs02/arithmetic.bin
```

## Integration with CI/CD

### GitHub Actions Example
```yaml
- name: Run cross-validation tests
  run: |
    ./bin/mmemu-cli -m c64 --serial-monitor-port 6502 &
    sleep 2
    ./bin/mmemu-test-runner \
      -mmemu -xmega65 \
      -json \
      tests/45gs02/*.bin | tee test-results.json
    
    # Check results
    python3 scripts/validate_test_results.py test-results.json
```

### Jenkins/GitLab CI
```bash
#!/bin/bash
set -e

# Start emulator with serial monitor server
timeout 120 ./bin/mmemu-cli -m c64 --serial-monitor-port 6502 >/dev/null 2>&1 &
EMU_PID=$!
sleep 2

# Run tests
./bin/mmemu-test-runner \
  -mmemu -xmega65 \
  -json \
  tests/45gs02/*.bin > /tmp/results.json

# Cleanup
kill $EMU_PID 2>/dev/null || true

# Validate
if ! python3 scripts/validate_test_results.py /tmp/results.json; then
    echo "❌ Tests failed"
    exit 1
fi

echo "✅ All tests passed"
```

## Extending the Test Runner

### Adding a New Backend

1. Create a new bridge class inheriting from `HardwareTestBridge`
2. Implement `loadMemory()`, `readMemory()`, `step()`, etc.
3. Add factory method to `CrossValidationRunner`
4. Add command-line option to `UnifiedTestRunner::parseArgs()`

Example:
```cpp
class MyEmulatorBridge : public HardwareTestBridge {
    // Implementation
};

// In UnifiedTestRunner::createRunner()
if (backend == Backend::MYEMU) {
    return std::make_unique<MyEmulatorBridge>(...);
}
```

### Custom Test Discovery

```cpp
auto tests = UnifiedTestRunner::discoverTests("my/test/dir");
for (const auto& test : tests) {
    std::cout << "Found: " << test << "\n";
}
```

## See Also

- **HARDWARE_VALIDATION.md** — Detailed hardware integration guide
- **XEMU_VALIDATION.md** — xemu-xmega65 setup and usage
- **src/cli/main/unified_test_runner.h/cpp** — Implementation
- **src/cli/main/cross_validation_runner.h/cpp** — Backend coordination
- **src/cli/main/hardware_test_bridge.h/cpp** — Bridge implementations
