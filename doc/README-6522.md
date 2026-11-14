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
The timers follow VICE's `viacore.c` (#341), stepped a cycle at a time. A counter read sees the value of the current cycle. A write takes effect from the next cycle.

- **Timer 1:**
  - Counts N, N-1, …, 0, `$FFFF`, then reloads the latch: a period of N+2. It reloads in one-shot mode too, against the datasheet.
  - IFR bit 6 is set in the cycle after the counter reads 0, while an IRQ is armed. A T1CH write arms it. A one-shot fires once per write. Free-running keeps firing, but switching to free-running doesn't re-arm a one-shot that has already fired.
  - With ACR bit 7, PB7 shows the timer: low from a T1CH write, toggled at each IRQ. The latches read back at `$x6`/`$x7`.
- **Timer 2:**
  - Counts down from a T2CH write through `$FFFF`. IFR bit 5 is set once per write, the cycle after it reads `$0000`.
  - With the shift register on it (ACR modes 1, 4, 5) it is an 8-bit timer. The low byte reloads from its latch the cycle after it shows `xxFF`, the high byte counts those underflows, and the 16-bit underflow still flags.
  - Counting PB6 pulses (ACR bit 5) it stops: no pulses are modelled. Switching between the modes takes a cycle: one more decrement when stopping, a cycle's wait when starting.
- IER reads with bit 7 set. The shift register itself doesn't shift yet.

VICE testprogs `drive/viavarious` (19 programs, through the true 1541, README-1541) pass.

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
