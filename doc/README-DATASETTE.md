# mmemu Datasette Device

The Datasette is a software model of the Commodore cassette tape drive. It is wired into each tape-capable machine (C64, VIC-20, PET) as a named `"Tape"` I/O device with no fixed base address (it does not occupy any memory-mapped register space itself — all signals flow through the host machine's I/O chips).

---

## 1. Signal Interface

The Datasette communicates with the host machine through four signal lines, declared in the `tapeWiring` section of each machine's JSON descriptor:

| Signal | Direction | Description |
|--------|-----------|-------------|
| `motor` | Machine → Datasette | Enables tape transport. The Datasette only plays/records while the motor is asserted. Active-low on VIC-20 and PET; the signal source is machine-specific. |
| `write` | Machine → Datasette | Cassette write line. While recording, each cycle of the line, from one 0→1 transition to the next, is stored as one pulse, as VICE records it. |
| `readPulse` | Datasette → Machine | Each stored tape pulse drives this line low then high. The receiving chip (CIA1 FLAG, VIA CA1, or PIA CA1) captures the edge as an interrupt. |
| `sense` | Datasette → Machine | Asserted (high) whenever a tape is mounted or recording is armed. Tells the KERNAL that a button is pressed. |

### Per-machine wiring summary

| Machine | Motor source | Write source | Read pulse destination |
|---------|-------------|--------------|------------------------|
| C64 | 6510 port bit 5 (active-low) | 6510 port bit 3 | CIA1 FLAG → `$DC0D` bit 4 |
| VIC-20 | VIA1 CA2 (manual low via PCR `$911C`) | VIA2 PB3 | VIA2 CA1 → `$912D` bit 1 (sense: VIA1 PA6, `$911F` bit 6) |
| PET 2001/4032/8032 | VIA CA2 (manual low via PCR) | VIA PB7 | PIA1 CA1 → `$E811` bit 7 |

---

## 2. Tape File Format

Tape images use the standard **C64-TAPE-RAW v1** format (`.tap`):

| Offset | Size | Content |
|--------|------|---------|
| 0 | 12 | Magic string `C64-TAPE-RAW` |
| 12 | 1 | Version (`0x01`) |
| 13 | 3 | Reserved |
| 16 | 4 | Data length in bytes (little-endian) |
| 20 | N | Pulse data |

Each pulse data byte encodes the time between leading edges in units of **8 CPU clock cycles**. A value of `0x00` is an overflow marker: the next three bytes give the actual 24-bit count in little-endian order.

---

## 3. C++ API

```cpp
#include "datasette.h"

// --- Playback ---
bool mountTape(const std::string& path);   // load a .tap file; asserts sense line
void play();                               // start playback (motor must be on)
void stop();                               // pause playback
void rewind();                             // seek to beginning

// --- Recording ---
bool startRecord();                        // arm recording; asserts sense line
void stopTapeRecord();                     // stop capturing; buffer retained in memory
bool saveTapeRecording(const std::string& path); // write buffer to .tap file

// --- Signal line wiring (called by the JSON machine loader) ---
void setSignalLine(const char* name, ISignalLine* line);
// Recognised names: "motor", "write", "readPulse", "sense"
```

The motor signal is sampled each `tick()`. Recording captures the `write` line level on every transition detected during ticking. Playback drives `readPulse` with a falling-then-rising edge for each stored pulse, at the timing encoded in the tape file.

---

## 4. CLI and GUI Controls

### CLI (`mmemu-cli`)

```
tape mount <path>       Mount a .tap file for playback.
tape play               Start playback (motor must be enabled by the machine).
tape stop               Pause playback.
tape rewind             Seek to the beginning.
tape record             Arm recording; asserts the sense line.
tape stoprecord         Stop capturing the write line.
tape save <path>        Write the captured buffer to a .tap file.
```

Typical save workflow:
```
tape record
(run the machine's SAVE command)
tape stoprecord
tape save output.tap
```

### GUI (Tape Pane)

The Tape Pane provides buttons for **Mount**, **Play**, **Stop**, **Rewind**, **Record**, and **Save**. Record arms capture; Save stops recording and opens a file-save dialog.

### MCP (`mmemu-mcp`)

- `mount_tape(machine_id, path)` — mount a `.tap` file.
- `control_tape(machine_id, operation)` — `"play"`, `"stop"`, `"rewind"`, `"record"`, `"stoprecord"`.
- `save_tape_recording(machine_id, path)` — save the captured buffer.

---

## 5. Source Layout

```
src/plugins/devices/datasette/
└── main/
    ├── datasette.h     — class declaration and signal interface
    └── datasette.cpp   — playback, recording, and .tap file I/O

src/plugins/cbm-loader/
└── main/
    ├── tap_parser.h    — TapArchive: .tap file read and write
    └── tap_parser.cpp

tests/src/
└── test_tape_roundtrip.cpp  — hardware-level roundtrip tests for C64, VIC-20, PET
```
