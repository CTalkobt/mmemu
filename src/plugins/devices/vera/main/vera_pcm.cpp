// VERA PCM.
//
// Ported from the Commander X16 emulator (x16-emulator, src/vera_pcm.c),
// Copyright (c) 2020 Frank van den Hoef. All rights reserved.
// License: 2-clause BSD.

#include "vera_pcm.h"

namespace {
const uint8_t VOLUME_LUT[16] = {0, 1, 2, 3, 4, 5, 6, 8, 11, 14, 18, 23, 30, 38, 49, 64};
}

void VeraPcm::reset() {
    fifoReset();
    m_ctrl = 0;
    m_rate = 0;
    m_curL = m_curR = 0;
    m_phase = 0;
}

void VeraPcm::writeCtrl(uint8_t val) {
    if (val & 0x80) fifoReset();
    m_ctrl = val & 0x3F;
}

uint8_t VeraPcm::readCtrl() const {
    uint8_t result = m_ctrl;
    // As in the emulator, writeFifo() stops at FIFO_SIZE - 1 bytes, so the
    // FULL flag (bit 7) is never seen; the docs say a full FIFO sets it.
    if (m_fifoCnt == FIFO_SIZE) result |= 0x80;
    if (m_fifoCnt == 0) result |= 0x40;
    return result;
}

void VeraPcm::writeFifo(uint8_t val) {
    if (m_fifoCnt < FIFO_SIZE - 1) {
        m_fifo[m_wrIdx++] = val;
        if (m_wrIdx == FIFO_SIZE) m_wrIdx = 0;
        m_fifoCnt++;
    }
}

uint8_t VeraPcm::readFifo() {
    if (m_fifoCnt == 0) return 0;
    uint8_t result = m_fifo[m_rdIdx++];
    if (m_rdIdx == FIFO_SIZE) m_rdIdx = 0;
    m_fifoCnt--;
    return result;
}

void VeraPcm::render(int16_t* left, int16_t* right) {
    uint8_t oldPhase = m_phase;
    m_phase = (uint8_t)(m_phase + m_rate);
    if ((oldPhase & 0x80) != (m_phase & 0x80)) {
        if (m_fifoCnt == 0) {
            m_curL = 0;
            m_curR = 0;
        } else {
            switch ((m_ctrl >> 4) & 3) {
                case 0:   // mono 8-bit
                    m_curL = (int16_t)(readFifo() << 8);
                    m_curR = m_curL;
                    break;
                case 1:   // stereo 8-bit
                    if (m_fifoCnt < 2) {
                        m_fifoCnt = 0;
                        m_rdIdx = m_wrIdx;
                    } else {
                        m_curL = (int16_t)(readFifo() << 8);
                        m_curR = (int16_t)(readFifo() << 8);
                    }
                    break;
                case 2:   // mono 16-bit
                    if (m_fifoCnt < 2) {
                        m_fifoCnt = 0;
                        m_rdIdx = m_wrIdx;
                    } else {
                        uint16_t lo = readFifo();
                        m_curL = (int16_t)(lo | (readFifo() << 8));
                        m_curR = m_curL;
                    }
                    break;
                case 3:   // stereo 16-bit
                    if (m_fifoCnt < 4) {
                        m_fifoCnt = 0;
                        m_rdIdx = m_wrIdx;
                    } else {
                        uint16_t lo = readFifo();
                        m_curL = (int16_t)(lo | (readFifo() << 8));
                        lo = readFifo();
                        m_curR = (int16_t)(lo | (readFifo() << 8));
                    }
                    break;
            }
        }
    }
    *left  = (int16_t)((int32_t)m_curL * VOLUME_LUT[m_ctrl & 0xF] / 64);
    *right = (int16_t)((int32_t)m_curR * VOLUME_LUT[m_ctrl & 0xF] / 64);
}
