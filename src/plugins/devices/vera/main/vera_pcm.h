#pragma once

// VERA PCM: the audio FIFO and its playback.
//
// Ported from the Commander X16 emulator (x16-emulator, src/vera_pcm.c),
// Copyright (c) 2020 Frank van den Hoef, 2-clause BSD license.

#include <cstdint>

/**
 * The VERA PCM player: a 4 KB FIFO ($9F3D) played at RATE/128 of the VERA
 * audio rate, 8 or 16 bit, mono or stereo, with a 4-bit volume
 * (AUDIO_CTRL $9F3B, AUDIO_RATE $9F3C).
 */
class VeraPcm {
public:
    /** The hardware FIFO is 4 KB but only 4095 bytes are usable. */
    static constexpr unsigned FIFO_SIZE = 4096 - 1;

    VeraPcm() { reset(); }

    void    reset();
    void    writeCtrl(uint8_t val);
    uint8_t readCtrl() const;
    void    writeRate(uint8_t val) { m_rate = val; }
    uint8_t readRate() const { return m_rate; }
    void    writeFifo(uint8_t val);
    /** One stereo output sample. */
    void    render(int16_t* left, int16_t* right);
    /** AFLOW: the FIFO is less than 1/4 full. */
    bool    fifoAlmostEmpty() const { return m_fifoCnt < 1024; }
    unsigned fifoCount() const { return m_fifoCnt; }

private:
    void    fifoReset() { m_wrIdx = m_rdIdx = m_fifoCnt = 0; }
    uint8_t readFifo();

    uint8_t  m_fifo[FIFO_SIZE] = {};
    unsigned m_wrIdx = 0, m_rdIdx = 0, m_fifoCnt = 0;
    uint8_t  m_ctrl = 0;
    uint8_t  m_rate = 0;
    int16_t  m_curL = 0, m_curR = 0;
    uint8_t  m_phase = 0;
};
