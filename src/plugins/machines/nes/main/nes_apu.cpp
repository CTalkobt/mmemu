// NES APU (2A03) after Mesen2 (Core/NES/APU/*, NesSoundMixer.cpp), GPL-3.0,
// Sour and the Mesen contributors, and the nesdev wiki ("APU").

#include "nes_apu.h"

#include <algorithm>
#include <cmath>

namespace nes {

namespace {

const uint8_t LENGTHS[32] = {10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
                             12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};
const uint8_t DUTY[4][8] = {{0, 0, 0, 0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 0, 1, 1}, {0, 0, 0, 0, 1, 1, 1, 1}, {1, 1, 1, 1, 1, 1, 0, 0}};
const uint8_t TRIANGLE[32] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
                              0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
// NTSC periods in CPU cycles
const uint16_t NOISE_PERIODS[16] = {4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068};
const uint16_t DMC_RATES[16] = {428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54};
// Frame counter steps (Mesen ApuFrameCounter, NTSC): Q = quarter frame, H = half
const int32_t STEPS[2][6] = {{7457, 14913, 22371, 29828, 29829, 29830}, {7457, 14913, 22371, 29829, 37281, 37282}};
enum { NONE, QUARTER, HALF };
const int STEP_TYPE[6] = {QUARTER, HALF, QUARTER, NONE, HALF, NONE};

// The nesdev mixer formulas as tables (Mesen NesSoundMixer)
struct MixTables {
    float pulse[31];
    float tnd[203];
    MixTables() {
        pulse[0] = 0;
        for (int i = 1; i < 31; ++i) pulse[i] = (float)(95.52 / (8128.0 / i + 100.0));
        tnd[0] = 0;
        for (int i = 1; i < 203; ++i) tnd[i] = (float)(163.67 / (24329.0 / i + 100.0));
    }
};
const MixTables MIX;

}  // namespace

void Apu::Envelope::tick() {
    if (start) {
        start = false;
        decay = 15;
        divider = volume;
        return;
    }
    if (divider == 0) {
        divider = volume;
        if (decay) --decay;
        else if (loop) decay = 15;
    } else {
        --divider;
    }
}

void Apu::Length::load(uint8_t index) {
    if (!enabled) return;
    reloadValue = LENGTHS[index & 0x1F];
    previous = counter;
}

uint16_t Apu::Pulse::target() const {
    const uint16_t delta = period >> sweepShift;
    if (sweepNegate) return (uint16_t)(period - delta - (second ? 0 : 1));   // pulse 1 negates in ones' complement
    return (uint16_t)(period + delta);
}

void Apu::Pulse::tickSweep() {
    --sweepDivider;
    if (sweepDivider == 0) {
        if (sweepShift > 0 && sweepOn && period >= 8 && target() <= 0x7FF) period = target();
        sweepDivider = sweepPeriod;
    }
    if (sweepReload) {
        sweepDivider = sweepPeriod;
        sweepReload = false;
    }
}

void Apu::Pulse::clock() {
    // timer period 2p+1 in CPU cycles (Mesen SquareChannel::SetPeriod)
    if (timer == 0) {
        timer = (uint16_t)(period * 2 + 1);
        step = (step - 1) & 7;
    } else {
        --timer;
    }
}

uint8_t Apu::Pulse::out() const {
    if (muted() || !len.counter) return 0;
    return (uint8_t)(DUTY[duty][step] * env.level());
}

void Apu::Triangle::tickLinear() {
    if (linearReload) linear = linearPeriod;
    else if (linear) --linear;
    if (!control) linearReload = false;
}

void Apu::Triangle::clock() {
    if (timer == 0) {
        timer = period;
        // The sequencer stops with either counter at zero; ultrasonic periods (< 2) hold too
        if (len.counter && linear && period >= 2) step = (step + 1) & 31;
    } else {
        --timer;
    }
}

uint8_t Apu::Triangle::out() const { return TRIANGLE[step]; }

void Apu::Noise::clock() {
    if (timer == 0) {
        timer = (uint16_t)(period - 1);
        const uint16_t fb = (uint16_t)((shift & 1) ^ ((shift >> (mode ? 6 : 1)) & 1));
        shift = (uint16_t)((shift >> 1) | (fb << 14));
    } else {
        --timer;
    }
}

Apu::Apu() : m_audioBuf(AUDIO_BUF, 0.0f) { reset(); }

void Apu::reset() {
    m_pulse[0] = Pulse{};
    m_pulse[1] = Pulse{};
    m_pulse[1].second = true;
    m_tri = Triangle{};
    m_noise = Noise{};
    m_dmc = Dmc{};
    m_dmc.timer = (uint16_t)(m_dmc.rate - 1);
    m_frameCycle = 0;
    m_frameStep = 0;
    m_fiveStep = false;
    m_irqInhibit = false;
    m_frameBlock = 0;
    m_frameIrq = m_dmcIrq = false;
    // "After reset or power-up, APU acts as if $4017 were written with $00
    // from 9 to 12 clocks before first instruction begins"
    m_frameWriteValue = 0;
    m_frameWriteDelay = 3;
    m_cycle = 0;
    m_acc = 0;
    m_accCount = 0;
    m_phase = 0;
    m_hpIn = m_hpOut = 0;
    m_audioRead = m_audioWrite = m_audioCount = 0;
}

void Apu::softReset() {
    const Length tri = m_tri.len;
    const uint16_t sampleAddr = m_dmc.sampleAddr, sampleLength = m_dmc.sampleLength;
    const bool five = m_fiveStep;
    reset();
    m_tri.len.counter = tri.counter;
    m_tri.len.halt = tri.halt;
    m_tri.len.newHalt = tri.newHalt;
    m_dmc.sampleAddr = sampleAddr;
    m_dmc.sampleLength = sampleLength;
    m_fiveStep = five;
    m_frameWriteValue = five ? 0x80 : 0x00;
}

void Apu::write(uint16_t addr, uint8_t v) {
    switch (addr) {
    case 0x4000: case 0x4004: {
        Pulse& p = m_pulse[(addr >> 2) & 1];
        p.env.write(v);
        p.len.setHalt(v & 0x20);
        p.duty = v >> 6;
        break;
    }
    case 0x4001: case 0x4005: {
        Pulse& p = m_pulse[(addr >> 2) & 1];
        p.sweepOn = v & 0x80;
        p.sweepNegate = v & 0x08;
        p.sweepPeriod = (uint8_t)(((v >> 4) & 7) + 1);
        p.sweepShift = v & 7;
        p.sweepReload = true;
        break;
    }
    case 0x4002: case 0x4006: {
        Pulse& p = m_pulse[(addr >> 2) & 1];
        p.period = (uint16_t)((p.period & 0x700) | v);
        break;
    }
    case 0x4003: case 0x4007: {
        Pulse& p = m_pulse[(addr >> 2) & 1];
        p.len.load(v >> 3);
        p.period = (uint16_t)((p.period & 0xFF) | ((v & 7) << 8));
        p.step = 0;
        p.env.start = true;
        break;
    }
    case 0x4008:
        m_tri.control = v & 0x80;
        m_tri.len.setHalt(v & 0x80);
        m_tri.linearPeriod = v & 0x7F;
        break;
    case 0x400A:
        m_tri.period = (uint16_t)((m_tri.period & 0x700) | v);
        break;
    case 0x400B:
        m_tri.len.load(v >> 3);
        m_tri.period = (uint16_t)((m_tri.period & 0xFF) | ((v & 7) << 8));
        m_tri.linearReload = true;
        break;
    case 0x400C:
        m_noise.env.write(v);
        m_noise.len.setHalt(v & 0x20);
        break;
    case 0x400E:
        m_noise.mode = v & 0x80;
        m_noise.period = NOISE_PERIODS[v & 0x0F];
        break;
    case 0x400F:
        m_noise.len.load(v >> 3);
        m_noise.env.start = true;
        break;
    case 0x4010:
        m_dmc.irqOn = v & 0x80;
        m_dmc.loop = v & 0x40;
        m_dmc.rate = DMC_RATES[v & 0x0F];
        if (!m_dmc.irqOn) m_dmcIrq = false;
        break;
    case 0x4011:
        m_dmc.level = v & 0x7F;
        break;
    case 0x4012:
        m_dmc.sampleAddr = (uint16_t)(0xC000 | (v << 6));
        break;
    case 0x4013:
        m_dmc.sampleLength = (uint16_t)((v << 4) | 1);
        break;
    case 0x4015: {
        Length* lens[4] = {&m_pulse[0].len, &m_pulse[1].len, &m_tri.len, &m_noise.len};
        for (int i = 0; i < 4; ++i) {
            lens[i]->enabled = (v >> i) & 1;
            if (!lens[i]->enabled) lens[i]->counter = 0;
        }
        m_dmcIrq = false;
        if (!(v & 0x10)) {
            // Disabling takes effect a cycle or two later; a DMA starting
            // meanwhile is cancelled (but still halts the CPU a cycle)
            if (m_dmc.disableDelay == 0) m_dmc.disableDelay = cpuCycleEven() ? 2 : 3;
        } else if (m_dmc.remaining == 0) {
            dmcStart();
            // The first fetch waits 2 or 3 cycles (dmc_dma_start_test)
            m_dmc.startDelay = cpuCycleEven() ? 2 : 3;
        }
        break;
    }
    case 0x4017:
        m_frameWriteValue = v;
        // "If the write occurs during an APU cycle, the effects occur 3 CPU
        // cycles after the write cycle; between APU cycles, 4"
        // (the CPU cycle's parity, as Mesen)
        m_frameWriteDelay = cpuCycleEven() ? 3 : 4;
        m_irqInhibit = v & 0x40;
        if (m_irqInhibit) m_frameIrq = false;
        break;
    default:
        break;
    }
}

uint8_t Apu::peekStatus(uint8_t openBus) const {
    uint8_t s = (uint8_t)(openBus & 0x20);
    if (m_pulse[0].len.counter) s |= 0x01;
    if (m_pulse[1].len.counter) s |= 0x02;
    if (m_tri.len.counter) s |= 0x04;
    if (m_noise.len.counter) s |= 0x08;
    if (m_dmc.remaining) s |= 0x10;
    if (m_frameIrq) s |= 0x40;
    if (m_dmcIrq) s |= 0x80;
    return s;
}

uint8_t Apu::readStatus(uint8_t openBus) {
    const uint8_t s = peekStatus(openBus);
    m_frameIrq = false;
    return s;
}

int Apu::output(int ch) const {
    switch (ch) {
    case 0: return m_pulse[0].out();
    case 1: return m_pulse[1].out();
    case 2: return m_tri.out();
    case 3: return m_noise.out();
    case 4: return m_dmc.level;
    default: return 0;
    }
}

int Apu::lengthCounter(int ch) const {
    switch (ch) {
    case 0: return m_pulse[0].len.counter;
    case 1: return m_pulse[1].len.counter;
    case 2: return m_tri.len.counter;
    case 3: return m_noise.len.counter;
    default: return 0;
    }
}

void Apu::quarterFrame() {
    m_pulse[0].env.tick();
    m_pulse[1].env.tick();
    m_noise.env.tick();
    m_tri.tickLinear();
}

void Apu::halfFrame() {
    m_pulse[0].len.tick();
    m_pulse[1].len.tick();
    m_tri.len.tick();
    m_noise.len.tick();
    m_pulse[0].tickSweep();
    m_pulse[1].tickSweep();
}

void Apu::clockFrameCounter() {
    ++m_frameCycle;
    const int mode = m_fiveStep ? 1 : 0;
    if (m_frameCycle >= STEPS[mode][m_frameStep]) {
        if (!m_fiveStep && m_frameStep >= 3 && !m_irqInhibit) m_frameIrq = true;
        const int t = STEP_TYPE[m_frameStep];
        if (t != NONE && !m_frameBlock) {
            quarterFrame();
            if (t == HALF) halfFrame();
            m_frameBlock = 2;
        }
        if (++m_frameStep == 6) {
            m_frameStep = 0;
            m_frameCycle = 0;
        }
    }
    if (m_frameWriteDelay && --m_frameWriteDelay == 0) {
        m_fiveStep = m_frameWriteValue & 0x80;
        m_frameStep = 0;
        m_frameCycle = 0;
        // "Writing to $4017 with bit 7 set will immediately generate a clock
        // for both the quarter frame and the half frame units"
        if (m_fiveStep && !m_frameBlock) {
            quarterFrame();
            halfFrame();
            m_frameBlock = 2;
        }
    }
    if (m_frameBlock) --m_frameBlock;
}

void Apu::dmcRequest() {
    if (m_dmc.empty && m_dmc.remaining && m_dmaStart) m_dmaStart();
}

void Apu::dmcSetBuffer(uint8_t v) {
    if (m_dmc.remaining) {
        m_dmc.buffer = v;
        m_dmc.empty = false;
        m_dmc.addr = m_dmc.addr == 0xFFFF ? 0x8000 : (uint16_t)(m_dmc.addr + 1);
        if (--m_dmc.remaining == 0) {
            if (m_dmc.loop) dmcStart();         // a looped sample never raises the IRQ
            else if (m_dmc.irqOn) m_dmcIrq = true;
        }
    }
    if (m_dmc.sampleLength == 1 && !m_dmc.loop && m_dmc.bits == 1 && m_dmc.timer < 2) {
        // The DMA ended on the APU cycle before the bit counter reloads: a
        // DMA is requested and aborted a cycle later (one halted cycle)
        m_dmc.shift = m_dmc.buffer;
        m_dmc.empty = false;
        dmcStart();
        m_dmc.disableDelay = 3;
    }
}

void Apu::clockDmc() {
    if (m_dmc.disableDelay && --m_dmc.disableDelay == 0) {
        m_dmc.remaining = 0;
        if (m_dmaStop) m_dmaStop();             // abort a transfer not fully started
    }
    if (m_dmc.startDelay && --m_dmc.startDelay == 0) dmcRequest();

    if (m_dmc.timer == 0) {
        m_dmc.timer = (uint16_t)(m_dmc.rate - 1);
        if (!m_dmc.silence) {
            if (m_dmc.shift & 1) { if (m_dmc.level <= 125) m_dmc.level += 2; }
            else if (m_dmc.level >= 2) m_dmc.level -= 2;
        }
        m_dmc.shift >>= 1;
        if (--m_dmc.bits == 0) {
            m_dmc.bits = 8;
            if (m_dmc.empty) m_dmc.silence = true;
            else {
                m_dmc.silence = false;
                m_dmc.shift = m_dmc.buffer;
                m_dmc.empty = true;
                // Not while a $4015 enable is still pending (it requests then)
                if (m_dmc.startDelay == 0) dmcRequest();
            }
        }
    } else {
        --m_dmc.timer;
    }
}

float Apu::mix() const {
    const int p = m_pulse[0].out() + m_pulse[1].out();
    const int tnd = 3 * m_tri.out() + 2 * m_noise.out() + m_dmc.level;
    return MIX.pulse[p] + MIX.tnd[tnd];
}

void Apu::clock(uint32_t cycles) {
    while (cycles--) {
        clockFrameCounter();
        m_pulse[0].len.reload();
        m_pulse[1].len.reload();
        m_tri.len.reload();
        m_noise.len.reload();
        m_pulse[0].clock();
        m_pulse[1].clock();
        m_tri.clock();
        m_noise.clock();
        clockDmc();
        ++m_cycle;

        m_acc += mix();
        ++m_accCount;
        m_phase += (uint64_t)m_sampleRate;
        if (m_phase >= CPU_HZ) {
            m_phase -= CPU_HZ;
            const float x = (float)(m_acc / m_accCount);
            m_acc = 0;
            m_accCount = 0;
            // A first-order high-pass (the console's output coupling) removes the DC offset
            m_hpOut = x - m_hpIn + 0.9965f * m_hpOut;
            m_hpIn = x;
            const float s = std::max(-1.0f, std::min(1.0f, m_hpOut * 1.5f));
            if (m_audioCount == AUDIO_BUF) {          // full: drop the oldest
                m_audioRead = (m_audioRead + 1) % AUDIO_BUF;
                --m_audioCount;
            }
            m_audioBuf[m_audioWrite] = s;
            m_audioWrite = (m_audioWrite + 1) % AUDIO_BUF;
            ++m_audioCount;
        }
    }
}

int Apu::pullSamples(float* buffer, int maxSamples) {
    const int n = std::min(maxSamples, m_audioCount);
    for (int i = 0; i < n; ++i) {
        buffer[i] = m_audioBuf[m_audioRead];
        m_audioRead = (m_audioRead + 1) % AUDIO_BUF;
    }
    m_audioCount -= n;
    return n;
}

}  // namespace nes
