// The SNES S-DSP after Mesen2 (Core/SNES/DSP/), GPL-3.0, Sour and the Mesen
// contributors; after blargg's SPC_DSP and the fullsnes documentation.

#include "snes_dsp.h"

#include <cstring>

namespace snes {

namespace {

// Register numbers
enum : int {
    VOLL = 0, VOLR = 1, PITCHL = 2, PITCHH = 3, SRCN = 4, ADSR1 = 5, ADSR2 = 6, GAIN = 7, ENVX = 8, OUTX = 9,
    MVOLL = 0x0C, MVOLR = 0x1C, EVOLL = 0x2C, EVOLR = 0x3C, KON = 0x4C, KOFF = 0x5C, FLG = 0x6C, ENDX = 0x7C,
    EFB = 0x0D, PMON = 0x2D, NON = 0x3D, EON = 0x4D, DIR = 0x5D, ESA = 0x6D, EDL = 0x7D, FIR0 = 0x0F,
};

const int16_t GAUSS[512] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,   2,   2,   2,
    2,   2,   3,   3,   3,   3,   3,   4,   4,   4,   4,   4,   5,   5,   5,   5,
    6,   6,   6,   6,   7,   7,   7,   8,   8,   8,   9,   9,   9,  10,  10,  10,
    11,  11,  11,  12,  12,  13,  13,  14,  14,  15,  15,  15,  16,  16,  17,  17,
    18,  19,  19,  20,  20,  21,  21,  22,  23,  23,  24,  24,  25,  26,  27,  27,
    28,  29,  29,  30,  31,  32,  32,  33,  34,  35,  36,  36,  37,  38,  39,  40,
    41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,
    58,  59,  60,  61,  62,  64,  65,  66,  67,  69,  70,  71,  73,  74,  76,  77,
    78,  80,  81,  83,  84,  86,  87,  89,  90,  92,  94,  95,  97,  99, 100, 102,
    104, 106, 107, 109, 111, 113, 115, 117, 118, 120, 122, 124, 126, 128, 130, 132,
    134, 137, 139, 141, 143, 145, 147, 150, 152, 154, 156, 159, 161, 163, 166, 168,
    171, 173, 175, 178, 180, 183, 186, 188, 191, 193, 196, 199, 201, 204, 207, 210,
    212, 215, 218, 221, 224, 227, 230, 233, 236, 239, 242, 245, 248, 251, 254, 257,
    260, 263, 267, 270, 273, 276, 280, 283, 286, 290, 293, 297, 300, 304, 307, 311,
    314, 318, 321, 325, 328, 332, 336, 339, 343, 347, 351, 354, 358, 362, 366, 370,
    374, 378, 381, 385, 389, 393, 397, 401, 405, 410, 414, 418, 422, 426, 430, 434,
    439, 443, 447, 451, 456, 460, 464, 469, 473, 477, 482, 486, 491, 495, 499, 504,
    508, 513, 517, 522, 527, 531, 536, 540, 545, 550, 554, 559, 563, 568, 573, 577,
    582, 587, 592, 596, 601, 606, 611, 615, 620, 625, 630, 635, 640, 644, 649, 654,
    659, 664, 669, 674, 678, 683, 688, 693, 698, 703, 708, 713, 718, 723, 728, 732,
    737, 742, 747, 752, 757, 762, 767, 772, 777, 782, 787, 792, 797, 802, 806, 811,
    816, 821, 826, 831, 836, 841, 846, 851, 855, 860, 865, 870, 875, 880, 884, 889,
    894, 899, 904, 908, 913, 918, 923, 927, 932, 937, 941, 946, 951, 955, 960, 965,
    969, 974, 978, 983, 988, 992, 997,1001,1005,1010,1014,1019,1023,1027,1032,1036,
    1040,1045,1049,1053,1057,1061,1066,1070,1074,1078,1082,1086,1090,1094,1098,1102,
    1106,1109,1113,1117,1121,1125,1128,1132,1136,1139,1143,1146,1150,1153,1157,1160,
    1164,1167,1170,1174,1177,1180,1183,1186,1190,1193,1196,1199,1202,1205,1207,1210,
    1213,1216,1219,1221,1224,1227,1229,1232,1234,1237,1239,1241,1244,1246,1248,1251,
    1253,1255,1257,1259,1261,1263,1265,1267,1269,1270,1272,1274,1275,1277,1279,1280,
    1282,1283,1284,1286,1287,1288,1290,1291,1292,1293,1294,1295,1296,1297,1297,1298,
    1299,1300,1300,1301,1302,1302,1303,1303,1303,1304,1304,1304,1304,1304,1305,1305,
};

int16_t clamp16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v; }

int16_t gauss(int32_t pos, const int16_t* s, uint8_t bufferPos) {
    const uint8_t p = (uint8_t)((pos >> 12) + bufferPos);
    const uint8_t off = (uint8_t)((pos >> 4) & 0xFF);
    // "The above 3 wrap at 15 bits signed. The last is added to that, and is clamped rather than wrapped."
    const int32_t out = (int16_t)(((GAUSS[255 - off] * (int32_t)s[p % 12]) >> 11) +
                                  ((GAUSS[511 - off] * (int32_t)s[(p + 1) % 12]) >> 11) +
                                  ((GAUSS[256 + off] * (int32_t)s[(p + 2) % 12]) >> 11)) +
                        ((GAUSS[off] * (int32_t)s[(p + 3) % 12]) >> 11);
    return (int16_t)(clamp16(out) & ~1);
}

}  // namespace

Dsp::Dsp(uint8_t* ram) : m_ram(ram) {
    for (int i = 0; i < 8; ++i) { m_voices[i].index = i; m_voices[i].bit = (uint8_t)(1 << i); }
    reset();
}

void Dsp::reset() {
    std::memset(m_regs, 0, sizeof m_regs);
    std::memset(m_ext, 0, sizeof m_ext);
    for (auto& v : m_voices) {
        const int i = v.index;
        v = Voice{};
        v.index = i;
        v.bit = (uint8_t)(1 << i);
    }
    m_regs[FLG] = m_ext[FLG] = 0xE0;
    m_counter = 0;
    m_echoHistPos = 0;
    m_echoOffset = 0;
    m_everyOther = 1;
    m_noise = 0x4000;
    m_step = 0;
    m_newKon = m_kon = m_koff = 0;
    m_outCount = 0;
}

bool Dsp::checkCounter(int rate) const {
    static const uint16_t rates[32] = {0xFFFF, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
                                       64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1};
    static const uint16_t offsets[32] = {1, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536,
                                         0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 0, 0};
    return ((m_counter + offsets[rate]) % rates[rate]) == 0;
}

void Dsp::write(uint8_t r, uint8_t value) {
    r &= 0x7F;
    m_regs[r] = value;
    m_ext[r] = value;
    switch (r & 0x0F) {
    case ENVX: m_envBuf = value; break;
    case OUTX: m_outBuf = value; break;
    case 0x0C:
        if (r == KON) m_newKon = value;
        else if (r == ENDX) { m_endBuf = 0; writeGlobal(ENDX, 0); }
        break;
    }
}

int32_t Dsp::fir(int index, int ch) const {
    return (m_echoHist[(m_echoHistPos + index + 1) & 7][ch] * (int8_t)m_regs[FIR0 + (index << 4)]) >> 6;
}

// ---------------------------------------------------------------------------
// Voices
// ---------------------------------------------------------------------------

void Dsp::decodeBrr(Voice& v) {
    const uint8_t next = m_ram[(uint16_t)(v.brrAddress + v.brrOffset + 1)];
    const int16_t s4[4] = {
        (int16_t)((int16_t)((m_brrData & 0xF0) << 8) >> 12), (int16_t)((int16_t)((m_brrData & 0x0F) << 12) >> 12),
        (int16_t)((int16_t)((next & 0xF0) << 8) >> 12), (int16_t)((int16_t)((next & 0x0F) << 12) >> 12)};
    const int shift = m_brrHeader >> 4, filter = m_brrHeader & 0x0C;
    int32_t p1 = v.buffer[v.bufferPos > 0 ? v.bufferPos - 1 : 11] >> 1;
    int32_t p2 = v.buffer[v.bufferPos > 1 ? v.bufferPos - 2 : 10] >> 1;
    for (int i = 0; i < 4; ++i) {
        int32_t s = ((int32_t)s4[i] << shift) >> 1;
        if (shift >= 0x0D) s = s < 0 ? -0x800 : 0;
        switch (filter) {
        case 0x04: s += p1 + (-p1 >> 4); break;
        case 0x08: s += (p1 << 1) + ((-((p1 << 1) + p1)) >> 5) - p2 + (p2 >> 4); break;
        case 0x0C: s += (p1 << 1) + ((-(p1 + (p1 << 2) + (p1 << 3))) >> 6) - p2 + (((p2 << 1) + p2) >> 4); break;
        }
        v.buffer[v.bufferPos + i] = (int16_t)(clamp16(s) * 2);
        p2 = p1;
        p1 = v.buffer[v.bufferPos + i] >> 1;
    }
    v.bufferPos = v.bufferPos <= 4 ? (uint8_t)(v.bufferPos + 4) : 0;
}

void Dsp::envelope(Voice& v) {
    int32_t env = v.envVolume;
    if (v.envMode == Env::Release) {
        env -= 8;
        v.envVolume = env < 0 ? 0 : env;
        return;
    }
    int rate = 0;
    uint8_t sustain;
    if (m_adsr1 & 0x80) {
        const uint8_t adsr2 = vreg(v, ADSR2);
        sustain = adsr2;
        switch (v.envMode) {
        case Env::Attack:
            if ((m_adsr1 & 0x0F) == 0x0F) { rate = 31; env += 1024; }
            else { rate = ((m_adsr1 & 0x0F) << 1) | 1; env += 32; }
            break;
        case Env::Decay:
            env -= ((env - 1) >> 8) + 1;
            rate = ((m_adsr1 >> 3) & 0x0E) | 0x10;
            break;
        default:
            env -= ((env - 1) >> 8) + 1;
            rate = adsr2 & 0x1F;
            break;
        }
    } else {
        const uint8_t gain = vreg(v, GAIN);
        sustain = gain;
        if (gain & 0x80) {
            rate = gain & 0x1F;
            switch (gain & 0x60) {
            case 0x00: env -= 32; break;
            case 0x20: env -= ((env - 1) >> 8) + 1; break;
            case 0x40: env += 32; break;
            case 0x60: env += ((uint16_t)v.prevEnv < 0x600 ? 32 : 8); break;
            }
        } else {
            env = (int32_t)gain << 4;
            rate = 31;
        }
    }
    if (v.envMode == Env::Decay && (env >> 8) == (sustain >> 5)) v.envMode = Env::Sustain;
    v.prevEnv = env;
    if (env < 0 || env > 0x7FF) {
        env = env < 0 ? 0 : 0x7FF;
        if (v.envMode == Env::Attack) v.envMode = Env::Decay;
    }
    if (checkCounter(rate)) v.envVolume = env;
}

void Dsp::output(Voice& v, bool right) {
    const int32_t out = (m_voiceOut * (int8_t)vreg(v, right ? VOLR : VOLL)) >> 7;
    m_outSamples[right] = clamp16(m_outSamples[right] + out);
    if (m_eon & v.bit) m_echoOut[right] = clamp16(m_echoOut[right] + out);
}

void Dsp::v1(Voice& v) {
    m_sampleAddr = (uint16_t)(m_dir * 0x100 + m_srcn * 4);
    m_srcn = vreg(v, SRCN);
}

void Dsp::v2(Voice& v) {
    uint16_t a = m_sampleAddr;
    if (v.keyOnDelay == 0) a = (uint16_t)(a + 2);
    m_brrNext = (uint16_t)(m_ram[a] | m_ram[(uint16_t)(a + 1)] << 8);
    m_adsr1 = vreg(v, ADSR1);
    m_pitch = vreg(v, PITCHL);
}

void Dsp::v3a(Voice& v) { m_pitch |= (vreg(v, PITCHH) & 0x3F) << 8; }

void Dsp::v3b(Voice& v) {
    m_brrHeader = m_ram[v.brrAddress];
    m_brrData = m_ram[(uint16_t)(v.brrAddress + v.brrOffset)];
}

void Dsp::v3c(Voice& v) {
    if (m_pmon & v.bit) m_pitch += ((m_voiceOut >> 5) * m_pitch) >> 10;
    if (v.keyOnDelay) {
        if (v.keyOnDelay == 5) {
            v.brrAddress = m_brrNext;
            v.brrOffset = 1;
            v.bufferPos = 0;
            m_brrHeader = 0;
        }
        v.envVolume = 0;
        v.prevEnv = 0;
        --v.keyOnDelay;
        v.interpPos = (v.keyOnDelay & 3) ? 0x4000 : 0;
        m_pitch = 0;
    }
    int32_t out = gauss(v.interpPos, v.buffer, v.bufferPos);
    if (m_non & v.bit) out = (int16_t)(m_noise * 2);
    m_voiceOut = ((out * v.envVolume) >> 11) & ~1;
    v.envOut = (uint8_t)(v.envVolume >> 4);
    if ((reg(FLG) & 0x80) || (m_brrHeader & 0x03) == 0x01) {
        v.envMode = Env::Release;
        v.envVolume = 0;
    }
    if (m_everyOther) {
        if (m_koff & v.bit) v.envMode = Env::Release;
        if (m_kon & v.bit) { v.keyOnDelay = 5; v.envMode = Env::Attack; }
    }
    if (!v.keyOnDelay) envelope(v);
}

void Dsp::v4(Voice& v) {
    m_looped = 0;
    if (v.interpPos >= 0x4000) {
        decodeBrr(v);
        if (v.brrOffset >= 7) {
            if (m_brrHeader & 0x01) { v.brrAddress = m_brrNext; m_looped = v.bit; }
            else v.brrAddress = (uint16_t)(v.brrAddress + 9);
            v.brrOffset = 1;
        } else {
            v.brrOffset = (uint16_t)(v.brrOffset + 2);
        }
    }
    v.interpPos = (v.interpPos & 0x3FFF) + m_pitch;
    if (v.interpPos > 0x7FFF) v.interpPos = 0x7FFF;
    output(v, false);
}

void Dsp::v5(Voice& v) {
    output(v, true);
    uint8_t end = (uint8_t)(reg(ENDX) | m_looped);
    if (v.keyOnDelay == 5) end &= (uint8_t)~v.bit;
    m_endBuf = end;
}

void Dsp::v6(Voice&) { m_outBuf = (uint8_t)(m_voiceOut >> 8); }
void Dsp::v7(Voice& v) { writeGlobal(ENDX, m_endBuf); m_envBuf = v.envOut; }
void Dsp::v8(Voice& v) { writeVoice(v, OUTX, m_outBuf); }
void Dsp::v9(Voice& v) { writeVoice(v, ENVX, m_envBuf); }

// ---------------------------------------------------------------------------
// Echo
// ---------------------------------------------------------------------------

void Dsp::echo22() {
    m_echoHistPos = (m_echoHistPos + 1) & 7;
    m_echoPtr = (uint16_t)((m_esa << 8) + m_echoOffset);
    const int16_t s = (int16_t)(m_ram[m_echoPtr] | m_ram[(uint16_t)(m_echoPtr + 1)] << 8);
    m_echoHist[m_echoHistPos][0] = (int16_t)(s >> 1);
    m_echoIn[0] = fir(0, 0);
    m_echoIn[1] = fir(0, 1);
}
void Dsp::echo23() {
    const int16_t s = (int16_t)(m_ram[(uint16_t)(m_echoPtr + 2)] | m_ram[(uint16_t)(m_echoPtr + 3)] << 8);
    m_echoHist[m_echoHistPos][1] = (int16_t)(s >> 1);
    m_echoIn[0] += fir(1, 0) + fir(2, 0);
    m_echoIn[1] += fir(1, 1) + fir(2, 1);
}
void Dsp::echo24() {
    m_echoIn[0] += fir(3, 0) + fir(4, 0) + fir(5, 0);
    m_echoIn[1] += fir(3, 1) + fir(4, 1) + fir(5, 1);
}
void Dsp::echo25() {
    const int32_t l = (int16_t)(m_echoIn[0] + fir(6, 0)), r = (int16_t)(m_echoIn[1] + fir(6, 1));
    m_echoIn[0] = clamp16(l + (int16_t)fir(7, 0)) & ~1;
    m_echoIn[1] = clamp16(r + (int16_t)fir(7, 1)) & ~1;
}
void Dsp::echo26() {
    m_outSamples[0] = clamp16(((m_outSamples[0] * (int8_t)reg(MVOLL)) >> 7) + ((m_echoIn[0] * (int8_t)reg(EVOLL)) >> 7));
    const int32_t l = m_echoOut[0] + (int16_t)((m_echoIn[0] * (int8_t)reg(EFB)) >> 7);
    const int32_t r = m_echoOut[1] + (int16_t)((m_echoIn[1] * (int8_t)reg(EFB)) >> 7);
    m_echoOut[0] = clamp16(l) & ~1;
    m_echoOut[1] = clamp16(r) & ~1;
}
void Dsp::echo27() {
    m_outSamples[1] = clamp16(((m_outSamples[1] * (int8_t)reg(MVOLR)) >> 7) + ((m_echoIn[1] * (int8_t)reg(EVOLR)) >> 7));
}
void Dsp::echo28() { m_echoEnabled = !(reg(FLG) & 0x20); }
void Dsp::echo29() {
    if (m_echoOffset == 0) m_echoLength = (uint16_t)((reg(EDL) & 0x0F) << 11);
    m_echoOffset = (uint16_t)(m_echoOffset + 4);
    if (m_echoOffset >= m_echoLength) m_echoOffset = 0;
    if (m_echoEnabled) {
        m_ram[m_echoPtr] = (uint8_t)m_echoOut[0];
        m_ram[(uint16_t)(m_echoPtr + 1)] = (uint8_t)(m_echoOut[0] >> 8);
    }
    m_echoOut[0] = 0;
    m_esa = reg(ESA);
    m_echoEnabled = !(reg(FLG) & 0x20);
}
void Dsp::echo30() {
    if (m_echoEnabled) {
        m_ram[(uint16_t)(m_echoPtr + 2)] = (uint8_t)m_echoOut[1];
        m_ram[(uint16_t)(m_echoPtr + 3)] = (uint8_t)(m_echoOut[1] >> 8);
    }
    m_echoOut[1] = 0;
}

// ---------------------------------------------------------------------------
// The pipeline
// ---------------------------------------------------------------------------

void Dsp::step() {
    Voice* V = m_voices;
    const uint8_t s = m_step;
    m_step = (m_step + 1) & 0x1F;
    switch (s) {
    case 0: v5(V[0]); v2(V[1]); break;
    case 1: v6(V[0]); v3(V[1]); break;
    case 2: v7(V[0]); v4(V[1]); v1(V[3]); break;
    case 3: v8(V[0]); v5(V[1]); v2(V[2]); break;
    case 4: v9(V[0]); v6(V[1]); v3(V[2]); break;
    case 5: v7(V[1]); v4(V[2]); v1(V[4]); break;
    case 6: v8(V[1]); v5(V[2]); v2(V[3]); break;
    case 7: v9(V[1]); v6(V[2]); v3(V[3]); break;
    case 8: v7(V[2]); v4(V[3]); v1(V[5]); break;
    case 9: v8(V[2]); v5(V[3]); v2(V[4]); break;
    case 10: v9(V[2]); v6(V[3]); v3(V[4]); break;
    case 11: v7(V[3]); v4(V[4]); v1(V[6]); break;
    case 12: v8(V[3]); v5(V[4]); v2(V[5]); break;
    case 13: v9(V[3]); v6(V[4]); v3(V[5]); break;
    case 14: v7(V[4]); v4(V[5]); v1(V[7]); break;
    case 15: v8(V[4]); v5(V[5]); v2(V[6]); break;
    case 16: v9(V[4]); v6(V[5]); v3(V[6]); break;
    case 17: v1(V[0]); v7(V[5]); v4(V[6]); break;
    case 18: v8(V[5]); v5(V[6]); v2(V[7]); break;
    case 19: v9(V[5]); v6(V[6]); v3(V[7]); break;
    case 20: v1(V[1]); v7(V[6]); v4(V[7]); break;
    case 21: v8(V[6]); v5(V[7]); v2(V[0]); break;
    case 22: v3a(V[0]); v9(V[6]); v6(V[7]); echo22(); break;
    case 23: v7(V[7]); echo23(); break;
    case 24: v8(V[7]); echo24(); break;
    case 25: v3b(V[0]); v9(V[7]); echo25(); break;
    case 26: echo26(); break;
    case 27:
        m_pmon = reg(PMON) & 0xFE;
        echo27();
        if (m_outCount + 2 <= (int)(sizeof m_out / sizeof m_out[0])) {
            const bool mute = reg(FLG) & 0x40;
            m_out[m_outCount] = mute ? 0 : (int16_t)m_outSamples[0];
            m_out[m_outCount + 1] = mute ? 0 : (int16_t)m_outSamples[1];
            m_outCount += 2;
        }
        m_outSamples[0] = m_outSamples[1] = 0;
        break;
    case 28:
        m_dir = reg(DIR);
        m_non = reg(NON);
        m_eon = reg(EON);
        echo28();
        break;
    case 29:
        m_everyOther ^= 1;
        if (m_everyOther) m_newKon &= (uint8_t)~m_kon;
        echo29();
        break;
    case 30:
        if (m_everyOther) { m_kon = m_newKon; m_koff = reg(KOFF); }
        m_counter = m_counter == 0 ? 0x77FF : (uint16_t)(m_counter - 1);
        if (checkCounter(reg(FLG) & 0x1F)) {
            const int bit = ((m_noise << 14) ^ (m_noise << 13)) & 0x4000;
            m_noise = bit ^ (m_noise >> 1);
        }
        v3c(V[0]);
        echo30();
        break;
    case 31: v4(V[0]); v1(V[2]); break;
    }
}

}  // namespace snes
