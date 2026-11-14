# mmemu Virtual IEC Bus Device

> The C64's drive 8 is now `IecDrive` (`iec_drive`, `src/plugins/devices/iec_drive/`): the line-level 1541 the C128 and Plus/4 use, also an `IPortDevice` for a CIA port, registered by the same plugin as `virtual_iec`. Its host-folder DOS tries `<name>.prg` for a name without an extension, as VICE's file-system drive does. `VirtualIECBus` below stays available but no machine uses it; its loads stalled after `LOADING` (#348).

`VirtualIECBus` provides Level 2 (protocol-level) Commodore Serial Bus (IEC) emulation. It handles the ATN/CLK/DATA handshake at the signal level so that standard KERNAL disk routines work without emulating a full 1541 drive internally.

For a faster alternative that bypasses the serial protocol entirely, see the [CBM KERNAL HLE Plugin](README-CBM-HLE.md).

---

## 1. IEC Bus Basics

The Commodore Serial Bus uses open-collector (wired-OR) logic on three primary signal lines:

| Line | Direction | Description |
|------|-----------|-------------|
| **ATN** | Host → Device | Attention: host is broadcasting a command byte. |
| **CLK** | Talker → Listener | Clock: synchronises each data bit. |
| **DATA** | Talker or Listener | Carries data bits; also used for handshake acknowledge. |

A line is logical **True** (1) when pulled low (0 V) and **False** (0) when released high (+5 V). Multiple devices share the same lines; any device can assert True regardless of others.

---

## 2. State Machine

`VirtualIECBus` progresses through the following states on each `tick()` call:

```
IDLE → ATTENTION → ADDRESSING → ACKNOWLEDGE
                                   ↓
                             READY_TO_RECEIVE / READY_TO_SEND
                                   ↓
                             RECEIVING / SENDING
```

| State | Entry condition | Device action |
|-------|-----------------|---------------|
| `IDLE` | ATN released | All lines released |
| `ATTENTION` | ATN asserted by host | Device pulls DATA after ~2000 cycles |
| `ADDRESSING` | ATN still asserted | Receive 8-bit command via CLK/DATA edges |
| `ACKNOWLEDGE` | Command byte received | DATA held low until ATN released |
| `READY_TO_RECEIVE` | LISTEN command for our unit | Wait for data bytes |
| `READY_TO_SEND` | TALK command for our unit | Prepare data for transmission |

The state machine processes up to two transitions per `tick()` call so that accumulated cycles carry across a state boundary (e.g., detecting ATN and satisfying the ATTENTION threshold in a single `tick(3000)` call).

---

## 3. CIA / VIA Port Mapping

### C64 — CIA #2 Port A (`$DD00`)

| Bit | Direction | Signal |
|-----|-----------|--------|
| 3 | Output | ATN OUT |
| 4 | Output | CLK OUT |
| 5 | Output | DATA OUT |
| 6 | Input | CLK IN (bus CLK reflected back) |
| 7 | Input | DATA IN (bus DATA reflected back) |

`writePort(val)` reads bits 3–5 to update the host's contribution to the bus. `readPort()` combines the host's outputs with the virtual device's outputs (wired-OR) and returns bits 6–7.

### VIC-20 — VIA #2

The same bit mapping applies; connect via `iecWiring` in the machine JSON.

---

## 4. JSON Machine Wiring

Add an `iecWiring` section to a machine descriptor to attach `VirtualIECBus` to a CIA or VIA port:

```json
"devices": [
  { "type": "virtual_iec", "name": "IEC", "baseAddr": null }
],
"iecWiring": {
  "device": "IEC",
  "busPort": { "device": "CIA2", "port": "A" }
}
```

The loader casts the named device to `IPortDevice` and calls `setPortADevice()` (or `setPortBDevice()`) on the target CIA/VIA.

---

## 5. C++ API

```cpp
#include "virtual_iec.h"

// Construction
VirtualIECBus iec(8);       // unit number 8 (default)

// IPortDevice — called by the CIA/VIA on every port read/write
uint8_t readPort();          // returns combined CLK-IN / DATA-IN bits
void    writePort(uint8_t);  // sets ATN-IN, CLK-IN, DATA-IN from host bits
void    setDdr(uint8_t);     // data direction register (informational)

// IOHandler lifecycle
void tick(uint64_t cycles);  // advance state machine
void reset();                // return to IDLE, clear all lines

// Disk image management (stubs; full implementation in Phase 15.3)
bool mountDisk(int unit, const std::string& path);
void ejectDisk(int unit);
```

---

## 6. Testing

```
src/plugins/devices/virtual_iec/test/test_virtual_iec.cpp
  virtual_iec_handshake — verifies IDLE→ATTENTION→ADDRESSING cycle.
src/plugins/devices/virtual_iec/test/test_iec_d64.cpp
  iec_d64_mount — verifies .d64 image mounting and sector reading via IEC state machine.
```

Run with `make test`.

---

## 7. Source Layout

```
src/plugins/devices/virtual_iec/
├── main/
│   ├── virtual_iec.h          — class declaration, state enum, port mapping
│   ├── virtual_iec.cpp        — state machine, bit transfer, command dispatch
│   └── plugin_init.cpp        — mmemuPluginInit() entry point
└── test/
    ├── test_virtual_iec.cpp   — handshake unit test
    └── test_iec_d64.cpp       — D64 disk image unit test
```

The plugin is built as `lib/mmemu-plugin-virtual-iec.so` and loaded automatically by the plugin loader.

---

## 8. Limitations

- EOI (End-Or-Identify) signalling is currently simplified.
- Only one virtual device (unit 8) per bus is supported by default.
- Fast-loaders that bit-bang the bus without using standard KERNAL timing may require further refinement.
- **Note**: Recent updates have added support for bit-streaming from `.d64` disk images (Phase 15.3 partially integrated).
