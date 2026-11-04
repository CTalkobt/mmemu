// VERA PSG.
//
// Ported from the Commander X16 emulator (x16-emulator, src/vera_psg.c),
// Copyright (c) 2020 Frank van den Hoef. All rights reserved.
// License: 2-clause BSD.

#include "vera_psg.h"

namespace {
const uint16_t VOLUME_LUT[64] = {
      0,                                           4,   8,  12,
     16,  17,  18,  20,  21,  22,  23,  25,  26,  28,  30,  31,
     33,  35,  37,  40,  42,  45,  47,  50,  53,  56,  60,  63,
     67,  71,  75,  80,  85,  90,  95, 101, 107, 113, 120, 127,
    135, 143, 151, 160, 170, 180, 191, 202, 214, 227, 241, 255,
    270, 286, 303, 321, 341, 361, 382, 405, 429, 455, 482, 511
};
}  // namespace

void VeraPsg::reset() {
    for (auto& c : m_ch) c = Channel();
    m_noiseState = 1;
}

void VeraPsg::writeReg(uint8_t reg, uint8_t val) {
    reg &= 0x3f;
    int ch  = reg / 4;
    int idx = reg & 3;
    switch (idx) {
        case 0: m_ch[ch].freq = (uint16_t)((m_ch[ch].freq & 0xFF00) | val); break;
        case 1: m_ch[ch].freq = (uint16_t)((m_ch[ch].freq & 0x00FF) | (val << 8)); break;
        case 2:
            m_ch[ch].right  = (val & 0x80) != 0;
            m_ch[ch].left   = (val & 0x40) != 0;
            m_ch[ch].volume = VOLUME_LUT[val & 0x3F];
            break;
        case 3:
            m_ch[ch].pw       = val & 0x3F;
            m_ch[ch].waveform = val >> 6;
            break;
    }
}

void VeraPsg::render(int16_t* left, int16_t* right) {
    int16_t l = 0;
    int16_t r = 0;

    for (int i = 0; i < 16; i++) {
        // In the FPGA, noise values are generated every system clock and the
        // channels are updated sequentially, so two channels fetching noise in
        // the same sample get different values.
        m_noiseState = (uint16_t)((m_noiseState << 1) |
            (((m_noiseState >> 1) ^ (m_noiseState >> 2) ^ (m_noiseState >> 4) ^ (m_noiseState >> 15)) & 1));

        Channel* ch = &m_ch[i];

        uint32_t newPhase = (ch->left || ch->right) ? ((ch->phase + ch->freq) & 0x1FFFF) : 0;
        if ((ch->phase & 0x10000) && !(newPhase & 0x10000)) {
            ch->noiseval = (m_noiseState >> 1) & 0x3F;
        }
        ch->phase = newPhase;

        uint32_t v = 0;
        switch (ch->waveform) {
            case WF_PULSE:    v = ((ch->phase >> 10) > ch->pw) ? 0 : 0x3F; break;
            case WF_SAWTOOTH: v = (ch->phase >> 11) ^ ((ch->pw ^ 0x3f) & 0x3f); break;
            case WF_TRIANGLE:
                v = ((ch->phase & 0x10000) ? (~(ch->phase >> 10) & 0x3F) : ((ch->phase >> 10) & 0x3F))
                    ^ ((ch->pw ^ 0x3f) & 0x3f);
                break;
            case WF_NOISE:    v = ch->noiseval; break;
        }
        int16_t sv = (int16_t)(v ^ 0x20);
        if (sv & 0x20) sv = (int16_t)(sv | 0xFFC0);

        int16_t val = (int16_t)(sv * ch->volume);

        if (ch->left)  l = (int16_t)(l + (val >> 3));
        if (ch->right) r = (int16_t)(r + (val >> 3));
    }

    *left  = l;
    *right = r;
}
