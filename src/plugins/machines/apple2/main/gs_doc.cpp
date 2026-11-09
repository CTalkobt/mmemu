// The IIgs's Ensoniq ES5503 DOC, after MAME es5503.cpp v2.4 (R. Belmont,
// BSD-3-Clause), and the sound GLU after KEGS doc.c / MAME apple2gs.cpp.

#include "gs_doc.h"
#include "apple2_parts.h"
#include <algorithm>

namespace apple2 {

namespace {
constexpr uint16_t WAVESIZES[8] = {256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
constexpr uint32_t WAVEMASKS[8] = {0x1FF00, 0x1FE00, 0x1FC00, 0x1F800, 0x1F000, 0x1E000, 0x1C000, 0x18000};
constexpr uint32_t ACCMASKS[8] = {0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF, 0x3FFF, 0x7FFF};
constexpr int RESSHIFTS[8] = {9, 10, 11, 12, 13, 14, 15, 16};
constexpr double DOC_PER_SLOW = 7.0;                // 7.16 MHz / 1.023 MHz
}  // namespace

GsSound::GsSound(Apple2Speaker* speaker, double slowHz)
    : m_speaker(speaker), m_docPerSlow(DOC_PER_SLOW), m_outPeriod(slowHz * DOC_PER_SLOW / SAMPLE_RATE) {
    m_ram.assign(0x10000, 0);
    reset();
}

void GsSound::reset() {
    m_rege0 = 0xFF;
    for (auto& o : m_osc) o = Osc();
    for (auto& o : m_osc) { o.control = 0; o.wtsize = 0; }
    m_numOsc = 1;
    m_irq = false;
    m_ctl = 0;
    m_ptr = 0;
}

// halt_osc: type 1 for a zero in the sample data, 0 for the end of the table
void GsSound::haltOsc(int onum, int type, uint32_t* acc, int resshift, uint8_t newCtrl) {
    Osc& o = m_osc[onum];
    Osc& partner = m_osc[onum ^ 1];
    int mode = (o.control >> 1) & 3;
    const int partnerMode = (partner.control >> 1) & 3;
    if (mode == MODE_SYNCAM) {
        if (!(onum & 1) && onum > 0 && !(m_osc[onum - 1].control & 1)) m_osc[onum - 1].accumulator = 0;
        mode = MODE_FREE;                                   // sync and AM loop
    }
    if (mode != MODE_FREE || type != 0) o.control |= 1;
    else *acc -= (uint32_t)(uint16_t)(o.wtsize - 1) << resshift;      // keep the phase when looping
    if (mode == MODE_SWAP) {
        partner.control &= ~1;
        partner.accumulator = 0;
    } else if (partnerMode == MODE_SWAP && !(onum & 1)) {
        o.control &= ~1;                                    // even, the partner swaps: retrigger
        *acc -= (uint32_t)(uint16_t)(o.wtsize - 1) << resshift;
    }
    if (o.control & 0x08) {
        if (type == 0 && !(newCtrl & 0x08)) return;
        o.irqpend = true;
        updateIrq();
    }
}

void GsSound::updateIrq() {
    m_irq = false;
    for (int i = 0; i < m_numOsc; ++i) if (m_osc[i].irqpend) m_irq = true;
}

// One DOC sample: every enabled, running oscillator steps once
int32_t GsSound::docSample() {
    int32_t mix = 0;
    for (int n = 0; n < m_numOsc; ++n) {
        Osc& o = m_osc[n];
        if (o.control & 1) continue;
        const uint32_t wtptr = o.wavetblpointer & WAVEMASKS[o.wavetblsize];
        const int resshift = RESSHIFTS[o.resolution] - o.wavetblsize;
        const uint32_t sizemask = ACCMASKS[o.wavetblsize];
        const int mode = (o.control >> 1) & 3;
        uint32_t acc = o.accumulator;
        const uint32_t altram = acc >> resshift;
        const uint32_t ramptr = altram & sizemask;
        acc += o.freq;
        const uint8_t byte = m_ram[(ramptr + wtptr) & 0xFFFF];      // 64 KB, mirrored
        const int32_t data = (int32_t)(int8_t)(byte ^ 0x80);
        if (byte == 0) {
            haltOsc(n, 1, &acc, resshift, o.control);
        } else {
            const int times = n == m_numOsc - 1 ? 3 : 1;           // the highest enabled oscillator
            if (mode != MODE_SYNCAM) {
                mix += data * o.vol * times;
            } else if (n & 1) {                                     // AM: the odd one sets the next's volume
                if (n < 31 && !(m_osc[n + 1].control & 1)) m_osc[n + 1].vol = (uint8_t)(data ^ 0x80);
            } else {
                mix += data * o.vol * times;
            }
            if (altram >= (uint32_t)(uint16_t)(o.wtsize - 1)) haltOsc(n, 0, &acc, resshift, o.control);
        }
        o.accumulator = acc;
        o.data = byte;
    }
    return mix;
}

void GsSound::advance(uint64_t slowCycle) {
    if (slowCycle <= m_lastSlow) return;
    const double target = m_docClock + (double)(slowCycle - m_lastSlow) * m_docPerSlow;
    m_lastSlow = slowCycle;
    const double period = 8.0 * (m_numOsc + 2);
    while (true) {
        const double next = std::min(m_nextSample, m_nextOut);
        if (next > target) break;
        if (m_nextSample <= m_nextOut) {
            m_acc += docSample();
            ++m_accCount;
            m_nextSample += period;
        } else {
            const float vol = (float)(m_ctl & 0x0F) / 15.0f;
            if (m_accCount) m_last = (float)(m_acc / m_accCount / (32768.0 * 8.0)) * vol;
            m_acc = 0;
            m_accCount = 0;
            m_out.push_back(m_last);
            if (m_out.size() > (size_t)SAMPLE_RATE) m_out.pop_front();
            m_nextOut += m_outPeriod;
        }
    }
    m_docClock = target;
}

int GsSound::pullSamples(float* buffer, int maxSamples) {
    const int n = std::min(maxSamples, (int)m_out.size());
    std::vector<float> spk((size_t)std::max(n, 1), 0.0f);
    const int got = m_speaker ? m_speaker->pullSamples(spk.data(), n) : 0;
    for (int i = 0; i < n; ++i) {
        buffer[i] = std::clamp(m_out.front() + (i < got ? spk[(size_t)i] : 0.0f), -1.0f, 1.0f);
        m_out.pop_front();
    }
    return n;
}

uint8_t GsSound::docRead(uint8_t reg) {
    if (reg < 0xE0) {
        const Osc& o = m_osc[reg & 0x1F];
        switch (reg & 0xE0) {
            case 0x00: return (uint8_t)o.freq;
            case 0x20: return (uint8_t)(o.freq >> 8);
            case 0x40: return o.vol;
            case 0x60: return o.data;
            case 0x80: return (uint8_t)(o.wavetblpointer >> 8);
            case 0xA0: return o.control;
            default:
                return (uint8_t)(((o.wavetblpointer & 0x10000) ? 0x40 : 0) | o.wavetblsize << 3 | o.resolution);
        }
    }
    switch (reg) {
        case 0xE0: {                                         // the interrupting oscillator, cleared
            uint8_t ret = m_rege0;
            for (int i = 0; i < m_numOsc; ++i)
                if (m_osc[i].irqpend) {
                    ret = (uint8_t)(i << 1);
                    m_rege0 = ret | 0x80;
                    m_osc[i].irqpend = false;
                    break;
                }
            updateIrq();
            return ret | 0x41;
        }
        case 0xE1: return (uint8_t)((m_numOsc - 1) << 1);
        case 0xE2: return 0x80;                              // the A/D converter: centred
        default: return 0;
    }
}

void GsSound::docWrite(uint8_t reg, uint8_t v) {
    if (reg < 0xE0) {
        const int n = reg & 0x1F;
        Osc& o = m_osc[n];
        switch (reg & 0xE0) {
            case 0x00: o.freq = (uint16_t)((o.freq & 0xFF00) | v); break;
            case 0x20: o.freq = (uint16_t)((o.freq & 0x00FF) | v << 8); break;
            case 0x40: o.vol = v; break;
            case 0x60: break;
            case 0x80: o.wavetblpointer = (uint32_t)v << 8; break;
            case 0xA0:
                if ((o.control & 1) && !(v & 1)) o.accumulator = 0;              // key on
                if (!(o.control & 1) && (v & 1) && ((v >> 1) & 1))               // halted from the CPU
                    haltOsc(n, 0, &o.accumulator, RESSHIFTS[o.resolution], v);
                o.control = v;
                break;
            default:
                if (v & 0x40) o.wavetblpointer |= 0x10000; else o.wavetblpointer &= 0xFFFF;
                o.wavetblsize = (v >> 3) & 7;
                o.wtsize = WAVESIZES[o.wavetblsize];
                o.resolution = v & 7;
                break;
        }
        return;
    }
    if (reg == 0xE1) {
        m_numOsc = ((v >> 1) & 0x1F) + 1;
        updateIrq();
    }
}

// $C03D: reads return the value fetched by the previous access
uint8_t GsSound::readData(uint64_t now) {
    advance(now);
    const uint8_t ret = m_saved;
    m_saved = (m_ctl & 0x40) ? m_ram[m_ptr] : docRead((uint8_t)m_ptr);
    if (m_ctl & 0x20) ++m_ptr;
    return ret;
}

void GsSound::writeData(uint8_t v, uint64_t now) {
    advance(now);
    if (m_ctl & 0x40) m_ram[m_ptr] = v;
    else docWrite((uint8_t)m_ptr, v);
    if (m_ctl & 0x20) ++m_ptr;
    m_saved = v;
}

}  // namespace apple2
