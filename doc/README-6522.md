# mmemu-plugin-via6522: MOS 6522 VIA Implementation

This document describes the MOS 6522 Versatile Interface Adapter (VIA) implementation for the mmemu platform.

---

## 1. Intent
The MOS 6522 is a highly flexible I/O chip featuring two 8-bit bidirectional ports, two 16-bit timers, and a shift register. It is a core component of the VIC-20 and many other 8-bit computers and peripherals (such as the 1541 disk drive).

---

## 2. Functionality

### 2.1 I/O Ports (Port A & Port B)
- **Bidirectional Control**: Fully supports Data Direction Registers (DDRA/DDRB).
- **Peripheral Injection**: Uses the `IPortDevice` interface to allow external hardware (keyboards, joysticks) to be attached at runtime.
- **Input/Output Masking**: Correctly applies DDR masks during reads and writes.

### 2.2 Timers
- **Timer 1**: Supports one-shot and continuous (auto-reload) modes. Sets the Interrupt Flag Register (IFR) bit 6 on underflow.
- **Timer 2**: Supports one-shot interval mode. Sets IFR bit 5 on underflow.

### 2.3 Control Lines & Signal API

The VIA exposes all four handshake/interrupt control lines via `getSignalLine(name)`:

| Name | Direction | IFR bit | Description |
|------|-----------|---------|-------------|
| `"ca1"` | Input | bit 1 | Triggers IFR on active edge (PCR bit 0: 0=falling, 1=rising). |
| `"ca2"` | In/Out | bit 0 | Input edge or output (handshake/pulse/manual per PCR bits 3–1). |
| `"cb1"` | Input | bit 4 | Triggers IFR on active edge (PCR bit 4: 0=falling, 1=rising). |
| `"cb2"` | In/Out | bit 3 | Input edge or output (per PCR bits 7–5). |
| `"pb7"` | Read-only proxy | — | Reads bit 7 of the ORB register. |
| `"pb0"`–`"pb6"` | Read-only proxy | — | Reads the corresponding bit of ORB. |
| `"pa0"`–`"pa7"` | Input pin | Peripheral | A port A pin held by a peripheral: `set(false)` pulls it low. While its DDRA bit is 0, ORA reads it (other input pins read high). The VIC-20's tape sense is VIA1 `"pa6"`. |

**Immediate CA1/CB1 edge detection**: CA1 and CB1 use dedicated conduit objects that detect the active edge inside `set()` and update IFR immediately — without waiting for the next `tick()` call. This ensures devices that pulse these lines instantaneously within their own tick (such as the Datasette) are correctly detected regardless of device tick ordering. CA2 and CB2 in input mode continue to use tick-based polling.

### 2.4 Interrupts
- **IER/IFR Logic**: Implements the Interrupt Enable and Interrupt Flag registers.
- **IRQ Signal**: Propagates interrupt state to the CPU via the `ISignalLine` interface.

---

## 3. Dependencies
This plugin is a **leaf node** in the plugin hierarchy:
- **Depends on**: None (Core mmemu interfaces only).
- **Required by**: [VIC-20 Machine Plugin](README-VIC20.md).

---

## 4. Implementation Details
- **Source Location**: `src/plugins/devices/via6522/`
- **Library**: `lib/internal/libdeviceVIA6522.a`
- **Plugin**: `lib/mmemu-plugin-via6522.so`
- **Class**: `VIA6522`
- **Registration Name**: `"6522"`
