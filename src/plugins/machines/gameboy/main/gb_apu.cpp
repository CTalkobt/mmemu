// The Game Boy's APU, after Mesen2 (Core/Gameboy/APU/), GPL-3.0, Sour and the Mesen contributors.

#include "gb_apu.h"
#include "gameboy.h"

#include <cstring>

namespace gb {

namespace {
// The duty waveforms in the phase Mesen's GbSquareChannel has them (SameSuite's channel_1/2 tests)
const uint8_t DUTY[4][8] = {
    {0, 0, 0, 0, 0, 0, 1, 0}, {0, 0, 0, 0, 0, 0, 1, 1}, {0, 0, 0, 0, 1, 1, 1, 1}, {1, 1, 1, 1, 1, 1, 0, 0}};
// The DMG's wave RAM at power on (Mesen's preset, so R-Type is audible)
const uint8_t WAVE_DEFAULT[16] = {0x84, 0x40, 0x43, 0xAA, 0x2D, 0x78, 0x92, 0x3C, 0x60, 0x59, 0x59, 0xB0, 0x34, 0xB8, 0x2E, 0xDA};
}  // namespace

void Apu::init() {
    m_sq1 = Square{};
    m_sq2 = Square{};
    m_wave = Wave{};
    std::memcpy(m_wave.ram, WAVE_DEFAULT, 16);
    m_noise = Noise{};
    m_enabled = false;
    m_frameStep = 0;
    m_speedSwitchDelay = false;
    std::memset(m_enLeft, 0, sizeof(m_enLeft));
    std::memset(m_enRight, 0, sizeof(m_enRight));
    m_leftVolume = m_rightVolume = 0;
    m_extLeft = m_extRight = false;
    m_prevLeft = m_prevRight = 0;
    m_prevClock = 0;
    m_skipFirstEvent = 0;
    m_powerOnCycle = 0;
    m_phase = 0;
    m_accL = m_accR = 0;
    m_samples.clear();
}

void Apu::setSampleRate(double rate, double clockHz) { m_period = clockHz / rate; }

bool Apu::oddApuCycle() const { return ((m_gb->apuCycles() - m_powerOnCycle) & 2) != 0; }
uint64_t Apu::elapsedApuCycles() const { return m_gb->apuCycles() - m_powerOnCycle; }

void Apu::span(uint32_t clocks) {
    // The level held over `clocks`, box-filtered into samples
    double c = clocks;
    while (c > 0) {
        const double room = m_period - m_phase;
        if (c < room) {
            m_accL += m_prevLeft * c;
            m_accR += m_prevRight * c;
            m_phase += c;
            return;
        }
        m_accL += m_prevLeft * room;
        m_accR += m_prevRight * room;
        c -= room;
        m_samples.push_back((int16_t)(m_accL / m_period));
        m_samples.push_back((int16_t)(m_accR / m_period));
        if (m_samples.size() > 2 * 16384) { m_samples.pop_front(); m_samples.pop_front(); }   // nobody taking them
        m_accL = m_accR = 0;
        m_phase = 0;
    }
}

void Apu::run() {
    const uint64_t now = m_gb->apuCycles();
    uint32_t clocks = (uint32_t)(now - m_prevClock);
    m_prevClock = now;
    if (!m_enabled) {
        span(clocks);
        return;
    }
    while (clocks > 0) {
        uint32_t step = 250;
        if (clocks < step) step = clocks;
        if (m_sq1.timer < step) step = m_sq1.timer;
        if (m_sq2.timer < step) step = m_sq2.timer;
        if (m_wave.timer < step) step = m_wave.timer;
        if (m_noise.timer < step) step = m_noise.timer;
        clocks -= step;
        sqExec(m_sq1, step);
        sqExec(m_sq2, step);
        waveExec(step);
        noiseExec(step);
        span(step);
        updateOutput();
    }
}

void Apu::updateOutput() {
    const double s1 = analog(m_sq1.output, m_sq1.dac), s2 = analog(m_sq2.output, m_sq2.dac);
    const double w = analog(m_wave.output, m_wave.dac), n = analog(m_noise.output, m_noise.dac);
    // Mesen's mix: the channel's analog value times (its volume setting (100) & the panning byte) / 100
    auto ch = [](double v, uint8_t en) { return v * (int32_t)(100 & en) / 100; };
    m_prevLeft = (int16_t)((ch(s1, m_enLeft[0]) + ch(s2, m_enLeft[1]) + ch(w, m_enLeft[2]) + ch(n, m_enLeft[3])) * (m_leftVolume + 1) * 40);
    m_prevRight = (int16_t)((ch(s1, m_enRight[0]) + ch(s2, m_enRight[1]) + ch(w, m_enRight[2]) + ch(n, m_enRight[3])) * (m_rightVolume + 1) * 40);
}

void Apu::clockFrameSequencer() {
    run();
    if (!m_enabled) return;
    if (m_skipFirstEvent && --m_skipFirstEvent) return;
    if ((m_frameStep & 1) == 0) {
        clockLength(m_sq1);
        clockLength(m_sq2);
        clockLength(m_wave);
        clockLength(m_noise);
        if ((m_frameStep & 3) == 2) sqClockSweep(m_sq1);
    }
    clockEnvelope(m_sq1, [this]() { sqUpdateOutput(m_sq1); });
    clockEnvelope(m_sq2, [this]() { sqUpdateOutput(m_sq2); });
    clockEnvelope(m_noise, [this]() { noiseUpdateOutput(); });
    m_frameStep = (m_frameStep + 1) & 7;
}

void Apu::speedChanged(bool high) {
    // Each switch to double speed with the APU on turns the frame sequencer's delay on or off
    // (the first, third... on; AGE's after_speed_change, spsw-ch2-lc-delay)
    if (m_enabled && high) m_speedSwitchDelay = !m_speedSwitchDelay;
}

void Apu::frameSequencerRise() {
    run();
    if (!m_enabled) return;
    armEnvelope(m_sq1);
    armEnvelope(m_sq2);
    armEnvelope(m_noise);
}

template <class T> void Apu::clockLength(T& c) {
    if (c.lengthEnabled && c.length > 0) {
        if (--c.length == 0) { c.enabled = false; c.output = 0; }
    }
}

// The envelope (SameBoy's model, apu.c): a 3-bit countdown, counted down on step 6 while
// the envelope's clock is low. On the frame sequencer bit's rising edge a countdown at 0
// reloads with the period and, for a period other than 0, raises the clock; the next step
// steps the volume and lowers it. Raised at volume 15 going up (0 going down) the clock
// locks the envelope until the next trigger. So a period written from 0 to more steps the
// volume at the second step after it (channel_1_nrx2_speed_change).

template <class T> void Apu::setEnvelopeClock(T& s, bool on) {
    if (s.envClock == on) return;
    s.envClock = on;
    if (on) s.envShouldLock = (s.volume == 0x0F && s.envRaise) || (s.volume == 0 && !s.envRaise);
    else s.envLocked |= s.envShouldLock;
}

template <class T> void Apu::armEnvelope(T& s) {
    if (!s.enabled || s.envCountdown != 0) return;
    s.envCountdown = s.envPeriod;
    setEnvelopeClock(s, s.envPeriod != 0);
}

template <class T, class U> void Apu::clockEnvelope(T& s, U update) {
    if (!s.envClock) {
        if (m_frameStep == 6) s.envCountdown = (s.envCountdown - 1) & 7;
        return;
    }
    setEnvelopeClock(s, false);
    if (s.envLocked || s.envPeriod == 0) return;
    s.volume = (uint8_t)((s.envRaise ? s.volume + 1 : s.volume - 1) & 0x0F);
    update();                                         // at once (div_trigger_volume, channel_4_volume_div)
}

template <class T, class U> void Apu::writeEnvelope(T& s, uint8_t v, U update) {
    const bool raise = (v & 0x08) != 0;
    const uint8_t period = v & 0x07;
    if ((v & 0xF8) == 0) {
        s.enabled = false;
        s.output = 0;
    } else if (s.enabled) {
        // "Zombie mode" (SameBoy's nrx2_glitch, CGB D/E): the volume stepped or inverted
        const uint8_t old = (uint8_t)((s.envVolume << 4) | (s.envRaise ? 0x08 : 0) | s.envPeriod);
        if (s.envClock) s.envCountdown = period;
        bool tick = period && !(old & 7) && !s.envLocked;
        if ((v & 0x0F) == 8 && (old & 0x0F) == 8 && !s.envLocked) tick = true;
        if (raise != s.envRaise) {
            if (raise) {
                if (!(old & 7) && !s.envLocked) s.volume ^= 0x0F;
                else s.volume = (uint8_t)((14 - s.volume) & 0x0F);
                tick = false;
            } else {
                s.volume = (uint8_t)((16 - s.volume) & 0x0F);
            }
        }
        if (tick) s.volume = (uint8_t)((raise ? s.volume + 1 : s.volume - 1) & 0x0F);
        else if (!period && s.envClock) setEnvelopeClock(s, false);
    }
    s.envPeriod = period;
    s.envRaise = raise;
    s.envVolume = (uint8_t)((v & 0xF0) >> 4);
    update();
}

template <class L> void Apu::lengthEnableFlag(uint8_t v, L& length, bool& lengthEnabled, bool& enabled) {
    const bool newEnabled = (v & 0x40) != 0;
    if (newEnabled && !lengthEnabled && ((m_frameStep & 1) == 1 || m_skipFirstEvent)) {
        // The extra length clock when the next step doesn't clock lengths
        if (length > 0 && --length == 0) {
            if (v & 0x80) length = sizeof(L) == 1 ? 0x3F : 0xFF;
            else enabled = false;
        }
    }
    lengthEnabled = newEnabled;
}

// ---------------------------------------------------------------------------
// Square channels
// ---------------------------------------------------------------------------

void Apu::sqDisable(Square& s) {
    const uint8_t len = s.length;
    const Dac dac = s.dac;
    s = Square{};
    s.dac = dac;
    s.length = len;
    s.timer = 2048 * 4;
}

uint16_t Apu::sqSweepTarget(const Square& s) const {
    const uint16_t shifted = (uint16_t)(s.sweepFreq >> s.sweepShift);
    return s.sweepNegate ? (uint16_t)(s.sweepFreq - shifted) : (uint16_t)(s.sweepFreq + shifted);
}

void Apu::sqClockSweep(Square& s) {
    if (!s.sweepEnabled) return;
    if (--s.sweepTimer == 0) {
        s.sweepTimer = s.sweepPeriod ? s.sweepPeriod : 8;
        if (s.sweepPeriod == 0) return;
        const uint16_t f = sqSweepTarget(s);
        if (s.sweepNegate) s.sweepNegateCalcDone = true;
        // Just after a trigger the calculation keeps the trigger's addend, none for shift 0 (SameBoy's
        // channel_1_restart_hold: channel_1_sweep_restart_2)
        if (s.sweepShift == 0 && m_gb->apuCycles() - s.triggerClock < SWEEP_RESTART_HOLD) return;
        if (f >= 2048) {
            // The overflow turns the channel off once the calculation is done, a moment later
            s.sweepEnabled = false;
            s.sweepOverflowDelay = 2;
        } else if (s.sweepShift) {
            sqSetFrequency(s, f, true);
            s.sweepFreq = f;
            s.sweepUpdateDelay = 8 * 4;          // the next check 8 CPU cycles later (channel_1_sweep)
        }
    }
}

void Apu::sqUpdateOutput(Square& s) {
    if (!s.enabled) return;
    s.output = s.firstStep ? 0 : (uint8_t)(DUTY[s.duty][(s.dutyPos - 1) & 7] * s.volume);
}

void Apu::sqExec(Square& s, uint32_t clocks) {
    s.dac.exec(clocks, s.envVolume || s.envRaise);
    if (!s.enabled) return;
    if (s.sweepOverflowDelay) {
        if (s.sweepOverflowDelay > clocks) {
            s.sweepOverflowDelay = (uint8_t)(s.sweepOverflowDelay - clocks);
        } else {
            s.sweepOverflowDelay = 0;
            s.enabled = false;
            s.output = 0;
            return;
        }
    }
    if (s.sweepUpdateDelay) {
        if (s.sweepUpdateDelay > clocks) {
            s.sweepUpdateDelay = (uint8_t)(s.sweepUpdateDelay - clocks);
        } else {
            s.sweepUpdateDelay = 0;
            if (s.sweepShift && sqSweepTarget(s) >= 2048) {
                s.enabled = false;
                s.sweepEnabled = false;
                s.output = 0;
                return;
            }
        }
    }
    s.timer = (uint16_t)(s.timer - clocks);
    if (s.timer == 0) {
        s.timer = (uint16_t)((2048 - s.frequency) * 4);
        s.dutyPos = (s.dutyPos + 1) & 7;
        s.firstStep = false;
        sqUpdateOutput(s);
    }
}

void Apu::sqSetFrequency(Square& s, uint16_t f, bool sweep) {
    // The sweep's change on the clock the timer reloads: the reload takes it (channel_1_sweep_restart,
    // as SameBoy's just_reloaded). A write taking the high bits from 7 to less within a clock of the
    // reload, the same (CGB D/E: channel_1_freq_change_timing-cgbDE; SameBoy's ">= $700 to < $700")
    const int elapsed = (2048 - s.frequency) * 4 - s.timer;
    const int window = sweep ? 1 : ((s.frequency >> 8) == 7 && (f >> 8) != 7) ? 4 : 0;
    if (s.enabled && elapsed >= 0 && elapsed < window) s.timer = (uint16_t)((2048 - f) * 4 - elapsed);
    s.frequency = f;
}

uint8_t Apu::sqRead(const Square& s, int reg) const {
    static const uint8_t openBus[5] = {0x80, 0x3F, 0x00, 0xFF, 0xBF};
    uint8_t v = 0;
    switch (reg) {
    case 0: v = (uint8_t)((s.sweepPeriod << 4) | (s.sweepNegate ? 0x08 : 0) | s.sweepShift); break;
    case 1: v = (uint8_t)(s.duty << 6); break;
    case 2: v = (uint8_t)((s.envVolume << 4) | (s.envRaise ? 0x08 : 0) | s.envPeriod); break;
    case 4: v = s.lengthEnabled ? 0x40 : 0; break;
    }
    return v | openBus[reg];
}

void Apu::sqWrite(Square& s, int reg, uint8_t v) {
    switch (reg) {
    case 0:
        s.sweepShift = v & 0x07;
        s.sweepNegate = (v & 0x08) != 0;
        s.sweepPeriod = (uint16_t)((v & 0x70) >> 4);
        if (!s.sweepNegate && s.sweepNegateCalcDone) { s.enabled = false; s.output = 0; }   // sweep-details 4-6
        break;
    case 1:
        s.length = (uint8_t)(64 - (v & 0x3F));
        s.duty = (uint8_t)((v & 0xC0) >> 6);
        break;
    case 2:
        writeEnvelope(s, v, [&]() { sqUpdateOutput(s); });
        sqUpdateOutput(s);
        break;
    case 3:
        s.frequency = (uint16_t)((s.frequency & 0x700) | v);
        break;
    case 4: {
        const bool prevEnabled = s.enabled;
        sqSetFrequency(s, (uint16_t)((s.frequency & 0xFF) | ((v & 0x07) << 8)));
        if (v & 0x80) {
            s.triggerClock = m_gb->apuCycles();
            s.sweepOverflowDelay = 0;
            // Trigger: the timer's low two bits aren't changed
            s.timer = (uint16_t)(((2048 - s.frequency) * 4) + 8 + (oddApuCycle() ? 2 : 0));
            if (s.enabled) s.timer = (uint16_t)(s.timer - 4);        // channel_1_restart
            s.volume = s.envVolume;
            if (s.enabled) sqUpdateOutput(s);                         // channel_1_restart_nrx2_glitch
            s.enabled = s.envRaise || s.envVolume > 0;
            if (s.enabled && !prevEnabled) s.firstStep = true;        // silent until the first step (Daiku no Gen-san)
            if (s.length == 0) { s.length = 64; s.lengthEnabled = false; }
            s.envCountdown = s.envPeriod;
            s.envClock = s.envLocked = false;
            s.sweepFreq = s.frequency;
            s.sweepTimer = s.sweepPeriod ? s.sweepPeriod : 8;
            s.sweepNegateCalcDone = false;
            s.sweepEnabled = s.sweepPeriod > 0 || s.sweepShift > 0;
            if (s.sweepShift > 0) {
                s.sweepUpdateDelay = 8 * 4 + 2;
                if (s.sweepNegate) s.sweepNegateCalcDone = true;
            }
        }
        lengthEnableFlag(v, s.length, s.lengthEnabled, s.enabled);
        if (!s.enabled && prevEnabled) { s.output = 0; sqUpdateOutput(s); }
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// The wave channel
// ---------------------------------------------------------------------------

void Apu::waveDisable() {
    const uint16_t len = m_wave.length;
    uint8_t ram[16];
    std::memcpy(ram, m_wave.ram, 16);
    const Dac dac = m_wave.dac;
    m_wave = Wave{};
    m_wave.dac = dac;
    m_wave.length = len;
    std::memcpy(m_wave.ram, ram, 16);
    m_wave.timer = 2048 * 2;
}

void Apu::waveUpdateOutput() {
    if (!m_wave.enabled) return;
    m_wave.output = m_wave.volume ? (uint8_t)(m_wave.sampleBuffer >> (m_wave.volume - 1)) : 0;
}

void Apu::waveExec(uint32_t clocks) {
    m_wave.dac.exec(clocks, m_wave.dacEnabled);
    if (!m_wave.enabled) return;
    m_wave.timer = (uint16_t)(m_wave.timer - clocks);
    m_wave.allowRamAccess = false;
    if (m_wave.timer == 0) {
        m_wave.timer = (uint16_t)((2048 - m_wave.frequency) * 2);
        m_wave.position = (m_wave.position + 1) & 0x1F;
        m_wave.sampleBuffer = (m_wave.position & 1) ? (m_wave.ram[m_wave.position >> 1] & 0x0F) : (m_wave.ram[m_wave.position >> 1] >> 4);
        waveUpdateOutput();
        m_wave.allowRamAccess = true;
    }
}

void Apu::waveCorruption() {
    if (m_gb->isCgb()) return;                    // the DMG's only
    const uint8_t pos = (uint8_t)(((m_wave.position + 1) & 0x1F) >> 1);
    if (pos < 4) m_wave.ram[0] = m_wave.ram[pos];
    else std::memmove(m_wave.ram, m_wave.ram + (pos & 0x0C), 4);
}

void Apu::waveWrite(int reg, uint8_t v) {
    switch (reg) {
    case 0:
        m_wave.dacEnabled = (v & 0x80) != 0;
        m_wave.enabled &= m_wave.dacEnabled;
        break;
    case 1: m_wave.length = (uint16_t)(256 - v); break;
    case 2:
        m_wave.volume = (uint8_t)((v & 0x60) >> 5);
        waveUpdateOutput();                       // at once (channel_3_shift_delay)
        break;
    case 3: m_wave.frequency = (uint16_t)((m_wave.frequency & 0x700) | v); break;
    case 4: {
        const bool prevEnabled = m_wave.enabled;
        m_wave.frequency = (uint16_t)((m_wave.frequency & 0xFF) | ((v & 0x07) << 8));
        if (v & 0x80) {
            if (m_wave.enabled && m_wave.timer <= 2) waveCorruption();    // retriggered while reading a sample
            m_wave.enabled = m_wave.dacEnabled;
            if (m_wave.enabled) waveUpdateOutput();
            m_wave.timer = (uint16_t)((2048 - m_wave.frequency) * 2 + 6);
            if (m_wave.length == 0) { m_wave.length = 256; m_wave.lengthEnabled = false; }
            m_wave.position = 0;
        }
        lengthEnableFlag(v, m_wave.length, m_wave.lengthEnabled, m_wave.enabled);
        if (!m_wave.enabled && prevEnabled) { m_wave.output = 0; waveUpdateOutput(); }
        break;
    }
    }
}

uint8_t Apu::waveReadRam(uint16_t addr) const {
    if (!m_wave.enabled) return m_wave.ram[addr & 0x0F];
    // Playing, the byte being played: the DMG only on the clock the channel reads it, the CGB any time
    return (m_wave.allowRamAccess || m_gb->isCgb()) ? m_wave.ram[m_wave.position >> 1] : 0xFF;
}

void Apu::waveWriteRam(uint16_t addr, uint8_t v) {
    if (!m_wave.enabled) m_wave.ram[addr & 0x0F] = v;
    else if (m_wave.allowRamAccess || m_gb->isCgb()) m_wave.ram[m_wave.position >> 1] = v;
}

// ---------------------------------------------------------------------------
// The noise channel
// ---------------------------------------------------------------------------

void Apu::noiseDisable() {
    const uint8_t len = m_noise.length;
    const Dac dac = m_noise.dac;
    m_noise = Noise{};
    m_noise.dac = dac;
    m_noise.length = len;
    m_noise.timer = noisePeriod();
}

uint32_t Apu::noisePeriod() const {
    return m_noise.divisor == 0 ? (8u << m_noise.periodShift) : ((16u * m_noise.divisor) << m_noise.periodShift);
}

void Apu::noiseUpdateOutput() {
    if (m_noise.enabled) m_noise.output = (uint8_t)(((m_noise.shiftRegister & 1) ^ 1) * m_noise.volume);
}

void Apu::noiseExec(uint32_t clocks) {
    m_noise.dac.exec(clocks, m_noise.envVolume || m_noise.envRaise);
    if (!m_noise.enabled || m_noise.periodShift >= 14) return;      // shifts 14 and 15: no clocks
    m_noise.timer -= clocks;
    if (m_noise.timer == 0) {
        m_noise.timer = noisePeriod();
        const uint16_t shifted = (uint16_t)(m_noise.shiftRegister >> 1);
        const uint8_t x = (uint8_t)((m_noise.shiftRegister & 1) ^ (shifted & 1));
        m_noise.shiftRegister = (uint16_t)((x << 14) | shifted);
        if (m_noise.shortWidth) m_noise.shiftRegister = (uint16_t)((m_noise.shiftRegister & ~0x40) | (x << 6));
        noiseUpdateOutput();
    }
}

void Apu::noiseWrite(int reg, uint8_t v) {
    switch (reg) {
    case 1: m_noise.length = (uint8_t)(64 - (v & 0x3F)); break;
    case 2: writeEnvelope(m_noise, v, [this]() { noiseUpdateOutput(); }); break;
    case 3:
        m_noise.periodShift = (uint8_t)((v & 0xF0) >> 4);
        m_noise.shortWidth = (v & 0x08) != 0;
        m_noise.divisor = v & 0x07;
        break;
    case 4: {
        const bool prevEnabled = m_noise.enabled;
        if (v & 0x80) {
            // The first period halved plus 8 (SameSuite's channel_4_delay, as Mesen)
            m_noise.timer = (noisePeriod() / 2 + 8) + (oddApuCycle() ? 2 : 0);
            if (m_noise.enabled) m_noise.timer += 4;
            if (m_noise.divisor && !(elapsedApuCycles() & 0x04)) m_noise.timer += (m_noise.divisor == 1) ? 4 : (uint32_t)-4;
            m_noise.volume = m_noise.envVolume;
            if (m_noise.enabled) noiseUpdateOutput();
            m_noise.enabled = m_noise.envRaise || m_noise.envVolume > 0;
            m_noise.shiftRegister = 0x7FFF;
            if (m_noise.length == 0) { m_noise.length = 64; m_noise.lengthEnabled = false; }
            m_noise.envCountdown = m_noise.envPeriod;
            m_noise.envClock = m_noise.envLocked = false;
        }
        lengthEnableFlag(v, m_noise.length, m_noise.lengthEnabled, m_noise.enabled);
        if (!m_noise.enabled && prevEnabled) { m_noise.output = 0; noiseUpdateOutput(); }
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

uint8_t Apu::read(uint16_t addr) {
    run();
    return peek(addr);
}

uint8_t Apu::peek(uint16_t addr) const {
    if (addr >= 0xFF10 && addr <= 0xFF14) return sqRead(m_sq1, addr - 0xFF10);
    if (addr >= 0xFF16 && addr <= 0xFF19) return sqRead(m_sq2, addr - 0xFF15);
    if (addr >= 0xFF1A && addr <= 0xFF1E) {
        static const uint8_t openBus[5] = {0x7F, 0xFF, 0x9F, 0xFF, 0xBF};
        const int r = addr - 0xFF1A;
        uint8_t v = 0;
        if (r == 0) v = m_wave.dacEnabled ? 0x80 : 0;
        else if (r == 2) v = (uint8_t)(m_wave.volume << 5);
        else if (r == 4) v = m_wave.lengthEnabled ? 0x40 : 0;
        return v | openBus[r];
    }
    if (addr >= 0xFF20 && addr <= 0xFF23) {
        static const uint8_t openBus[5] = {0xFF, 0xFF, 0x00, 0x00, 0xBF};
        const int r = addr - 0xFF1F;
        uint8_t v = 0;
        if (r == 2) v = (uint8_t)((m_noise.envVolume << 4) | (m_noise.envRaise ? 0x08 : 0) | m_noise.envPeriod);
        else if (r == 3) v = (uint8_t)((m_noise.periodShift << 4) | (m_noise.shortWidth ? 0x08 : 0) | m_noise.divisor);
        else if (r == 4) v = m_noise.lengthEnabled ? 0x40 : 0;
        return v | openBus[r];
    }
    switch (addr) {
    case 0xFF24: return (uint8_t)((m_extLeft ? 0x80 : 0) | (m_leftVolume << 4) | (m_extRight ? 0x08 : 0) | m_rightVolume);
    case 0xFF25:
        return (uint8_t)((m_enLeft[3] ? 0x80 : 0) | (m_enLeft[2] ? 0x40 : 0) | (m_enLeft[1] ? 0x20 : 0) | (m_enLeft[0] ? 0x10 : 0) |
                         (m_enRight[3] ? 0x08 : 0) | (m_enRight[2] ? 0x04 : 0) | (m_enRight[1] ? 0x02 : 0) | (m_enRight[0] ? 0x01 : 0));
    case 0xFF26:
        return (uint8_t)((m_enabled ? 0x80 : 0) | 0x70 | ((m_enabled && m_noise.enabled) ? 0x08 : 0) |
                         ((m_enabled && m_wave.enabled) ? 0x04 : 0) | ((m_enabled && m_sq2.enabled) ? 0x02 : 0) |
                         ((m_enabled && m_sq1.enabled) ? 0x01 : 0));
    }
    if (addr >= 0xFF30 && addr <= 0xFF3F) return waveReadRam(addr);
    return 0xFF;
}

uint8_t Apu::pcm(uint16_t addr) const {
    // A square's or the noise's output is 0 with its DAC off (Mesen's GetRawOutput); the wave's with the channel off
    auto env = [](const Envelope& c, uint8_t out) { return (!c.envRaise && c.envVolume == 0) ? 0 : out; };
    if (addr == 0xFF76) return (uint8_t)(env(m_sq1, m_sq1.output) | (env(m_sq2, m_sq2.output) << 4));
    return (uint8_t)((m_wave.enabled ? m_wave.output : 0) | (env(m_noise, m_noise.output) << 4));
}

void Apu::write(uint16_t addr, uint8_t v) {
    run();
    if (!m_enabled) {
        if (addr == 0xFF11 || addr == 0xFF16 || addr == 0xFF20) v &= 0x3F;   // the lengths, not the duty
        else if (addr < 0xFF26 && addr != 0xFF1B) return;
    }
    if (addr >= 0xFF10 && addr <= 0xFF14) sqWrite(m_sq1, addr - 0xFF10, v);
    else if (addr >= 0xFF16 && addr <= 0xFF19) sqWrite(m_sq2, addr - 0xFF15, v);
    else if (addr >= 0xFF1A && addr <= 0xFF1E) waveWrite(addr - 0xFF1A, v);
    else if (addr >= 0xFF20 && addr <= 0xFF23) noiseWrite(addr - 0xFF1F, v);
    else if (addr == 0xFF24) {
        m_extLeft = (v & 0x80) != 0;
        m_leftVolume = (uint8_t)((v & 0x70) >> 4);
        m_extRight = (v & 0x08) != 0;
        m_rightVolume = v & 0x07;
    } else if (addr == 0xFF25) {
        for (int i = 0; i < 4; ++i) {
            m_enLeft[i] = (v & (0x10 << i)) ? 0xFF : 0;
            m_enRight[i] = (v & (0x01 << i)) ? 0xFF : 0;
        }
    } else if (addr == 0xFF26) {
        const bool on = (v & 0x80) != 0;
        if (m_enabled != on) {
            if (!on) {
                sqDisable(m_sq1); sqDisable(m_sq2); waveDisable(); noiseDisable();
                write(0xFF24, 0);
                write(0xFF25, 0);
            } else {
                // Powered on with DIV's bit 4 set: the first frame sequencer event is skipped
                m_skipFirstEvent = m_gb->frameSequencerBit() ? 2 : 0;
                m_frameStep = 0;
                m_speedSwitchDelay = false;
                m_powerOnCycle = m_gb->apuCycles();
                sqDisable(m_sq1); sqDisable(m_sq2); noiseDisable(); waveDisable();
                if (m_gb->isCgb()) m_sq1.length = m_sq2.length = m_noise.length = m_wave.length = 0;   // the CGB clears them
            }
            m_enabled = on;
        }
    } else if (addr >= 0xFF30 && addr <= 0xFF3F) {
        waveWriteRam(addr, v);
    }
    updateOutput();          // some writes change the output at once
}

}  // namespace gb
