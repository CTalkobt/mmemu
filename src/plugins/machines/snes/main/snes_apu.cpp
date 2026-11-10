// The SNES sound system after Mesen2 (Core/SNES/Spc.cpp, SpcTimer.h),
// GPL-3.0, Sour and the Mesen contributors.

#include "snes_apu.h"
#include "snes_msu1.h"
#include "libdebug/main/execution_observer.h"

#include <algorithm>
#include <cstring>

namespace snes {

namespace {

// The IPL boot ROM at $FFC0 (Sony; as embedded in Mesen2 Spc.h and bsnes):
// clears the zero page, answers $AA $BB on ports 0/1, then takes blocks of
// bytes over the ports and jumps to the program
const uint8_t IPL[64] = {
    0xCD, 0xEF, 0xBD, 0xE8, 0x00, 0xC6, 0x1D, 0xD0, 0xFC, 0x8F, 0xAA, 0xF4, 0x8F, 0xBB, 0xF5, 0x78,
    0xCC, 0xF4, 0xD0, 0xFB, 0x2F, 0x19, 0xEB, 0xF4, 0xD0, 0xFC, 0x7E, 0xF4, 0xD0, 0x0B, 0xE4, 0xF5,
    0xCB, 0xF4, 0xD7, 0x00, 0xFC, 0xD0, 0xF3, 0xAB, 0x01, 0x10, 0xEF, 0x7E, 0xF4, 0x10, 0xEB, 0xBA,
    0xF6, 0xDA, 0x00, 0xBA, 0xF4, 0xC4, 0xF4, 0xDD, 0x5D, 0xD0, 0xDB, 0x1F, 0x00, 0x00, 0xC0, 0xFF,
};

/** The SPC700's bus: the APU decodes it. */
class ApuBus : public IBus {
public:
    explicit ApuBus(Apu* apu) : m_apu(apu) { m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF}; }
    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "aram"; }
    uint8_t read8(uint32_t a) override {
        const uint8_t v = m_apu->smpRead((uint16_t)a);
        if (m_observer) m_observer->onMemoryRead(this, a & 0xFFFF, v);
        return v;
    }
    uint8_t peek8(uint32_t a) override { return m_apu->smpPeek((uint16_t)a); }
    void write8(uint32_t a, uint8_t v) override {
        if (m_observer) m_observer->onMemoryWrite(this, a & 0xFFFF, peek8(a), v);
        m_apu->smpWrite((uint16_t)a, v);
    }
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}
private:
    Apu* m_apu;
    BusConfig m_cfg;
};

}  // namespace

Apu::Apu() : m_cpu(new Spc700()), m_bus(new ApuBus(this)), m_dsp(m_ram), m_audioBuf(AUDIO_BUF, 0.0f) {
    m_cpu->setDataBus(m_bus);
    m_timers[2].rate = 16;
    reset();
}

Apu::~Apu() {
    if (m_owns) { delete m_cpu; delete m_bus; }
}

void Apu::reset() {
    std::memset(m_ram, 0, sizeof m_ram);
    m_dsp.reset();
    for (auto& t : m_timers) {
        const uint8_t rate = t.rate;
        t = Timer{};
        t.rate = rate;
    }
    m_internalSpeed = m_externalSpeed = 0;
    m_timersEnabled = true; m_timersDisabled = false; m_writeEnabled = true;
    m_inStep = false; m_accesses = 0;
    std::memset(m_fromCpu, 0, sizeof m_fromCpu);
    std::memset(m_fromCpuNew, 0, sizeof m_fromCpuNew);
    m_portsPending = false; m_portsAt = 0;
    std::memset(m_toCpu, 0, sizeof m_toCpu);
    m_dspAddr = 0;
    m_ramReg[0] = m_ramReg[1] = 0;
    m_iplOn = true;
    m_cycle = 0;
    m_cpu->reset();
    m_phase = 0;
    m_acc = 0;
    m_accCount = 0;
    m_lastOut = 0;
    m_audioRead = m_audioWrite = m_audioCount = 0;
}

uint8_t Apu::smpPeek(uint16_t a) const {
    if (a >= 0xFFC0 && m_iplOn) return IPL[a & 0x3F];
    switch (a) {
    case 0xF0: case 0xF1: case 0xFA: case 0xFB: case 0xFC: return 0;
    case 0xF2: return m_dspAddr;
    case 0xF3: return m_dsp.read(m_dspAddr & 0x7F);
    case 0xF4: case 0xF5: case 0xF6: case 0xF7: return m_fromCpu[a & 3];
    case 0xF8: case 0xF9: return m_ramReg[a & 1];
    case 0xFD: case 0xFE: case 0xFF: return m_timers[a - 0xFD].output & 0x0F;
    default: return m_ram[a];
    }
}

void Apu::tick(uint8_t sel) {
    // Mesen's IncCycleCount: 2, 4, 10 or 20 half-cycles; the timers move 2, 4, 8 or 16
    static const uint8_t WAIT[4] = {1, 2, 5, 10}, TIMER_STEP[4] = {2, 4, 8, 16};
    m_dsp.step();
    m_cycle += WAIT[sel & 3];
    for (auto& t : m_timers) t.run(TIMER_STEP[sel & 3]);
}

void Apu::cpuWrite(int port, uint8_t v, uint64_t visibleFrom) {
    port &= 3;
    if (m_fromCpuNew[port] == v) return;
    m_fromCpuNew[port] = v;
    if (visibleFrom <= m_cycle) m_fromCpu[port] = v;
    else { m_portsPending = true; m_portsAt = visibleFrom; }   // then all four, as Mesen
}

uint8_t Apu::smpRead(uint16_t a) {
    const uint64_t at = m_cycle;                               // the cycle this access is in
    if (m_inStep) { ++m_accesses; tick(speedFor(a)); }
    if (m_portsPending && at >= m_portsAt) {
        std::memcpy(m_fromCpu, m_fromCpuNew, sizeof m_fromCpu);
        m_portsPending = false;
    }
    const uint8_t v = smpPeek(a);
    if (a >= 0xFD && a <= 0xFF) m_timers[a - 0xFD].output = 0;   // reading a counter clears it
    return v;
}

void Apu::smpWrite(uint16_t a, uint8_t v) {
    if (m_inStep) { ++m_accesses; tick(speedFor(a)); }
    if (m_writeEnabled) m_ram[a] = v;                              // the registers' RAM too, unless TEST says no
    switch (a) {
    case 0xF0:
        // TEST: only with the direct page flag clear (as Mesen)
        if (!(m_cpu->psw() & 0x20)) {
            m_internalSpeed = (v >> 6) & 3;
            m_externalSpeed = (v >> 4) & 3;
            m_timersEnabled = v & 0x08;
            m_timersDisabled = v & 0x01;
            m_writeEnabled = v & 0x02;
            const bool on = m_timersEnabled && !m_timersDisabled;
            for (auto& t : m_timers) t.setGlobal(on);
        }
        break;
    case 0xF1:
        if (v & 0x10) { m_fromCpu[0] = m_fromCpu[1] = m_fromCpuNew[0] = m_fromCpuNew[1] = 0; }
        if (v & 0x20) { m_fromCpu[2] = m_fromCpu[3] = m_fromCpuNew[2] = m_fromCpuNew[3] = 0; }
        for (int i = 0; i < 3; ++i) m_timers[i].enable(v & (1 << i));
        m_iplOn = v & 0x80;
        break;
    case 0xF2: m_dspAddr = v; break;
    case 0xF3: if (m_dspAddr < 0x80) m_dsp.write(m_dspAddr, v); break;
    case 0xF4: case 0xF5: case 0xF6: case 0xF7: m_toCpu[a & 3] = v; break;
    case 0xF8: case 0xF9: m_ramReg[a & 1] = v; break;
    case 0xFA: case 0xFB: case 0xFC: m_timers[a - 0xFA].target = v; break;
    default: break;
    }
}

void Apu::runTo(uint64_t target) {
    while (m_cycle < target) {
        // The accesses took their time as they happened; the rest of the instruction is idle cycles
        m_inStep = true;
        m_accesses = 0;
        const int n = m_cpu->step();
        m_inStep = false;
        for (int i = m_accesses; i < n; ++i) tick(m_internalSpeed);
    }
    drainDsp();
}

void Apu::drainDsp() {
    const int n = m_dsp.sampleCount();
    const int16_t* s = m_dsp.samples();
    constexpr uint32_t DSP_HZ = CLOCK_HZ / 32;          // 32000
    for (int i = 0; i + 1 < n; i += 2) {
        int32_t l = s[i], r = s[i + 1];
        if (m_extra) {
            m_extra->mix(l, r);
            l = std::clamp(l, -32768, 32767);
            r = std::clamp(r, -32768, 32767);
        }
        m_acc += (l + r) / 65536.0;
        ++m_accCount;
        m_phase += (uint64_t)m_sampleRate;
        while (m_phase >= DSP_HZ) {
            m_phase -= DSP_HZ;
            // Two outputs from one DSP sample (32 kHz up to 44.1 kHz): the second repeats it, not silence
            const float v = m_accCount ? (float)(m_acc / m_accCount) : m_lastOut;
            m_lastOut = v;
            if (m_audioCount == AUDIO_BUF) { m_audioRead = (m_audioRead + 1) % AUDIO_BUF; --m_audioCount; }
            m_audioBuf[m_audioWrite] = std::max(-1.0f, std::min(1.0f, v));
            m_audioWrite = (m_audioWrite + 1) % AUDIO_BUF;
            ++m_audioCount;
            m_acc = 0;
            m_accCount = 0;
        }
    }
    m_dsp.clearSamples();
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

}  // namespace snes
