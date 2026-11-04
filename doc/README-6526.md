# mmemu-plugin-cia6526: MOS 6526 CIA Implementation

The MOS 6526 Complex Interface Adapter (CIA) is a versatile I/O chip used in the Commodore 64 as CIA #1 (\$DC00) and CIA #2 (\$DD00). It is implemented as an `IOHandler` plugin within `mmemu-plugin-cia6526.so`.

---

## 1. Intent

The CIA provides parallel I/O ports, programmable interval timers, a Time-Of-Day (TOD) clock, and interrupt generation. On the C64, CIA #1 handles keyboard scanning and joystick input, while CIA #2 manages the IEC serial bus, user port, and the VIC-II bank select lines.

---

## 2. Functionality

### 2.1 I/O Ports

Two 8-bit bidirectional ports (Port A and Port B), each with an independent Data Direction Register (DDR):

| Address | Register | Description |
|---------|----------|-------------|
| $x0 | PRA | Port A data latch |
| $x1 | PRB | Port B data latch |
| $x2 | DDRA | Port A direction (1 = output) |
| $x3 | DDRB | Port B direction (1 = output) |

Output bits come from the latch; input bits are read from the attached `IPortDevice`. Pull-up resistors make undriven inputs read as `1`.

### 2.2 Timers A and B

Two independent 16-bit countdown timers with latches:

| Address | Register | Description |
|---------|----------|-------------|
| $x4/$x5 | TALO/TAHI | Timer A counter (read) / latch (write) |
| $x6/$x7 | TBLO/TBHI | Timer B counter (read) / latch (write) |
| $xE | CRA | Timer A control |
| $xF | CRB | Timer B control |

**CRA/CRB bit fields:**

| Bit | Mask | Description |
|-----|------|-------------|
| 0 | `CR_START` | Timer running when set |
| 3 | `CR_ONESHOT` | One-shot mode (stops on underflow); continuous when clear |
| 4 | `CR_LOAD` | Force-load latch into counter (self-clearing) |
| CRA bit 7 | `CRA_TODIN` | TOD input frequency: 0 = 60 Hz, 1 = 50 Hz |
| CRB bit 6-5 | `CRB_INMODE_TA` | Timer B counts Timer A underflows when `$40` |
| CRB bit 7 | `CRB_ALARM` | TOD writes go to alarm registers when set |

On underflow, the latch is reloaded automatically (continuous mode) or the timer stops (one-shot). Writing to the high byte (TAHI/TBHI) while the timer is stopped also loads the latch into the counter.

### 2.3 TOD Clock

BCD real-time clock with alarm:

| Address | Register | Description |
|---------|----------|-------------|
| $x8 | TODTEN | Tenths of seconds (0–9 BCD) |
| $x9 | TODSEC | Seconds (0–59 BCD) |
| $xA | TODMIN | Minutes (0–59 BCD) |
| $xB | TODHR | Hours (1–12 BCD) + bit 7 AM/PM |

**Latch behavior:** Reading TODHR freezes all TOD registers into a shadow latch. Subsequent reads return latch values until TODTEN is read, which releases the latch. This prevents tearing on multi-byte reads.

**Alarm:** When CRB bit 7 is set, TOD writes go to the alarm registers instead of the clock. The TOD ICR bit fires when the running clock matches the alarm.

### 2.4 Interrupt Control Register (ICR)

| Address | Register | Description |
|---------|----------|-------------|
| $xD | ICR | Interrupt Control Register |

**ICR bit assignments:**

| Bit | Mask | Source |
|-----|------|--------|
| 0 | `ICR_TA` | Timer A underflow |
| 1 | `ICR_TB` | Timer B underflow |
| 2 | `ICR_TOD` | TOD alarm match |
| 7 | `ICR_INT` | Any enabled interrupt active |

**Write semantics:** Bit 7 = 1 sets the mask bits; bit 7 = 0 clears them.
**Read semantics:** Returns pending bits; reading clears all pending bits and de-asserts IRQ.

### 2.5 Peripheral Injection

Attach keyboard matrices or joysticks via `IPortDevice`:

```cpp
cia1->setPortADevice(kbdMatrix);
cia1->setPortBDevice(joystick);
cia1->setIrqLine(irqLine);
```

By default an output bit reads back the output latch. `setOutputsReadPins(true)` makes it read the pin instead, `latch & device pins`, as on the real 6526: a device can then pull low a line the CIA drives high. The C64 and MEGA65 turn this on for CIA1, where control port 2 shares Port A with the keyboard columns.

---

## 3. Register Map

| Offset | Symbol | Read | Write |
|--------|--------|------|-------|
| $0 | PRA | Port A (DDR masked) | Port A latch |
| $1 | PRB | Port B (DDR masked) | Port B latch |
| $2 | DDRA | Direction A | Direction A |
| $3 | DDRB | Direction B | Direction B |
| $4 | TALO | Timer A low byte | Timer A latch low |
| $5 | TAHI | Timer A high byte | Timer A latch high (+ load if stopped) |
| $6 | TBLO | Timer B low byte | Timer B latch low |
| $7 | TBHI | Timer B high byte | Timer B latch high (+ load if stopped) |
| $8 | TODTEN | TOD tenths (releases latch) | TOD/alarm tenths |
| $9 | TODSEC | TOD seconds | TOD/alarm seconds |
| $A | TODMIN | TOD minutes | TOD/alarm minutes |
| $B | TODHR | TOD hours (freezes latch) | TOD/alarm hours |
| $C | SDR | Serial Data Register (stub) | Serial Data Register (stub) |
| $D | ICR | Pending bits + $80 if any; clears on read | Set/clear mask (bit 7 selects mode) |
| $E | CRA | Control A | Control A |
| $F | CRB | Control B | Control B |

---

## 4. Dependencies

- **Depends on**: `IPortDevice`, `ISignalLine` (both from `libdevices`)
- **Required by**: C64 Machine Plugin (Phase 11.6)

---

## 5. Implementation Details

- **Source Location**: `src/plugins/devices/cia6526/main/cia6526.h/cpp`
- **Plugin**: `lib/mmemu-plugin-cia6526.so`
- **Class**: `CIA6526 : public IOHandler`
- **Registration Name**: `"6526"`
- **Address Mask**: `0x000F` (16 registers mirrored across the $DC00/$DD00 window)
- **Clock Default**: 1 022 727 Hz (NTSC C64); call `setClockHz(985248)` for PAL
