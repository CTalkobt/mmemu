#pragma once

// VERA PSG: 16-voice programmable sound generator.
//
// Ported from the Commander X16 emulator (x16-emulator, src/vera_psg.c),
// Copyright (c) 2020 Frank van den Hoef, 2-clause BSD license.

#include <cstdint>

/**
 * The VERA PSG. Its 64 registers live in VRAM at $1F9C0-$1F9FF (four per
 * voice: frequency low/high, right/left/volume, waveform/pulse width).
 * render() produces one stereo sample at the VERA audio rate (25 MHz / 512).
 */
class VeraPsg {
public:
    enum Waveform { WF_PULSE = 0, WF_SAWTOOTH, WF_TRIANGLE, WF_NOISE };

    struct Channel {
        uint16_t freq = 0;
        uint16_t volume = 0;   ///< after the volume look-up table (0-511)
        bool     left = false, right = false;
        uint8_t  pw = 0;
        uint8_t  waveform = 0;
        uint16_t noiseval = 0;
        uint32_t phase = 0;
    };

    VeraPsg() { reset(); }

    void reset();
    /** A write to PSG register reg (0-63). */
    void writeReg(uint8_t reg, uint8_t val);
    /** One sample of all 16 voices. */
    void render(int16_t* left, int16_t* right);

    const Channel& channel(int i) const { return m_ch[i & 15]; }

private:
    Channel  m_ch[16];
    uint16_t m_noiseState = 1;
};
