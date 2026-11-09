#pragma once

#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <deque>
#include <vector>

namespace apple2 {

class Apple2Speaker;

/**
 * The IIgs's sound: the Ensoniq ES5503 DOC behind the sound GLU, mixed with
 * the speaker. The DOC after MAME's es5503.cpp (R. Belmont, BSD-3-Clause,
 * v2.4); the GLU ($C03C-$C03F) after KEGS doc.c and MAME apple2gs.cpp.
 *
 * - 32 oscillators reading 8-bit samples from 64 KB of sound RAM: a 24-bit
 *   accumulator stepped by the frequency, the wave table's pointer, size
 *   (256 bytes to 32 KB) and resolution; modes free-run, one-shot, sync/AM
 *   and swap; a zero sample halts; a halt can raise the oscillator's
 *   interrupt (register $E0 names it). The highest enabled oscillator
 *   sounds three times over, as on the silicon.
 * - Timing: the DOC runs at 7.16 MHz (7 clocks a 1 MHz cycle), a sample
 *   every 8 x (oscillators + 2) clocks; the machine catches it up before
 *   each register access and after each instruction.
 * - The GLU: $C03C control (bit 6 RAM / registers, bit 5 auto-increment,
 *   bits 0-3 the volume), $C03D data (reads one access behind), $C03E/F the
 *   address.
 * - Output: mono at 44.1 kHz (each output sample averages the DOC samples
 *   in it), times the GLU's volume, plus the speaker.
 */
class GsSound : public IOHandler, public IAudioOutput {
public:
    static constexpr int SAMPLE_RATE = 44100;

    GsSound(Apple2Speaker* speaker, double slowHz);

    // IOHandler
    const char* name() const override { return "Sound"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override;
    void tick(uint64_t) override {}

    // IAudioOutput
    int nativeSampleRate() const override { return SAMPLE_RATE; }
    int pullSamples(float* buffer, int maxSamples) override;

    /** Run the DOC up to `slowCycle` (1 MHz cycles since power-on). */
    void advance(uint64_t slowCycle);
    /** The DOC's interrupt line. */
    bool irq() const { return m_irq; }

    // The GLU, at `now` (1 MHz cycles)
    uint8_t readCtl() const { return m_ctl; }
    void writeCtl(uint8_t v) { m_ctl = v & 0x7F; }
    uint8_t readPtr(int hi) const { return (uint8_t)(hi ? m_ptr >> 8 : m_ptr); }
    void writePtr(int hi, uint8_t v) { m_ptr = hi ? (uint16_t)((m_ptr & 0xFF) | v << 8) : (uint16_t)((m_ptr & 0xFF00) | v); }
    uint8_t readData(uint64_t now);
    void writeData(uint8_t v, uint64_t now);

    // The DOC's registers ($00-$FF) and RAM, for tests
    uint8_t docRead(uint8_t reg);
    void docWrite(uint8_t reg, uint8_t v);
    uint8_t* ram() { return m_ram.data(); }
    int oscillators() const { return m_numOsc; }

private:
    struct Osc {
        uint16_t freq = 0, wtsize = 0;
        uint8_t control = 1, vol = 0, data = 0x80;
        uint32_t wavetblpointer = 0;
        uint8_t wavetblsize = 0, resolution = 0;
        uint32_t accumulator = 0;
        bool irqpend = false;
    };
    enum { MODE_FREE = 0, MODE_ONESHOT = 1, MODE_SYNCAM = 2, MODE_SWAP = 3 };

    void haltOsc(int onum, int type, uint32_t* acc, int resshift, uint8_t newCtrl);
    int32_t docSample();
    void updateIrq();

    Apple2Speaker* m_speaker;
    std::vector<uint8_t> m_ram;
    Osc m_osc[32];
    int m_numOsc = 1;
    uint8_t m_rege0 = 0xFF;
    bool m_irq = false;
    // The GLU
    uint8_t m_ctl = 0, m_saved = 0;
    uint16_t m_ptr = 0;
    // Time, in DOC clocks
    const double m_docPerSlow;
    uint64_t m_lastSlow = 0;
    double m_docClock = 0, m_nextSample = 0, m_nextOut = 0;
    const double m_outPeriod;
    double m_acc = 0;
    int m_accCount = 0;
    float m_last = 0;
    std::deque<float> m_out;
};

}  // namespace apple2
