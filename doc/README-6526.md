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

| Bit | Mask | Description |
|-----|------|-------------|
| 1 | `CR_PBON` | Timer output on PB6 (timer A) / PB7 (timer B) |
| 2 | `CR_TOGGLE` | Output toggles on each underflow (set when started); else a one-cycle pulse |

**Cycle timing (old 6526, after VICE's `ciatimer.h`).** Each timer is a small
state machine stepped once per cycle: the control bits as written move
through delay stages, so

- a timer started on cycle *n* holds its value on *n*+1 and *n*+2 and decrements from *n*+3;
- a force load written on cycle *n* lands on *n*+2 (a latch write in that cycle goes straight to the counter);
- the counter counts down to 0 and reloads on that same cycle, so the period is latch + 1;
- one-shot mode stops the timer on its underflow; CRA/CRB bit 0 then reads 0;
- timer B counting timer A underflows takes each count one cycle after the underflow.

An access after `tick(n)` sees the chip as of the last cycle ticked; the C64
scheduler ticks through the access's own cycle before performing it.
`tick()` skips stretches of steady counting (or stopped timers) in bulk.

### 2.3 TOD Clock

BCD real-time clock with alarm:

| Address | Register | Description |
|---------|----------|-------------|
| $x8 | TODTEN | Tenths of seconds (0–9 BCD) |
| $x9 | TODSEC | Seconds (0–59 BCD) |
| $xA | TODMIN | Minutes (0–59 BCD) |
| $xB | TODHR | Hours (1–12 BCD) + bit 7 AM/PM |

**Latch behavior:** Reading TODHR freezes all TOD registers into a shadow latch. Subsequent reads return latch values until TODTEN is read, which releases the latch. This prevents tearing on multi-byte reads.

**Alarm:** When CRB bit 7 is set, TOD writes go to the alarm registers instead of the clock. The TOD ICR bit fires when the clock matches the alarm: when the clock counts to it, and when a write changes the clock or the alarm to match.

**Counting (after VICE's `ciacore_inttod`):** the TOD counts mains ticks, one every `clockHz / 50` cycles on a PAL machine (`clockHz` under 1 MHz) and `clockHz / 60` otherwise, kept to exactly 50 or 60 a second (VICE's jitter left out). A 3-bit ring counter divides them to tenths: CRA bit 7 picks 5 (50 Hz) or 6 (60 Hz) ticks, so the wrong setting runs the clock fast or slow. Each digit is a 4-bit counter (3 bits for the tens of seconds and minutes), so values set outside BCD count on as the chip's do; the hours go 09 → 10 and 12 → 01, and 11 → 12 flips AM/PM. Writing hour 12 to the clock flips AM/PM too. The clock is stopped after reset; writing the hours stops it and writing the tenths starts it.

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
| 3 | `ICR_SP` | Serial port byte shifted |
| 4 | — | FLAG input (falling edge) |
| 7 | `ICR_INT` | Any enabled interrupt active |

**Write semantics:** Bit 7 = 1 sets the mask bits; bit 7 = 0 clears them.
**Read semantics:** Returns pending bits; reading clears all pending bits and de-asserts IRQ.

**Interrupt timing (old 6526, VICE's `ifr_delay` pipeline):** a source sets its
flag at once; bit 7 (IR) and the IRQ output follow one cycle later. Enabling
the mask of a flag already set raises IRQ two cycles after the write. A read
clears the flags and releases IRQ at once, and IR two cycles later. Timer B
bug: if ICR was read the cycle before a timer B underflow, the next read drops
the TB flag, whether timer B counts Φ2 or timer A's underflows (VICE's
`ciacore_inttb` flags it either way; testprogs `CIA/ciavarious/cia8`). FLAG (bit 4), SP (bit 3) and the TOD alarm (bit 2) go through the
same pipeline.

**The 6526A** (`setOption("model", "6526A")`; machine JSON `"options": { "model": "6526A" }`), the C64C's and C128's "new" CIA, differs only in this pipeline (VICE's `CIA_MODEL_6526A`): a flag raises IR and IRQ in the cycle it is set (a cycle later if the ICR was read the cycle before); enabling the mask of a pending flag raises IRQ in the next cycle, unless the ICR was read last cycle; a read leaves the flags it returned set for one more cycle; and there is no timer B bug. The old 6526 is the default.

### 2.4.1 Serial port (SDR)

After VICE's `ciacore` (`sdr_delay`). In output mode (CRA bit 6), a byte written to SDR goes into the shift register two cycles later. Timer A's underflows toggle CNT about 1.5 cycles on, and a bit goes out on each falling edge, so a byte takes 16 underflows. The SP flag follows the last bit by two cycles, and a byte written while one is going out follows it straight on. Switching CRA's direction mid-byte can still raise SP, as on the real chip ([VICE bug 1219](https://sourceforge.net/p/vice-emu/bugs/1219/)). A read of the ICR in the cycle the shift register sets SP doesn't see the flag yet. In input mode a byte from outside (`receiveSerialByte()`, the C128's fast serial) lands in SDR and raises SP at once. Each byte shifted out goes to `setSerialOut()`. The MEGA65's CIAs (model `mega65`) finish an output byte at once instead, as the MEGA65 ROM expects.

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
| $C | SDR | Serial Data Register | Serial Data Register (output: shifted out) |
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
- **Options** (`setOption`, or `"options"` in a machine JSON device): `model` = `6526` (default), `6526A` or `mega65`
- **`decode`**: the address window the CIA answers in, as a mask (`0x0F` by default up to `0xFF`); its 16 registers repeat through it. The C64 decodes each CIA through its whole page, `$DC00-$DCFF` and `$DD00-$DDFF` (`machines/c64.json`: `"decode": "0xFF"`; VICE `CIA/mirrors`). The C128 mirrors them in its own I/O decode.
