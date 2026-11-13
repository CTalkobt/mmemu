#include "../main/sid6581.h"
#include "test_harness.h"
#include <cmath>
#include <vector>

// Issues #120-#123: per-cycle SID core, measured filter curves, DC offsets
// (volume-register digis) and filter saturation.

namespace {
struct Sid {
    SID6581 sid{"SID", 0xD400};
    explicit Sid(int variant = 0) {
        sid.setClockHz(985248);
        sid.setSampleRate(44100);
        sid.setFilterVariant(variant);
        sid.reset();
        // The envelope counters power up at $AA and fall to 0 in release
        sid.tick(100000);
        std::vector<float> drain(8192);
        sid.pullSamples(drain.data(), (int)drain.size());
    }
    void w(int reg, uint8_t v) { sid.ioWrite(nullptr, 0xD400 + reg, v); }
    uint8_t r(int reg) { uint8_t v = 0; sid.ioRead(nullptr, 0xD400 + reg, &v); return v; }
    void voice(int n, uint16_t freq, uint8_t ad, uint8_t sr, uint8_t cr, uint16_t pw = 0x800) {
        w(n * 7 + 0, freq & 0xFF); w(n * 7 + 1, freq >> 8);
        w(n * 7 + 2, pw & 0xFF);   w(n * 7 + 3, pw >> 8);
        w(n * 7 + 5, ad); w(n * 7 + 6, sr); w(n * 7 + 4, cr);
    }
    std::vector<float> run(uint64_t cycles) {
        sid.tick(cycles);
        std::vector<float> out(8192);
        out.resize(sid.pullSamples(out.data(), (int)out.size()));
        return out;
    }
    /** Cycles until ENV3 (voice 3) reaches `level`, ticking one cycle at a time. */
    int cyclesUntilEnv3(uint8_t level, int limit) {
        for (int c = 1; c <= limit; ++c) { sid.tick(1); if (r(SID6581::ENV3) == level) return c; }
        return -1;
    }
};

float rms(const std::vector<float>& v, size_t from = 0) {
    double s = 0; size_t n = 0;
    for (size_t i = from; i < v.size(); ++i) { s += (double)v[i] * v[i]; ++n; }
    return n ? (float)std::sqrt(s / n) : 0.0f;
}
float peak(const std::vector<float>& v, size_t from = 0) {
    float p = 0;
    for (size_t i = from; i < v.size(); ++i) p = std::max(p, std::fabs(v[i]));
    return p;
}
}  // namespace

// #123: everything is clocked per cycle, so the result cannot depend on how
// the time is split into tick() calls (the old sample-based core shifted the
// noise LFSR at most once per tick, and stepped envelopes per tick).
TEST_CASE(sid_cycle_result_independent_of_tick_size) {
    Sid a, b;
    for (Sid* s : {&a, &b}) {
        s->voice(0, 0x1234, 0x22, 0xA4, 0x41, 0x600);   // pulse
        s->voice(1, 0x2345, 0x11, 0x88, 0x15);          // triangle, ring mod
        s->voice(2, 0xF000, 0x00, 0xF0, 0x81);          // fast noise
        s->w(SID6581::FC_HI, 0x40); s->w(SID6581::RES_FILT, 0xA3); s->w(SID6581::MODE_VOL, 0x1F);
    }
    a.sid.tick(200000);
    for (int i = 0; i < 200000 / 7; ++i) b.sid.tick(7);
    b.sid.tick(200000 % 7);
    ASSERT_EQ((int)a.r(SID6581::OSC3), (int)b.r(SID6581::OSC3));
    ASSERT_EQ((int)a.r(SID6581::ENV3), (int)b.r(SID6581::ENV3));
    std::vector<float> sa(8192), sb(8192);
    int na = a.sid.pullSamples(sa.data(), 8192), nb = b.sid.pullSamples(sb.data(), 8192);
    ASSERT_EQ(na, nb);
    for (int i = 0; i < na; ++i) ASSERT(std::fabs(sa[i] - sb[i]) < 1e-5f);
}

// #123: attack at A=0 steps every 9 cycles (reSID rate period), 255 steps.
TEST_CASE(sid_cycle_attack_rate_exact) {
    Sid s;
    s.voice(2, 0, 0x00, 0xF0, 0x01);    // gate, A=0
    int first = s.cyclesUntilEnv3(1, 100);
    // The rate counter is free-running (up to one period), the gate goes
    // through a 2-cycle pipeline and ENV3 is a cycle late (reSIDfp)
    ASSERT(first >= 1 && first <= 12);
    int full = s.cyclesUntilEnv3(0xFF, 5000);
    ASSERT_EQ(full, 254 * 9);
}

// #123: decay is exponential-ish: the step period doubles at levels $5D, $36...
TEST_CASE(sid_cycle_decay_exponential_steps) {
    Sid s;
    s.voice(2, 0, 0x00, 0x00, 0x01);    // A=0 D=0 S=0
    ASSERT(s.cyclesUntilEnv3(0xFF, 5000) > 0);
    int toA = s.cyclesUntilEnv3(0x5D, 100000);      // 255 -> $5D at 1 step per period
    ASSERT_EQ(toA, (0xFF - 0x5D) * 9);
    int toB = s.cyclesUntilEnv3(0x36, 100000);      // $5D -> $36 at 1 step per 2 periods
    ASSERT_EQ(toB, (0x5D - 0x36) * 2 * 9 + 1);       // the new period starts a cycle late
    int toC = s.cyclesUntilEnv3(0x1A, 100000);      // then 4 periods per step
    ASSERT_EQ(toC, (0x36 - 0x1A) * 4 * 9);
}

// #123: the rate counter is never reset; lowering the rate period below the
// counter makes it run on to the 15-bit wraparound ("ADSR delay bug").
TEST_CASE(sid_cycle_adsr_delay_bug) {
    Sid s;
    s.voice(2, 0, 0xF0, 0xF0, 0x01);    // A=$F: period 31251
    s.sid.tick(20000);                  // counter well past 9, no step yet
    ASSERT_EQ((int)s.r(SID6581::ENV3), 0);
    s.w(2 * 7 + 5, 0x00);               // A=0: period 9, but the counter is at ~20000
    int c = s.cyclesUntilEnv3(1, 40000);
    ASSERT(c > 12000 && c < 12800);     // waits for the wrap at 0x7FFF (~32768 - 20000)
}

// #120: the 6581 cutoff curve drops at FC $400 (measured: 6000 Hz at $3FF,
// 4600 Hz at $400); the 8580 rises steadily (~6500 Hz at both). A 3.85 kHz
// triangle through the high-pass: a lower cutoff passes more of it.
TEST_CASE(sid_filter_measured_cutoff_curve) {
    auto hpLevel = [](int variant, int fc) {
        Sid s(variant);
        s.voice(0, 0xFFFF, 0x00, 0xF0, 0x11);   // triangle at ~3.85 kHz (the top)
        s.w(SID6581::FC_LO, fc & 7); s.w(SID6581::FC_HI, (uint8_t)(fc >> 3));
        s.w(SID6581::RES_FILT, 0x01); s.w(SID6581::MODE_VOL, 0x4F);   // HP
        auto out = s.run(985248 / 5);
        return rms(out, out.size() / 2);
    };
    float r6581 = hpLevel(0, 0x400) / hpLevel(0, 0x3FF);
    ASSERT(r6581 > 1.25f);              // 4600 Hz vs 6000 Hz cutoff
    float r8580 = hpLevel(1, 0x400) / hpLevel(1, 0x3FF);
    ASSERT(r8580 > 0.95f && r8580 < 1.05f);
    ASSERT(hpLevel(0, 0x7FF) < hpLevel(0, 0x400) * 0.5f);   // 18 kHz: little passes
}

// #120: resonance boosts the band around the cutoff.
TEST_CASE(sid_filter_resonance_boosts_cutoff_band) {
    auto bpLevel = [](uint8_t res) {
        Sid s(1);
        s.voice(0, 0x6C00, 0x00, 0xF0, 0x21);   // saw at ~1.6 kHz
        s.w(SID6581::FC_HI, 0x20);              // 8580: ~1.6 kHz
        s.w(SID6581::RES_FILT, (uint8_t)(res << 4 | 0x01)); s.w(SID6581::MODE_VOL, 0x2F);   // BP
        auto out = s.run(985248 / 5);
        return rms(out, out.size() / 2);
    };
    ASSERT(bpLevel(15) > bpLevel(0) * 1.5f);
}

// #122: on the 6581 the master volume also scales the chip's DC offsets, so
// writing $D418 alone makes sound (sample playback in many tunes); the 8580
// has no DC offsets and stays silent.
TEST_CASE(sid_volume_register_digis) {
    auto digi = [](int variant) {
        Sid s(variant);
        s.sid.tick(985248 / 10);                        // settle
        s.run(0);
        std::vector<float> all;
        for (int i = 0; i < 400; ++i) {                 // 4 kHz square on $D418
            s.w(SID6581::MODE_VOL, (i & 1) ? 0x0F : 0x00);
            auto out = s.run(123);
            all.insert(all.end(), out.begin(), out.end());
        }
        return rms(all);
    };
    ASSERT(digi(0) > 0.05f);
    ASSERT(digi(1) < 1e-4f);
}

// #121: the 6581 filter integrators clip at the op-amps' output swing, so a
// loud resonant band-pass is compressed: three voices are clearly less than
// three times as loud as one. The 8580 has more headroom and stays linear.
TEST_CASE(sid_filter_saturates_at_opamp_headroom) {
    auto level = [](int variant, int voices) {
        Sid s(variant);
        for (int v = 0; v < voices; ++v) s.voice(v, 0x2000, 0x00, 0xF0, 0x21);   // saws in phase, ~480 Hz
        s.w(SID6581::FC_HI, 0x44);                       // cutoff near the fundamental
        s.w(SID6581::RES_FILT, 0xF7); s.w(SID6581::MODE_VOL, 0x2F);   // full resonance, BP
        auto out = s.run(985248 / 5);
        return peak(out, out.size() / 2);
    };
    float one = level(0, 1), three = level(0, 3);
    ASSERT(three > one * 1.5f && three < one * 2.6f);   // measured 0.32 -> 0.78 (linear: 0.97)
    float one8 = level(1, 1), three8 = level(1, 3);
    ASSERT(three8 > one8 * 2.9f);
}

// reSID 1.0: a write-only register reads the data bus, the last value written
// or read; it fades to 0 after $1D00 cycles on the 6581 (testprogs
// general/fuxxortest ef2-inst4b, SID/busvalue). Reading one halves what is
// left (reSIDfp).
TEST_CASE(sid_bus_value_fades) {
    SID6581 sid("SID", 0xD400);
    uint8_t v;
    sid.ioWrite(nullptr, 0xD404, 0x41);
    sid.ioPeek(nullptr, 0xD400, &v);
    ASSERT_EQ(v, (uint8_t)0x41);
    sid.tick(0x1D00 - 1);
    sid.ioPeek(nullptr, 0xD41F, &v);                     // unused: the bus too
    ASSERT_EQ(v, (uint8_t)0x41);
    sid.tick(1);
    sid.ioPeek(nullptr, 0xD400, &v);
    ASSERT_EQ(v, (uint8_t)0x00);

    sid.ioWrite(nullptr, 0xD404, 0x42);
    sid.ioRead(nullptr, 0xD41F, &v);                     // read: $E80 cycles left
    ASSERT_EQ(v, (uint8_t)0x42);
    sid.tick(0xE80 - 1);
    sid.ioPeek(nullptr, 0xD400, &v);
    ASSERT_EQ(v, (uint8_t)0x42);
    sid.tick(1);
    sid.ioPeek(nullptr, 0xD400, &v);
    ASSERT_EQ(v, (uint8_t)0x00);
}
