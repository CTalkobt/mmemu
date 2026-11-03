#include "test_harness.h"
#include "plugins/devices/pokey/main/pokey.h"
#include <cmath>
#include <vector>

// POKEY (#10): timers and IRQs, keyboard, serial output, audio.

namespace {
class MockSignal : public ISignalLine {
public:
    bool get() const override { return m_level; }
    void set(bool level) override { m_level = level; }
    void pulse() override {}
    bool m_level = false;
};

struct Pokey {
    POKEY p;
    MockSignal irq;
    Pokey() { p.setIrqLine(&irq); p.reset(); w(POKEY::SKCTL, 0x03); }
    void w(int r, uint8_t v) { p.ioWrite(nullptr, 0xD200 + r, v); }
    uint8_t r(int reg) { uint8_t v = 0; p.ioRead(nullptr, 0xD200 + reg, &v); return v; }
};
}  // namespace

TEST_CASE(pokey_timer1_irq_at_64khz) {
    Pokey k;
    k.w(POKEY::AUDF1, 9);              // 10 ticks of the 64 kHz clock (28 cycles each)
    k.w(POKEY::IRQEN, POKEY::IRQ_TIMER1);
    k.w(POKEY::STIMER, 0);
    k.p.tick(10 * 28 - 30);
    ASSERT(!k.irq.m_level);
    ASSERT_EQ((int)(k.r(POKEY::IRQST) & 1), 1);
    k.p.tick(40);
    ASSERT(k.irq.m_level);
    ASSERT_EQ((int)(k.r(POKEY::IRQST) & 1), 0);   // active low
    k.w(POKEY::IRQEN, 0);                          // clearing IRQEN clears the IRQ
    ASSERT(!k.irq.m_level);
    ASSERT_EQ((int)(k.r(POKEY::IRQST) & 1), 1);
}

TEST_CASE(pokey_timer_at_179mhz_16bit) {
    Pokey k;
    k.w(POKEY::AUDCTL, POKEY::CTL_CLK1_17 | POKEY::CTL_CH12_16);
    k.w(POKEY::AUDF1, 0x00); k.w(POKEY::AUDF2, 0x01);   // 256 + 7 cycles
    k.w(POKEY::IRQEN, POKEY::IRQ_TIMER2);
    k.w(POKEY::STIMER, 0);
    k.p.tick(262);
    ASSERT(!k.irq.m_level);
    k.p.tick(1);
    ASSERT(k.irq.m_level);
}

TEST_CASE(pokey_keyboard) {
    Pokey k;
    k.w(POKEY::IRQEN, POKEY::IRQ_KEY);
    k.p.keyDown(0x3F | 0x40);                      // shift+A
    ASSERT_EQ((int)k.r(POKEY::KBCODE), 0x7F);
    ASSERT(k.irq.m_level);
    ASSERT_EQ((int)(k.r(POKEY::SKSTAT) & 0x04), 0);   // key down
    k.p.keyUp();
    ASSERT_EQ((int)(k.r(POKEY::SKSTAT) & 0x04), 0x04);
    k.p.setShift(true);
    ASSERT_EQ((int)(k.r(POKEY::SKSTAT) & 0x08), 0);
    k.w(POKEY::IRQEN, POKEY::IRQ_BREAK);
    ASSERT(!k.irq.m_level);
    k.p.breakKey();
    ASSERT_EQ((int)(k.r(POKEY::IRQST) & 0x80), 0);

    Pokey off;
    off.w(POKEY::SKCTL, 0x00);                     // keyboard scanning off
    off.p.keyDown(0x3F);
    ASSERT_EQ((int)off.r(POKEY::KBCODE), 0xFF);
}

TEST_CASE(pokey_serial_output) {
    Pokey k;
    std::vector<uint8_t> sent;
    k.p.setSerialOut([&](uint8_t b) { sent.push_back(b); });
    // 19200 baud as the OS sets it: channels 3+4 joined at 1.79 MHz, $0028
    k.w(POKEY::AUDCTL, POKEY::CTL_CLK3_17 | POKEY::CTL_CH34_16);
    k.w(POKEY::AUDF3, 0x28); k.w(POKEY::AUDF4, 0x00);
    k.w(POKEY::IRQEN, POKEY::IRQ_SEROUT | POKEY::IRQ_SERDONE);
    ASSERT(k.irq.m_level);                         // idle: "transmission done"
    k.w(POKEY::STIMER, 0);                         // sync the bit clock for the timing below
    k.w(POKEY::SEROUT, 0x31);
    ASSERT_EQ((int)(k.r(POKEY::IRQST) & POKEY::IRQ_SEROUT), 0);   // buffer free at once
    k.w(POKEY::IRQEN, POKEY::IRQ_SERDONE);         // acknowledge
    k.w(POKEY::SEROUT, 0x52);                      // into the holding buffer
    const int bit = 2 * (0x28 + 7);                // two channel 4 underflows a bit
    k.p.tick(10 * bit - 1);
    ASSERT(sent.empty());
    k.p.tick(2);
    ASSERT_EQ((int)sent.size(), 1);
    ASSERT_EQ((int)sent[0], 0x31);
    ASSERT(!k.irq.m_level);                        // the second byte is still going
    k.p.tick(10 * bit);
    ASSERT_EQ((int)sent.size(), 2);
    ASSERT_EQ((int)sent[1], 0x52);
    ASSERT(k.irq.m_level);                         // done
}

TEST_CASE(pokey_audio_pure_tone) {
    Pokey k;
    k.p.setSampleRate(44100);
    k.w(POKEY::AUDF1, 0x50);                       // ~ 64 kHz / 81 / 2 = 394 Hz
    k.w(POKEY::AUDC1, 0xAF);                       // pure tone, volume 15
    k.p.tick(1789773 / 10);
    std::vector<float> s(8192);
    int n = k.p.pullSamples(s.data(), (int)s.size());
    ASSERT(n > 4000);
    double rms = 0;
    int crossings = 0;
    for (int i = n / 2; i < n; ++i) {
        rms += s[i] * s[i];
        if (i > n / 2 && (s[i] > 0) != (s[i - 1] > 0)) ++crossings;
    }
    rms = std::sqrt(rms / (n - n / 2));
    ASSERT(rms > 0.1);
    double hz = crossings / 2.0 / ((n - n / 2) / 44100.0);
    ASSERT(hz > 360 && hz < 430);

    Pokey quiet;
    quiet.w(POKEY::AUDF1, 0x50);
    quiet.w(POKEY::AUDC1, 0xA0);                   // volume 0
    quiet.p.tick(1789773 / 20);
    n = quiet.p.pullSamples(s.data(), (int)s.size());
    float peak = 0;
    for (int i = 0; i < n; ++i) peak = std::max(peak, std::fabs(s[i]));
    ASSERT(peak < 1e-6f);
}

TEST_CASE(pokey_random_runs) {
    Pokey k;
    uint8_t a = k.r(POKEY::RANDOM);
    k.p.tick(13);
    uint8_t b = k.r(POKEY::RANDOM);
    ASSERT(a != b);
    Pokey reset;
    reset.w(POKEY::SKCTL, 0);
    reset.p.tick(13);
    ASSERT_EQ((int)reset.r(POKEY::RANDOM), 0xFF);
}
