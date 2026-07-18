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
- Four waveforms (any combination active, outputs ANDed):

| Waveform | CR Bit | Description |
|----------|--------|-------------|
| Triangle | `CR_TRI` (bit 4) | Folded from phase accumulator |
| Sawtooth | `CR_SAW` (bit 5) | Direct MSBs of phase accumulator |
| Pulse | `CR_PULSE` (bit 6) | High when `phase > PW` (12-bit pulse width) |
| Noise | `CR_NOISE` (bit 7) | 23-bit LFSR (taps 22,17); clocked on bit-19 rising edge |

**Combined Waveforms:**
When multiple waveforms are selected simultaneously, the real 6581 produces complex harmonic patterns through analog bit-mixing in the DAC stage. This is NOT a simple AND operation.
- Physical mechanism: open-collector NMOS transistor architecture with asymmetric driving strength
- Output amplitude: typically 30-40% lower than AND operation
- Tonal characteristics: triangle+pulse produces bell-like tones; sawtooth+pulse produces hollow/nasal tones; triangle+sawtooth produces warm tones
- Implementation: empirically-derived lookup tables based on real hardware measurements (reSIDfp model)
- Chip variants: 6581 exhibits more bit-mixing effects than 8580 due to circuit layout differences

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
| Attack | Gate 0→1 | Linear ramp from 0 to 255 |
| Decay | After attack | Linear decay from 255 toward sustain level |
| Sustain | During decay target | Hold at `SUSTAIN × 17` (0–240) |
| Release | Gate 1→0 | Linear decay from current level to 0 |

Attack/decay/release rates are set via nibbles in the AD and SR registers, spanning from ~2 ms to ~31 s.

### 2.2 Filter

A Chamberlin state-variable filter processes voices routed to it via `$D417` bits 0–2:

| Parameter | Register | Description |
|-----------|----------|-------------|
| Cutoff frequency | `$D415`/`$D416` | 11-bit value; range ~30 Hz – 12 kHz |
| Resonance | `$D417` bits 7–4 | 4-bit, 0–15 |
| LP enable | `$D418` bit 4 | Low-pass output mixed in |
| BP enable | `$D418` bit 5 | Band-pass output mixed in |
| HP enable | `$D418` bit 6 | High-pass output mixed in |
| Voice 3 off | `$D418` bit 7 | Disconnects voice 3 from mixed output |

Voices not routed to the filter bypass it and are summed directly into the output.

### 2.3 Volume and Output

Master volume (`$D418` bits 3–0, 0–15) scales the final mono float32 sample. Samples are pushed into an 8192-sample ring buffer at the configured sample rate.

### 2.4 Read-Only Registers

| Offset | Register | Description |
|--------|----------|-------------|
| $19 | POTX | Paddle X (returns $FF — A/D not implemented) |
| $1A | POTY | Paddle Y (returns $FF) |
| $1B | OSC3 | Voice 3 phase accumulator MSB |
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

---

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
