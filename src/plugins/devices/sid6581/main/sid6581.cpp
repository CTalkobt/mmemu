#include "sid6581.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
// The synthesis follows reSID 0.16 (Dag Lem): every clock cycle the
// oscillators, noise LFSR, envelope generators and filter are clocked (#123),
// and the per-cycle output is averaged down to the audio sample rate. Voice
// and filter levels are in reSID's units (20-bit voice output >> 7), as floats.

namespace {

// Envelope rate counter periods (cycles per step) for the 4-bit A/D/R values
const uint16_t RATE_PERIOD[16] = {
    9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532, 31251
};

// One voice's full swing: 12-bit waveform x 8-bit envelope, >> 7
constexpr float VOICE_RANGE = 4095.0f * 255.0f / 128.0f;

// MOS 6581 DC offsets (reSID, measured): the waveform DAC's "zero" is 0x380,
// not 0x800, the envelope DAC adds 0x800 * 0xFF, and the mixer has its own
// offset. The master volume multiplies the DC too, which is why writing $D418
// alone makes sound (volume-register "digis", #122). The 8580 has none.
constexpr float WAVE_ZERO_6581 = 0x380;
constexpr float VOICE_DC_6581  = 0x800 * 0xFF / 128.0f;
constexpr float MIXER_DC_6581  = -(0xFFF * 0xFF / 18) / 128.0f;

// Filter cutoff (Hz) against the 11-bit FC register, measured on real chips
// (reSID 0.16 f0_points, Dag Lem). The 6581 curve is strongly nonlinear and
// drops at $400; the 8580 is close to linear. Linear interpolation between
// the points.
struct FcPoint { int fc; float hz; };
const FcPoint FC_6581[] = {
    {    0,   220 }, {  128,   230 }, {  256,   250 }, {  384,   300 }, {  512,   420 },
    {  640,   780 }, {  768,  1600 }, {  832,  2300 }, {  896,  3200 }, {  960,  4300 },
    {  992,  5000 }, { 1008,  5400 }, { 1016,  5700 }, { 1023,  6000 },
    { 1024,  4600 }, { 1032,  4800 }, { 1056,  5300 }, { 1088,  6000 }, { 1120,  6600 },
    { 1152,  7200 }, { 1280,  9500 }, { 1408, 12000 }, { 1536, 14500 }, { 1664, 16000 },
    { 1792, 17100 }, { 1920, 17700 }, { 2047, 18000 },
};
const FcPoint FC_8580[] = {
    {    0,     0 }, {  128,   800 }, {  256,  1600 }, {  384,  2500 }, {  512,  3300 },
    {  640,  4100 }, {  768,  4800 }, {  896,  5600 }, { 1024,  6500 }, { 1152,  7500 },
    { 1280,  8400 }, { 1408,  9200 }, { 1536,  9800 }, { 1664, 10500 }, { 1792, 11000 },
    { 1920, 11700 }, { 2047, 12500 },
};

template <size_t N>
float cutoffHz(const FcPoint (&pts)[N], int fc) {
    for (size_t i = 1; i < N; ++i) {
        if (fc <= pts[i].fc) {
            const FcPoint& a = pts[i - 1];
            const FcPoint& b = pts[i];
            if (b.fc == a.fc) return b.hz;
            return a.hz + (b.hz - a.hz) * (float)(fc - a.fc) / (float)(b.fc - a.fc);
        }
    }
    return pts[N - 1].hz;
}

// Op-amp headroom of the filter integrators (#121), from reSID 1.0's op-amp
// transfer curves measured on a 6581R4AR and an 8580R5: 6581 outputs swing
// 0.81-10.31 V around a 4.54 V working point with one voice spanning ~1.5 V;
// the 8580 1.30-8.91 V around ~4.81 V with ~1.0 V per voice. The smaller side
// gives the limit in voice ranges: 3.73 / 1.5 and 3.51 / 1.0.
constexpr float HEADROOM_6581 = 3.73f / 1.5f;
constexpr float HEADROOM_8580 = 3.51f / 1.0f;

// Soft limit: linear up to `knee`, then bending smoothly towards `limit`.
inline float saturate(float x, float knee, float limit) {
    float a = std::fabs(x);
    if (a <= knee) return x;
    float span = limit - knee;
    float y = knee + span * std::tanh((a - knee) / span);
    return x < 0 ? -y : y;
}

// External output stage of the C64 (reSID): 10k/1000pF low-pass (w0 = 1e5
// rad/s, ~16 kHz) and 10k/10uF high-pass (w0 = 100 rad/s, ~16 Hz).
constexpr float EXT_W0_LP = 100000.0f;
constexpr float EXT_W0_HP = 100.0f;

}  // namespace

SID6581::SID6581(const std::string& name, uint32_t baseAddr)
    : m_name(name), m_baseAddr(baseAddr)
{
    SID6581::reset();
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

void SID6581::reset() {
    std::memset(m_regs, 0, sizeof(m_regs));
    for (auto& v : m_voices) {
        v = Voice();
        v.ratePeriod = RATE_PERIOD[0];
    }
    m_vhp = m_vbp = m_vlp = 0.0f;
    m_extLp = m_extHp = 0.0f;
    m_coeffValid = false;
    m_sampleFrac    = 0;
    m_sampleSum     = 0.0;
    m_sampleCycles  = 0;
    m_audioBufRead  = 0;
    m_audioBufWrite = 0;
    m_audioBufCount = 0;
    m_scopePos   = 0;
    m_scopeCount = 0;
    m_scopeTotal = 0;
}

// ---------------------------------------------------------------------------
// IOHandler: ioRead
// ---------------------------------------------------------------------------

bool SID6581::ioRead(IBus* /*bus*/, uint32_t addr, uint8_t* val) {
    if ((addr & ~addrMask()) != m_baseAddr) return false;
    uint8_t reg = addr & 0x1F;

    switch (reg) {
        case POTX:
        case POTY:
            *val = 0xFF; // paddle inputs not implemented
            break;
        case OSC3:
            // Upper 8 bits of voice 3's waveform output (reSID readOSC): the
            // folded triangle, pulse, noise etc., not the raw accumulator.
            *val = (uint8_t)(m_voices[2].waveOutput(false, ringMsbFor(m_voices[2]), chipModel()) >> 4);
            break;
        case ENV3:
            *val = m_voices[2].envLevel;
            break;
        default:
            // Write-only registers return the last written value (open-bus on real chip,
            // but returning the stored value is the common emulator approximation).
            if (reg <= 0x1C)
                *val = m_regs[reg];
            else
                *val = 0xFF;
            break;
    }
    return true;
}


// ---------------------------------------------------------------------------
// IOHandler: ioWrite
// ---------------------------------------------------------------------------

bool SID6581::ioWrite(IBus* /*bus*/, uint32_t addr, uint8_t val) {
    if ((addr & ~addrMask()) != m_baseAddr) return false;
    uint8_t reg = addr & 0x1F;

    if (reg > 0x18) return true; // $D419–$D41F are read-only or unused

    m_regs[reg] = val;
    if (reg < 21) writeVoiceReg(m_voices[reg / 7], reg % 7, val);
    else m_coeffValid = false;   // cutoff or resonance may have changed
    return true;
}

void SID6581::writeVoiceReg(Voice& v, int reg, uint8_t val) {
    switch (reg) {
        case 0: v.freq = (uint16_t)((v.freq & 0xFF00) | val); break;
        case 1: v.freq = (uint16_t)((v.freq & 0x00FF) | (val << 8)); break;
        case 2: v.pw   = (uint16_t)((v.pw & 0x0F00) | val); break;
        case 3: v.pw   = (uint16_t)((v.pw & 0x00FF) | ((val & 0x0F) << 8)); break;
        case 4: {
            bool gate = (val & CR_GATE) != 0, wasGate = (v.cr & CR_GATE) != 0;
            if (gate && !wasGate) {
                // Attack. The rate counter is not reset: the first step comes
                // when it next reaches the period (the "ADSR delay bug").
                v.envState = Voice::EnvState::ATTACK;
                v.ratePeriod = RATE_PERIOD[v.ad >> 4];
                v.holdZero = false;
            } else if (!gate && wasGate) {
                v.envState = Voice::EnvState::RELEASE;
                v.ratePeriod = RATE_PERIOD[v.sr & 0x0F];
            }
            // Test bit: oscillator held at 0, LFSR reloaded
            if (val & CR_TEST) {
                v.phase = 0;
                v.lfsr  = 0x7FFFFF;
            }
            v.cr = val;
            break;
        }
        case 5:
            v.ad = val;
            if (v.envState == Voice::EnvState::ATTACK)             v.ratePeriod = RATE_PERIOD[val >> 4];
            else if (v.envState == Voice::EnvState::DECAY_SUSTAIN) v.ratePeriod = RATE_PERIOD[val & 0x0F];
            break;
        case 6:
            v.sr = val;
            if (v.envState == Voice::EnvState::RELEASE) v.ratePeriod = RATE_PERIOD[val & 0x0F];
            break;
    }
}

// ---------------------------------------------------------------------------
// IOHandler: tick — advance synthesis and push samples
// ---------------------------------------------------------------------------

void SID6581::tick(uint64_t cycles) {
    synthesize(cycles);
}

// ---------------------------------------------------------------------------
// Envelope generator (reSID 0.16)
// ---------------------------------------------------------------------------

void SID6581::Voice::clockEnvelope() {
    // 15-bit rate counter compared with the period; it is never reset by a
    // gate change, so a smaller new period can mean a wait of up to 2^15
    // cycles for the wraparound (ADSR delay bug).
    if (++rateCounter & 0x8000) rateCounter = (uint16_t)((rateCounter + 1) & 0x7FFF);
    if (rateCounter != ratePeriod) return;
    rateCounter = 0;

    // Decay and release are slowed further by the exponential counter
    if (envState != EnvState::ATTACK && ++expCounter != expPeriod) return;
    expCounter = 0;
    if (holdZero) return;

    switch (envState) {
        case EnvState::ATTACK:
            ++envLevel;
            if (envLevel == 0xFF) {
                envState = EnvState::DECAY_SUSTAIN;
                ratePeriod = RATE_PERIOD[ad & 0x0F];
            }
            break;
        case EnvState::DECAY_SUSTAIN:
            // Sustain level: both nibbles of the counter compared with S
            if (envLevel != (uint8_t)((sr >> 4) * 0x11)) --envLevel;
            break;
        case EnvState::RELEASE:
            --envLevel;
            break;
    }

    // Piecewise-linear approximation of an exponential decay
    switch (envLevel) {
        case 0xFF: expPeriod = 1;  break;
        case 0x5D: expPeriod = 2;  break;
        case 0x36: expPeriod = 4;  break;
        case 0x1A: expPeriod = 8;  break;
        case 0x0E: expPeriod = 16; break;
        case 0x06: expPeriod = 30; break;
        case 0x00: expPeriod = 1; holdZero = true; break;
        default: break;
    }
}

SID6581::Voice::Env SID6581::Voice::envPhase() const {
    switch (envState) {
        case EnvState::ATTACK: return Env::ATTACK;
        case EnvState::DECAY_SUSTAIN:
            return envLevel == (uint8_t)((sr >> 4) * 0x11) ? Env::SUSTAIN : Env::DECAY;
        default:
            return envLevel == 0 ? Env::IDLE : Env::RELEASE;
    }
}

// ---------------------------------------------------------------------------
// Synthesis: every cycle (#123)
// ---------------------------------------------------------------------------

void SID6581::updateFilterCoeffs() {
    int fc = (m_regs[FC_LO] & 0x07) | (m_regs[FC_HI] << 3);
    int res = m_regs[RES_FILT] >> 4;
    bool is6581 = m_filterVariant == 0;
    float hz = is6581 ? cutoffHz(FC_6581, fc) : cutoffHz(FC_8580, fc);
    hz = std::min(hz, 16000.0f);                        // reSID's single-cycle ceiling
    m_w0dt = 2.0f * 3.14159265f * hz / (float)m_clockHz;
    m_invQ = 1.0f / (0.707f + (float)res / 15.0f);       // Q 0.707 .. 1.707
    float limit = (is6581 ? HEADROOM_6581 : HEADROOM_8580) * VOICE_RANGE;
    m_filterLo = 0.75f * limit;                          // knee
    m_filterHi = limit;
    m_coeffValid = true;
}

void SID6581::synthesize(uint64_t cycles) {
    if (!m_coeffValid) updateFilterCoeffs();

    const bool is6581 = m_filterVariant == 0;
    const auto chip = chipModel();
    const float waveZero = is6581 ? WAVE_ZERO_6581 : 2048.0f;
    const float voiceDc  = is6581 ? VOICE_DC_6581 : 0.0f;
    const float mixerDc  = is6581 ? MIXER_DC_6581 : 0.0f;

    const uint8_t filt    = m_regs[RES_FILT] & 0x07;
    const uint8_t modeVol = m_regs[MODE_VOL];
    const float   vol     = (float)(modeVol & 0x0F);
    const bool    v3off   = (modeVol & MV_3OFF) && !(filt & 0x04);

    // DC of the filtered voices: the filter's steady state passes it
    // inverted on the low-pass output only (Vlp = -Vi), so it is added there
    // rather than run through the integrators.
    float filtDc = 0.0f;
    for (int i = 0; i < 3; ++i)
        if (filt & (1 << i)) filtDc += voiceDc;

    const float extLpK = EXT_W0_LP / (float)m_clockHz;
    const float extHpK = EXT_W0_HP / (float)m_clockHz;
    // Full scale: three voices at full volume, as reSID. (Two clipped loud
    // tunes such as Rob Hubbard's Commando.)
    const float outScale = 1.0f / (VOICE_RANGE * 3.0f * 15.0f);

    for (uint64_t c = 0; c < cycles; ++c) {
        // 1. Envelopes and oscillators. Hard sync is applied after all three
        //    accumulators have advanced, from the sources' MSB rising edges.
        for (Voice& v : m_voices) {
            v.clockEnvelope();
            if (v.cr & CR_TEST) { v.msbRose = false; continue; }
            uint32_t old = v.phase;
            v.phase = (old + v.freq) & 0xFFFFFF;
            v.msbRose = !(old & 0x800000) && (v.phase & 0x800000);
            // Noise LFSR: shifted when accumulator bit 19 goes high
            if (!(old & 0x080000) && (v.phase & 0x080000)) {
                uint32_t bit = ((v.lfsr >> 22) ^ (v.lfsr >> 17)) & 1u;
                v.lfsr = ((v.lfsr << 1) | bit) & 0x7FFFFF;
                // Noise with another waveform: 0 output bits pull the LFSR
                // bits feeding them low (noise writeback, #119)
                if ((v.cr & CR_NOISE) && (v.cr & 0x70)) {
                    uint16_t out = v.waveOutput(false, ringMsbFor(v), chip);
                    v.lfsr &= ~CombinedWaveformTable::NOISE_TAPS | CombinedWaveformTable::noiseWriteback(out);
                }
            }
        }
        for (int i = 0; i < 3; ++i) {
            Voice& v = m_voices[i];
            const Voice& src = m_voices[(i + 2) % 3];
            const Voice& srcSrc = m_voices[(i + 1) % 3];
            // No sync when the source is itself being synced in this cycle (reSID)
            if ((v.cr & CR_SYNC) && src.msbRose && !((src.cr & CR_SYNC) && srcSrc.msbRose))
                v.phase = 0;
        }

        // 2. Voice outputs: (waveform - zero) x envelope + DC
        float vi = 0.0f, vnf = 0.0f;
        float voice[3];   // for the scope: each voice's swing, -1..1
        for (int i = 0; i < 3; ++i) {
            Voice& v = m_voices[i];
            uint16_t wave = v.waveOutput(false, ringMsbFor(v), chip);
            // 6581: with saw in a combination, a low output top bit pulls the
            // accumulator MSB low (reSIDfp)
            if (is6581 && (v.cr & CR_SAW) && (v.cr & 0xD0) && !(wave & 0x800))
                v.phase &= 0x7FFFFF;
            float ac = ((float)wave - waveZero) * (float)v.envLevel / 128.0f;
            voice[i] = ((float)wave - 2048.0f) / 2048.0f * (float)v.envLevel / 255.0f;
            if (filt & (1 << i))           vi += ac;
            else if (!(i == 2 && v3off))   vnf += ac + voiceDc;
        }

        // 3. Filter (#120): state-variable filter, one step per cycle, the
        //    integrators limited by the op-amps' output swing (#121)
        m_vbp -= m_w0dt * m_vhp;
        m_vlp -= m_w0dt * m_vbp;
        m_vbp = saturate(m_vbp, m_filterLo, m_filterHi);
        m_vlp = saturate(m_vlp, m_filterLo, m_filterHi);
        m_vhp = m_vbp * m_invQ - m_vlp - vi;
        float vf = 0.0f;
        if (modeVol & MV_LP) vf += m_vlp - filtDc;
        if (modeVol & MV_BP) vf += m_vbp;
        if (modeVol & MV_HP) vf += m_vhp;

        // 4. Mixer and master volume (which also scales the DC: digis, #122),
        //    then the external RC filters
        float mix = (vnf + vf + mixerDc) * vol;
        m_extLp += extLpK * (mix - m_extLp);
        m_extHp += extHpK * (m_extLp - m_extHp);
        float out = (m_extLp - m_extHp) * outScale;

        // 5. Resample: average the cycles of each output sample
        m_sampleSum += out;
        ++m_sampleCycles;
        m_sampleFrac += (uint32_t)m_sampleRate;
        if (m_sampleFrac >= m_clockHz) {
            m_sampleFrac -= m_clockHz;
            float s = (float)(m_sampleSum / m_sampleCycles);
            m_sampleSum = 0.0;
            m_sampleCycles = 0;
            s = std::max(-1.0f, std::min(1.0f, s));
            for (int i = 0; i < 3; ++i) m_scope[i][m_scopePos] = voice[i];
            m_scope[3][m_scopePos] = s;
            m_scopePos = (m_scopePos + 1) & (SCOPE_LEN - 1);
            if (m_scopeCount < SCOPE_LEN) ++m_scopeCount;
            ++m_scopeTotal;
            pushSample(s);
        }
    }
}

// ---------------------------------------------------------------------------
// Waveform output
// ---------------------------------------------------------------------------

uint16_t SID6581::Voice::noiseOutput() const {
    return CombinedWaveformTable::noise(lfsr);
}

uint16_t SID6581::Voice::waveOutput(bool syncReset, bool ringMsb,
                                    CombinedWaveformTable::Chip chip) const {
    uint32_t p = syncReset ? 0 : phase;
    // Combined waveforms use the pulldown model fitted to real chips (#119)
    return CombinedWaveformTable::getCombinedOutput(p, cr & 0xF0u, pw, lfsr, ringMsb, chip);
}

bool SID6581::ringMsbFor(const Voice& v) const {
    // Ring modulation only when the voice's RING bit is set: the triangle then
    // folds on (own MSB XOR source MSB), as in reSID. Source = previous voice.
    if (!(v.cr & CR_RING)) return false;
    int srcIdx = (int)(&v - m_voices + 2) % 3;
    return (m_voices[srcIdx].phase & 0x800000u) != 0u;
}

// ---------------------------------------------------------------------------
// Ring buffer
// ---------------------------------------------------------------------------

void SID6581::pushSample(float s) {
    if (m_audioBufCount >= AUDIO_BUF) return; // drop if full
    m_audioBuf[m_audioBufWrite] = s;
    m_audioBufWrite = (m_audioBufWrite + 1) & (AUDIO_BUF - 1);
    ++m_audioBufCount;
}

int SID6581::pullSamples(float* buffer, int maxSamples) {
    int n = (maxSamples < m_audioBufCount) ? maxSamples : m_audioBufCount;
    for (int i = 0; i < n; ++i) {
        buffer[i] = m_audioBuf[m_audioBufRead];
        m_audioBufRead = (m_audioBufRead + 1) & (AUDIO_BUF - 1);
    }
    m_audioBufCount -= n;
    return n;
}

void SID6581::getDeviceInfo(DeviceInfo& out) const {
    out.name = m_name;
    out.baseAddr = m_baseAddr;
    out.addrMask = addrMask();

    auto addReg = [&](const std::string& name, int reg, const std::string& desc = "") {
        out.registers.push_back({name, (uint32_t)reg, m_regs[reg], desc});
    };

    for (int i = 0; i < 3; ++i) {
        std::string v = "V" + std::to_string(i + 1) + "_";
        addReg(v + "FREQ_LO", i * 7 + 0);
        addReg(v + "FREQ_HI", i * 7 + 1);
        addReg(v + "PW_LO",   i * 7 + 2);
        addReg(v + "PW_HI",   i * 7 + 3);
        addReg(v + "CR",      i * 7 + 4);
        addReg(v + "AD",      i * 7 + 5);
        addReg(v + "SR",      i * 7 + 6);
    }
    addReg("FC_LO", 0x15);
    addReg("FC_HI", 0x16);
    addReg("RES_FILT", 0x17);
    addReg("MODE_VOL", 0x18);
    addReg("POTX", 0x19);
    addReg("POTY", 0x1A);
    addReg("OSC3", 0x1B);
    addReg("ENV3", 0x1C);

    for (int i = 0; i < 3; ++i) {
        const char* ph = "UNKNOWN";
        switch (m_voices[i].envPhase()) {
            case Voice::Env::IDLE: ph = "IDLE"; break;
            case Voice::Env::ATTACK: ph = "ATTACK"; break;
            case Voice::Env::DECAY: ph = "DECAY"; break;
            case Voice::Env::SUSTAIN: ph = "SUSTAIN"; break;
            case Voice::Env::RELEASE: ph = "RELEASE"; break;
        }
        std::string prefix = "Voice " + std::to_string(i+1);
        out.state.push_back({prefix + " Env Phase", ph});
        out.state.push_back({prefix + " Env Level", std::to_string(m_voices[i].envLevel)});
    }

    uint16_t fc = ((m_regs[FC_HI] << 3) | (m_regs[FC_LO] & 0x07));
    out.state.push_back({"Filter Cutoff", std::to_string(fc)});
    out.state.push_back({"Filter Res", std::to_string(m_regs[RES_FILT] >> 4)});
    out.state.push_back({"Main Volume", std::to_string(m_regs[MODE_VOL] & 0x0F)});
}

// ---------------------------------------------------------------------------
// IAudioInspect (#30)
// ---------------------------------------------------------------------------

std::string SID6581::chipName(int /*chip*/) const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s $%04X", m_name.c_str(), (unsigned)m_baseAddr);
    return buf;
}

bool SID6581::voiceInfo(int chip, int voice, VoiceInfo& out) const {
    if (chip != 0 || voice < 0 || voice > 2) return false;
    const Voice& v = m_voices[voice];
    out.freq       = v.freq;
    out.freqHz     = (double)v.freq * (double)m_clockHz / 16777216.0;
    out.pulseWidth = v.pw & 0x0FFF;
    out.control    = v.cr;
    out.attack     = v.ad >> 4;
    out.decay      = v.ad & 0x0F;
    out.sustain    = v.sr >> 4;
    out.release    = v.sr & 0x0F;
    switch (v.envPhase()) {
        case Voice::Env::IDLE:    out.envPhase = EnvPhase::Idle;    break;
        case Voice::Env::ATTACK:  out.envPhase = EnvPhase::Attack;  break;
        case Voice::Env::DECAY:   out.envPhase = EnvPhase::Decay;   break;
        case Voice::Env::SUSTAIN: out.envPhase = EnvPhase::Sustain; break;
        case Voice::Env::RELEASE: out.envPhase = EnvPhase::Release; break;
    }
    out.envLevel = v.envLevel;
    out.filtered = (m_regs[RES_FILT] >> voice) & 1;
    out.muted    = voice == 2 && (m_regs[MODE_VOL] & MV_3OFF);
    return true;
}

int SID6581::scopeSamples(int chip, int channel, float* out, int n) const {
    if (chip != 0 || channel < 0 || channel > 3 || n <= 0 || !out) return 0;
    if (n > m_scopeCount) n = m_scopeCount;
    int start = (m_scopePos - n) & (SCOPE_LEN - 1);
    for (int i = 0; i < n; ++i)
        out[i] = m_scope[channel][(start + i) & (SCOPE_LEN - 1)];
    return n;
}
