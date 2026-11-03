# mmemu-plugin-sid6581: MOS 6581 SID Implementation

The MOS 6581 Sound Interface Device (SID) is the audio synthesis chip used in the Commodore 64. It is implemented as an `IOHandler` and `IAudioOutput` plugin within `mmemu-plugin-sid6581.so`.

---

## 1. Intent

The SID produces audio by synthesizing three independent voices, passing them through a shared multi-mode filter, and mixing them with a master volume control. Each voice has a programmable oscillator, four selectable waveforms, and a full ADSR envelope generator.

---

## 2. Functionality

### 2.1 Voices

Each of the three voices contains:

**Oscillator:**
- 24-bit phase accumulator advancing by the FREQ register value each clock cycle
- Frequency in Hz = `FREQ × clockHz / 16777216`
- Four waveforms (any combination; see Combined Waveforms):

| Waveform | CR Bit | Description |
|----------|--------|-------------|
| Triangle | `CR_TRI` (bit 4) | Folded from phase accumulator (LSB grounded) |
| Sawtooth | `CR_SAW` (bit 5) | Direct MSBs of phase accumulator |
| Pulse | `CR_PULSE` (bit 6) | High when `phase > PW` (12-bit pulse width) |
| Noise | `CR_NOISE` (bit 7) | 23-bit LFSR (taps 22,17); clocked on bit-19 rising edge; output bits 11-4 = LFSR bits 20,18,14,11,9,5,2,0 |

**Combined Waveforms (#119):**
With several waveforms selected, each 0 output bit of a selected waveform grounds that line (an AND). When pulse, or saw and triangle together, are selected, the lines are also interconnected, so a grounded bit pulls neighbouring bits down too. The result is quieter than the AND and has its own timbre. With saw and triangle together the triangle's XOR is disabled, so the raw value is the saw ANDed with itself shifted left one bit.

mmsim uses the combined-waveform pulldown model by Antti Lankila and Leandro Nini (reSIDfp), with parameters fitted to samplings of real chips ([combined-waveforms](https://github.com/libsidplayfp/combined-waveforms)): 6581 R3 0486S and 8580 R5 1088. `CombinedWaveformTable` evaluates it once into 4096-entry tables for saw+triangle, pulse+triangle, pulse+saw, pulse+saw+triangle and pulse+noise. Pulse acts as a gate (combinations output 0 while it is low), and combinations with noise use the same model as without.

Against those chips' OSC3 samplings (all 4096 oscillator steps, 8 bits), the model mispredicts 124-1030 of 32768 bits per combination on the 6581 and 127-554 on the 8580, against 8665-14650 for a plain AND.

The chip model follows `setFilterVariant()` (0 = 6581, the default; 1 = 8580).

With noise combined with another waveform, a 0 output bit also clears the LFSR bit feeding it as the register shifts (noise writeback), so the noise dies away and stays silent ("noise lockup") until TEST reloads the LFSR.

Not modelled: the 6581's saw combinations pulling the accumulator MSB low, the 8580's half-cycle delay of the triangle/saw output on OSC3, and the fading of the waveform output when no waveform is selected.

**Control bits:**

| Bit | Mask | Description |
|-----|------|-------------|
| 0 | `CR_GATE` | Gate: 0→1 triggers attack; 1→0 triggers release |
| 1 | `CR_SYNC` | Hard sync: resets oscillator when sync source overflows |
| 2 | `CR_RING` | Ring modulation: triangle XOR'd with MSB of previous voice |
| 3 | `CR_TEST` | Test: freezes oscillator phase and loads LFSR with all-1s |

**ADSR Envelope Generator:**

| Phase | Trigger | Behavior |
|-------|---------|----------|
| Attack | Gate 0→1 | Linear ramp from the current level to 255 |
| Decay | After attack | Exponential-like decay toward the sustain level |
| Sustain | Level reaches `SUSTAIN × $11` | Hold |
| Release | Gate 1→0 | Exponential-like decay to 0 |

The envelope is clocked every cycle as in reSID (#123). A 15-bit rate counter counts up to the period for the A, D or R nibble (9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532 or 31251 cycles per step: 2 ms to 8 s for a full attack at 1 MHz). Decay and release are slowed further by an exponential counter, whose period changes to 1, 2, 4, 8, 16 and 30 as the level passes $FF, $5D, $36, $1A, $0E and $06. Reaching 0 freezes the counter until the next attack.

The rate counter is never reset. If a new, shorter period is set while the counter is already past it, the counter runs on to its 15-bit wraparound first, a delay of up to ~33 ms (the "ADSR delay bug" that players work around).

### 2.2 Filter

A state-variable filter, stepped every clock cycle as in reSID 0.16 (#120), processes voices routed to it via `$D417` bits 0–2:

| Parameter | Register | Description |
|-----------|----------|-------------|
| Cutoff frequency | `$D415`/`$D416` | 11-bit value; measured curve per chip (below) |
| Resonance | `$D417` bits 7–4 | 4-bit, 0–15 |
| LP enable | `$D418` bit 4 | Low-pass output mixed in |
| BP enable | `$D418` bit 5 | Band-pass output mixed in |
| HP enable | `$D418` bit 6 | High-pass output mixed in |
| Voice 3 off | `$D418` bit 7 | Disconnects voice 3 from mixed output |

Voices not routed to the filter bypass it and are summed directly into the output.

**Cutoff curve.** The cutoff frequency comes from points measured on real chips (reSID 0.16, Dag Lem), interpolated linearly:

- **6581:** strongly nonlinear. About 220 Hz up to FC $100, rising steeply to 6 kHz at $3FF, then dropping to 4.6 kHz at $400 (the register's top bit switches part of the circuit) and rising to 18 kHz at $7FF.
- **8580:** close to linear, 0 to 12.5 kHz.

Above 16 kHz the filter is held at 16 kHz, as in reSID, for stability.

**Resonance:** Q = 0.707 + res / 15.

**Saturation (#121).** The filter's integrators can only swing as far as the chip's op-amps. reSID 1.0's op-amp transfer curves, measured on a 6581R4AR and an 8580R5, put that at about 3.73 V from the working point on the 6581, against about 1.5 V for one voice, and 3.51 V against 1.0 V on the 8580. The band-pass and low-pass states are limited softly (linear to 75 %, then bending towards the limit), so loud resonant sounds on the 6581, such as several voices through the band-pass at high resonance, compress and distort. The 8580 has more headroom and rarely clips.

### 2.3 Volume and Output

Each voice's output is (waveform − zero level) × envelope + DC. The mixer sums the unfiltered voices, the selected filter outputs and its own DC offset, and the master volume (`$D418` bits 3–0) multiplies the total.

**DC offsets and volume-register samples (#122).** These are reSID's measured offsets:

| | Waveform zero | Voice DC | Mixer DC |
|---|---|---|---|
| 6581 | $380 | $800 × $FF | −($FFF × $FF / 18) |
| 8580 | $800 | 0 | 0 |

Because the master volume multiplies the DC as well, writing `$D418` changes the output level even when every voice is silent. That is how many tunes play samples: drums, speech and 4-bit "digis". On the 6581 these play as on hardware. On the 8580 they are silent, as they are on a real 8580 without the usual hardware modification. Setting the volume also gives the familiar click.

**External output stage:** as in reSID, the C64's RC filters follow the mixer: a ~16 kHz low-pass and a ~16 Hz high-pass that removes the DC.

**Resampling:** the per-cycle output is averaged over each output sample period. Samples are pushed into an 8192-sample ring buffer at the configured sample rate. Full scale is three voices at full volume, as in reSID.

### 2.4 Read-Only Registers

| Offset | Register | Description |
|--------|----------|-------------|
| $19 | POTX | Paddle X (returns $FF — A/D not implemented) |
| $1A | POTY | Paddle Y (returns $FF) |
| $1B | OSC3 | Upper 8 bits of voice 3's waveform output |
| $1C | ENV3 | Voice 3 envelope level |

### 2.5 Audio Output (IAudioOutput Pull Model)

```cpp
sid->setSampleRate(44100);
sid->setClockHz(985248);    // PAL; NTSC = 1022727
```

`tick(cycles)` synthesizes samples into the ring buffer. The host audio layer drains it:

```cpp
int n = sid->pullSamples(buffer, maxSamples);
```

### 2.6 Inspection and the SID Scope pane (IAudioInspect, #30)

`SID6581` and the MEGA65's `SidPair` implement `IAudioInspect`
(`src/libdevices/main/iaudio_inspect.h`), which debugger tools use without
knowing the chip type:

- `voiceInfo(chip, voice, out)`: frequency register and Hz, pulse width,
  control register, ADSR nibbles, envelope phase and level, filter routing,
  and whether voice 3 is disconnected ($D418 bit 7).
- `scopeSamples(chip, channel, out, n)`: the latest samples, oldest first, at
  the audio sample rate. Channels 0-2 are the voices (waveform x envelope,
  before the filter; voice 3 is recorded even when disconnected) and channel
  3 is the mixed output after the filter and volume. The history holds 4096
  samples (~93 ms at 44.1 kHz) and is cleared on reset.

`SidPair` reports two chips, `SID1 $D400` and `SID2 $D420`.

The GUI's **SID Scope** pane (View > SID Scope) draws one trace per voice and
one for the output. Controls:

| Control | Meaning |
|---------|---------|
| Chip | Which SID to show, when the machine has more than one |
| Timebase | Time across each trace: 2, 5, 10, 20 or 50 ms |
| Triggered / Free run | Triggered holds a rising zero crossing at the left edge of each trace, so steady tones stand still; free run shows the latest samples |

Each voice row shows its waveform and modifiers (Sync, Ring, Test, Gate), the
frequency with the nearest note and its offset in cents, the pulse width, the
ADSR settings, the envelope phase and level, and Filter / Off flags. On the
right, an ADSR diagram highlights the current phase and a bar shows the
envelope level. The pane updates while the machine runs and after each step.
The trigger search, note names and envelope times are in
`src/libdebug/main/audio_scope.cpp`.

#### MCP: `sid_scope`

The same scope as an image, drawn by `scope::renderScope()` (no GUI needed):

| Argument | Meaning |
|----------|---------|
| `machine_id` | Required |
| `chip` | Sound chip index (default 0; MEGA65 SID1 = 0, SID2 = 1) |
| `timebase_ms` | Time across each trace, 1-80 (default 20) |
| `trigger` | `rising` (default) or `free` |
| `path` | `.png` or `.gif`; without it only the voice state is returned |
| `frames` | 1-300 (default 1). More than 1 runs the machine between frames and writes an animated, looping GIF |
| `frame_ms` | Emulated ms between frames, 5-1000 (default 40); also the GIF frame delay |
| `width`, `row_height` | Image size: width 160-1600 (default 480), each of the 4 rows 40-400 (default 100) |

The reply lists each voice's state, e.g.
`Voice 1: Pulse Gate, 440.0 Hz (A4 +0c), PW 50.0%, ADSR 0/9/A/5, Sustain 170`.
Frames are spaced by the chip's sample count (`IAudioInspect::scopeTotal()`),
so they are in emulated time whatever the CPU speed. The first frame shows the
state before running. Capture stops early at a breakpoint, or if the chip
stops producing samples. GIFs are written by `src/libdebug/main/gif_writer.cpp`
(palette + LZW, no quantisation: the scope uses only a few colours).

#### CLI: `sid voices` and `sid scope`

```
sid voices [chip]
sid scope <file.png|file.gif> [frames N] [ms N] [timebase N] [free] [chip N] [width N] [height N]
```

`sid voices` prints the same voice table as the MCP tool, e.g. after `step`
or at a breakpoint. `sid scope` writes the same image: `ms` is `frame_ms`,
`timebase` is `timebase_ms`, `free` selects free run, and `height` is the row
height. More than one frame needs a `.gif` and runs the machine; a
breakpoint or Ctrl-C ends the capture early. Both front ends share
`scope::listChips()`, `scope::capture()` and `scope::writeCapture()`.

```
> sid scope song.gif frames 50 ms 40
SID $D400
Voice 1: Pulse Gate, 440.0 Hz (A4 +0c), PW 50.0%, ADSR 0/9/A/5, Sustain 170
...
Saved 50-frame 480x400 GIF (40 ms emulated per frame) to song.gif
```

---

### 2.7 Playing .sid files (PSID/RSID)

CLI `sid load <file.sid> [subtune]` and MCP `load_sid` (`file`, optional `subtune`) load a tune into a C64 and install a play driver, as a SID player does. Running the machine then plays it; `recordaudio <file.wav> <ms>` (MCP `record_audio`) captures it.

```
> sid load Commando.sid 1
PSID v2: Commando by Rob Hubbard (1985 Elite)
Songs: 11, playing subtune 1
Load: $5000-$5F9F, Init: $5F80, Play: $5012
Timing: 50 Hz (one PAL frame), SID 6581
Play driver installed at $0334; run the machine to play. ('recordaudio' captures it.)
> recordaudio commando.wav 60000
```

How the driver (`src/libdebug/main/sid_file.cpp`) plays the tune:

- **Init:** called with the subtune in A. `$01` is set from init's address as sidplayfp does: `$37` below `$A000`, `$36` below `$D000`, `$35` from `$E000`, else `$34`.
- **Play:** called once per CIA 1 timer A period, with interrupts off.
  - VBI songs: one PAL frame (19656 cycles, 50 Hz).
  - CIA-speed songs (the header's speed bit, and RSID): the tune's own timer value, or 60 Hz if init sets none.
- **No play address:** the tune installs its own interrupt handler, so interrupts are enabled after init and the driver idles.
- **Placement:** the driver goes in RAM the tune doesn't use: `$0334`, else `$0200`, else the first free 64 bytes from `$C000` or `$1000`.

Before this, the loader called play back to back as fast as the CPU ran, so tunes played far too fast. It also put its loop in zero page (`$0002`), which tunes use.

Checked against sidplayfp for Rob Hubbard's *Commando* and *Monty on the Run*: tempo and pitch match. The frequency balance matches reSID 0.16 within about 1 dB per band; reSIDfp has a little more bass from its transistor-level 6581 filter.

Not supported: data loaded into `$D000-$DFFF`, which the loader writes through the I/O area; RSID tunes that need a complete KERNAL environment; and multi-SID PSIDs, which are played on the first SID only.

## 3. Register Map

Each voice occupies 7 bytes (voice N at base + N×7):

| Offset | Symbol | Description |
|--------|--------|-------------|
| +$0 | FREQ_LO | Frequency low byte |
| +$1 | FREQ_HI | Frequency high byte |
| +$2 | PW_LO | Pulse width low byte |
| +$3 | PW_HI | Pulse width high nibble |
| +$4 | CR | Control register (gate/sync/ring/test/waveform) |
| +$5 | AD | Attack (bits 7–4) / Decay (bits 3–0) |
| +$6 | SR | Sustain level (bits 7–4) / Release (bits 3–0) |

Shared registers at base + $15:

| Offset | Symbol | Description |
|--------|--------|-------------|
| $15 | FC_LO | Filter cutoff low 3 bits |
| $16 | FC_HI | Filter cutoff high 8 bits |
| $17 | RES_FILT | Resonance (7–4) + filter routing (2–0) |
| $18 | MODE_VOL | Filter mode (6–4) + voice 3 off (7) + volume (3–0) |
| $19 | POTX | Paddle X (read-only) |
| $1A | POTY | Paddle Y (read-only) |
| $1B | OSC3 | Voice 3 oscillator MSB (read-only) |
| $1C | ENV3 | Voice 3 envelope level (read-only) |

---

## 4. Dependencies

- **Depends on**: `IAudioOutput` (from `libdevices`)
- **Required by**: C64 Machine Plugin (Phase 11.6)

---

## 5. Implementation Details

- **Source Location**: `src/plugins/devices/sid6581/main/sid6581.h/cpp`
- **Plugin**: `lib/mmemu-plugin-sid6581.so`
- **Class**: `SID6581 : public IOHandler, public IAudioOutput`
- **Registration Name**: `"6581"`
- **Base Address**: `0xD400`
- **Address Mask**: `0x001F` (32-byte window)
- **Clock Default**: 985 248 Hz (PAL); call `setClockHz(1022727)` for NTSC
- **Sample Rate Default**: 44 100 Hz
- **Ring Buffer**: 8192 float32 mono samples
- **Timing (#123)**: oscillators, noise LFSR (shifted when accumulator bit 19 rises), hard sync, envelopes, filter and external filter are all clocked every cycle. The output therefore doesn't depend on how the host splits time into `tick()` calls. Register writes take effect at the cycle the CPU instruction completes. A C64 at max speed runs about 11 times real time with this.
- **Chip model**: `setFilterVariant(0)` selects the 6581 (default), `1` the 8580. This affects combined waveforms, the cutoff curve, filter headroom and the DC offsets.
- **Not modelled**: reSID 1.0 / reSIDfp's transistor-level 6581 filter (its exact cutoff-dependent distortion and resonance shape), the 8580's half-cycle OSC3 delay, waveform DAC nonlinearity, and floating-waveform fade-out.
