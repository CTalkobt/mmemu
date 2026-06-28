# Commodore Plus/4 & 264 Series Implementation Architecture & Plan

This document outlines the architecture and implementation roadmap for adding full Commodore Plus/4, C16, and C116 (Commodore 264 series) simulation support to `mmsim` (`mmemu`).

---

## 1. Architectural Overview

The Commodore 264 series architecture centers around a highly integrated custom chip (the TED) combined with an 8-bit CPU derivative and modular ROM banking.

### Core Components
1. **MOS 7501 / 8501 CPU**: An NMOS 6502 derivative featuring an on-chip 6-bit directional I/O port at `$00/$01` (used for cassette motor/sense and memory banking controls). Supports dynamic clock switching between 0.89 MHz (display active) and 1.76 MHz (burst/blanking mode) driven by the TED.
2. **MOS 7360 / 8360 TED (Text Editing Device)**: An all-in-one chip that handles:
   - **Video Output**: 40×25 text and bitmap modes with a unique 121-color palette.
   - **Audio Output**: 2 sound channels (square wave and digital noise).
   - **Timers**: Three 16-bit interval timers.
   - **I/O & Banking**: Keyboard matrix scanning, joystick reading, and memory banking control.
3. **MOS 6551 ACIA**: Asynchronous Communications Interface Adapter mapped at `$FD00–$FD0F` providing RS-232 serial communication capabilities (present on Plus/4).
4. **Memory Layout**:
   - **RAM**: 64 KB (`$0000–$FDFF`) on Plus/4; 16 KB (`$0000–$3FFF`) on C16/C116.
   - **ROMs**: BASIC v3.5 (16 KB at `$8000–$BFFF`), KERNAL (16 KB at `$E000–$FFFF`), 3Plus1 / Function ROMs (C1/C2 at `$C000–$FCFF`).
   - **Bank Switching**: Triggered by dummy accesses to specific I/O address ranges (`$FDD0–$FDEF`) or TED banking registers.

---

## 2. Implementation Roadmap

### Phase 25.1: MOS 7501 / 8501 CPU Core
*Goal: Implement the MOS 7501/8501 CPU core with dynamic clock speed scaling.*

- [ ] **`MOS8501` Core** (`src/plugins/devices/plus4/cpu8501/cpu8501.h/cpp`):
  - Inherit from `MOS6510` (or `MOS6502`).
  - Implement 6-bit on-chip I/O port at `$00/$01` (Direction and Data registers).
  - Add dynamic clock speed scaling hook (0.89 MHz standard, 1.76 MHz fast mode during border/blanking intervals).

### Phase 25.2: MOS 7360 / 8360 TED Core & Register File
*Goal: Implement the TED register file, interval timers, and I/O latching.*

- [ ] **`TED7360` Core** (`src/plugins/devices/ted7360/ted7360.h/cpp`):
  - Implement `TED7360` inheriting from `IOHandler` spanning `$FF00–$FF3F`.
  - Complete 64-register file specification.
  - Three 16-bit interval timers (Timer 1, Timer 2, Timer 3) with reload latches and interrupt generation (`$FF09` status / `$FF0A` mask). Timer 3 drives system clock and cursor blink.
  - Keyboard matrix scanning latches (`$FF08`/`$FF09`) and joystick port multiplexing.

### Phase 25.3: TED Video Engine & 121-Color Renderer
*Goal: Implement TED video raster pipeline, display modes, and color generation.*

- [ ] **`TedVideo` Renderer** (`src/plugins/devices/ted_video/ted_video.h/cpp`):
  - Implement `TedVideo` inheriting from `IVideoOutput`.
  - Build 121-color RGBA lookup table (15 hues × 8 luminance levels + black).
  - Support video modes: Standard text, multicolor text, extended background color, standard bitmap, and multicolor bitmap.
  - Cycle-accurate raster counter (`$FF1C–$FF1D`) and compare IRQ.
  - Horizontal and vertical smooth scrolling (`$FF07` / `$FF06`).
  - Frame buffer rendering (`renderFrame()`).

### Phase 25.4: TED Audio Subsystem
*Goal: Implement 2-channel TED sound synthesis.*

- [ ] **`TedAudio` Generator** (`src/plugins/devices/ted_audio/ted_audio.h/cpp`):
  - Implement sound generation satisfying `IAudioOutput`.
  - Channel 1: 10-bit frequency divider square wave (`$FF0E`/`$FF10`).
  - Channel 2: 10-bit frequency divider square wave or 16-bit LFSR white noise (`$FF0F`/`$FF10` bit 7).
  - Master volume control (`$FF11` bits 3:0) and 16-bit PCM sample mixing.

### Phase 25.5: MOS 6551 ACIA Communications Adapter
*Goal: Implement serial RS-232 communications interface.*

- [ ] **`ACIA6551` Device** (`src/plugins/devices/acia6551/acia6551.h/cpp`):
  - Implement `ACIA6551` inheriting from `IOHandler` mapped at `$FD00–$FD0F`.
  - Registers: Data (`$FD00`), Status (`$FD01`), Command (`$FD02`), Control (`$FD03`).
  - Baud rate generator supporting 50 to 19200 baud.
  - Transmit/Receive interrupt handling.

### Phase 25.6: Plus/4 Memory Map & Machine Factory
*Goal: Assemble all components into machine descriptors for Plus/4, C16, and C116.*

- [ ] **Machine Descriptors** (`src/plugins/machines/plus4/machine_plus4.cpp`):
  - Create machine descriptors for `"plus4"`, `"c16"`, and `"c116"`.
  - Configure RAM size: 64 KB for Plus/4, 16 KB for C16/C116.
  - Load system ROMs: BASIC v3.5 (16 KB), KERNAL (16 KB), and 3Plus1 Function ROMs.
  - Implement dummy address banking decoder for `$FDD0–$FDEF` to swap RAM and ROMs at `$8000–$FCFF`.
  - Map 75-key keyboard matrix (including dedicated arrow keys and function keys).

### Phase 25.7: Plus/4 Integration Tests
*Goal: Automated validation of the Plus/4 simulation stack.*

- [ ] **Integration Test Suite** (`tests/test_plus4.cpp`):
  - TED color palette verification: Validate RGBA outputs across luminance levels.
  - TED timer IRQ verification.
  - Dummy address bank switching test: Verify RAM/ROM visibility changes at `$8000`.
  - ACIA loopback test.
  - Boot to BASIC 3.5 prompt integration test.
