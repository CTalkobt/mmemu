# mmemu-plugin-pokey: Atari POKEY Implementation

This document describes the POKEY (Potentiometer and Keyboard Encoder) implementation for the mmemu platform.

---

## 1. Intent
POKEY is a multi-purpose chip used in the Atari 8-bit family and many arcade games. It handles audio synthesis, interval timers, keyboard scanning, and serial I/O (SIO).

---

## 2. Functionality

POKEY is clocked once per CPU cycle (1.79 MHz NTSC).

### 2.1 Audio

- **Clocks:** four channels run on the 64 kHz base clock (CPU clock / 28) or 15 kHz (/ 114, AUDCTL bit 0). Channels 1 and 3 can instead run at 1.79 MHz (AUDCTL bits 6 and 5).
- **Periods:** an 8-bit channel's period is AUDF + 1, or AUDF + 4 at 1.79 MHz. Channels 1+2 and 3+4 can be joined into 16-bit channels (AUDCTL bits 4 and 3), with a period of AUDF + 1, or AUDF + 7 at 1.79 MHz.
- **Distortion (AUDC bits 7-5):** on each underflow, the 5-bit polynomial gates the change. The output then becomes a pure tone, the 4-bit polynomial or the 17-bit (or 9-bit, AUDCTL bit 7) polynomial.
- **Other features:**
  - Volume-only mode (AUDC bit 4).
  - High-pass filters: channel 1 by 3, and channel 2 by 4 (AUDCTL bits 2 and 1).
- **Output:** averaged to the sample rate, with the DC removed.

### 2.2 Timers

Channels 1, 2 and 4 raise IRQs when they underflow (IRQEN/IRQST bits 0, 1 and 2). STIMER restarts all counters. Clearing an IRQEN bit clears its IRQ.

### 2.3 Keyboard

- **`keyDown(code)`:** sets KBCODE (the 6-bit key code, plus `$40` shift and `$80` control), raises the key IRQ (bit 6) and clears SKSTAT bit 2 (key down).
- **`setShift`:** SKSTAT bit 3.
- **`breakKey`:** the BREAK IRQ (bit 7).
- **SKCTL bits 0-1 = 0:** keyboard scanning is off and RANDOM is held.

### 2.4 Serial I/O (SIO)

- **Sending:** a byte written to SEROUT moves into the shift register. The "output needed" IRQ (bit 4) fires and the next byte waits in a buffer.
- **Speed:** a frame is 10 bits, and a bit is two channel-4 underflows (19200 baud from the OS's `$0028` on channels 3+4 at 1.79 MHz).
- **Done:** "transmission done" (bit 3) is a level, asserted while the transmitter is idle.
- **Bytes sent** go to `setSerialOut()`. `receiveSerial()` delivers SERIN and the "input ready" IRQ (bit 5).
- **No devices yet:** nothing answers, so the OS's disk boot times out (about 4 seconds on an XL).

### 2.5 Other

- **RANDOM:** from the 17-bit or 9-bit polynomial.
- **Pots:** POT0-7 read 228, and ALLPOT reads 0.

## 3. Register Map ($D200–$D20F)

| Offset | Symbol | Access | Description |
|--------|--------|--------|-------------|
| $00 | AUDF1 | W | Audio Frequency Ch 1 |
| $01 | AUDC1 | W | Audio Control Ch 1 |
| $02 | AUDF2 | W | Audio Frequency Ch 2 |
| $03 | AUDC2 | W | Audio Control Ch 2 |
| $04 | AUDF3 | W | Audio Frequency Ch 3 |
| $05 | AUDC3 | W | Audio Control Ch 3 |
| $06 | AUDF4 | W | Audio Frequency Ch 4 |
| $07 | AUDC4 | W | Audio Control Ch 4 |
| $08 | AUDCTL | W | Audio Control (Global) |
| $09 | STIMER | W | Start Timers |
| $09 | ALLPOT | R | Potentiometer Status |
| $0A | SKRES | W | Reset Serial Status |
| $0A | KBCODE | R | Keyboard Code |
| $0B | POTGO | W | Start Pot Scan |
| $0B | RANDOM | R | Random Number (Poly17) |
| $0D | SEROUT | W | Serial Output |
| $0D | SERIN | R | Serial Input |
| $0E | IRQEN | W | IRQ Enable |
| $0E | IRQST | R | IRQ Status |
| $0F | SKCTL | W | Serial Control |
| $0F | SKSTAT | R | Serial Status |

---

## 4. Dependencies
- **Depends on**: `IAudioOutput` (from `libdevices`)
- **Required by**: Atari Machine Plugins (Phase 26.4).

---

## 5. Implementation Details
- **Source Location**: `src/plugins/devices/pokey/`
- **Plugin**: `lib/mmemu-plugin-pokey.so`
- **Class**: `POKEY`
- **Registration Name**: `"POKEY"`
