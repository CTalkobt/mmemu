#include "test_harness.h"
#include "plugins/devices/sid6581/main/sid6581.h"
#include "plugins/devices/sid_pair/main/sid_pair.h"
#include "libdebug/main/audio_scope.h"
#include <cmath>
#include <vector>

// IAudioInspect (#30): voice state and scope history for the SID Scope pane.

namespace {

constexpr uint32_t BASE = 0xD400;

void w(IOHandler& h, uint32_t reg, uint8_t v) { h.ioWrite(nullptr, BASE + reg, v); }

// Run like a CPU does: many short ticks.
void run(IOHandler& h, uint64_t cycles) {
    for (uint64_t done = 0; done < cycles; done += 20) h.tick(20);
}

} // namespace

TEST_CASE(sid_inspect_voice_info) {
    SID6581 sid("SID", BASE);
    sid.setClockHz(985248);
    sid.setSampleRate(44100);
    IAudioInspect* insp = &sid;

    ASSERT(insp->chipCount() == 1);
    ASSERT(insp->chipName(0) == "SID $D400");
    ASSERT(insp->voiceCount(0) == 3);

    // Voice 1: 440 Hz pulse, PW 50%, A=0 D=9 S=10 R=5, gate on
    w(sid, SID6581::V1_FREQ_LO, 0x45); w(sid, SID6581::V1_FREQ_HI, 0x1D);   // $1D45
    w(sid, SID6581::V1_PW_LO, 0x00);   w(sid, SID6581::V1_PW_HI, 0x08);
    w(sid, SID6581::V1_AD, 0x09);      w(sid, SID6581::V1_SR, 0xA5);
    w(sid, SID6581::V1_CR, SID6581::CR_PULSE | SID6581::CR_GATE);
    w(sid, SID6581::RES_FILT, 0x02);     // voice 2 through the filter
    w(sid, SID6581::MODE_VOL, 0x8F);     // voice 3 off, volume 15

    IAudioInspect::VoiceInfo vi;
    ASSERT(insp->voiceInfo(0, 0, vi));
    ASSERT(vi.freq == 0x1D45);
    ASSERT(std::fabs(vi.freqHz - 440.0) < 0.1);
    ASSERT(vi.pulseWidth == 0x800);
    ASSERT(vi.control == (SID6581::CR_PULSE | SID6581::CR_GATE));
    ASSERT(vi.attack == 0 && vi.decay == 9 && vi.sustain == 10 && vi.release == 5);
    ASSERT(!vi.filtered && !vi.muted);

    run(sid, 5000);   // attack 0 (2 ms) is over after ~5 ms
    ASSERT(insp->voiceInfo(0, 0, vi));
    ASSERT(vi.envPhase == IAudioInspect::EnvPhase::Decay);
    ASSERT(vi.envLevel > 0xA0);

    ASSERT(insp->voiceInfo(0, 1, vi) && vi.filtered && vi.envPhase == IAudioInspect::EnvPhase::Idle);
    ASSERT(insp->voiceInfo(0, 2, vi) && vi.muted);
    ASSERT(!insp->voiceInfo(0, 3, vi));
    ASSERT(!insp->voiceInfo(1, 0, vi));
}

TEST_CASE(sid_inspect_scope_records_voices_and_output) {
    SID6581 sid("SID", BASE);
    sid.setClockHz(985248);
    sid.setSampleRate(44100);
    IAudioInspect* insp = &sid;

    std::vector<float> buf(insp->scopeCapacity());
    ASSERT(insp->scopeSamples(0, 0, buf.data(), 100) == 0);   // nothing recorded yet

    // Voice 1: ~1 kHz sawtooth, instant attack, full sustain
    w(sid, SID6581::V1_FREQ_LO, 0x00); w(sid, SID6581::V1_FREQ_HI, 0x43);
    w(sid, SID6581::V1_AD, 0x00);      w(sid, SID6581::V1_SR, 0xF0);
    w(sid, SID6581::V1_CR, SID6581::CR_SAW | SID6581::CR_GATE);
    w(sid, SID6581::MODE_VOL, 0x0F);

    run(sid, 22340);   // ~1000 samples at 44.1 kHz
    int n = insp->scopeSamples(0, 0, buf.data(), 2000);
    ASSERT(n >= 995 && n <= 1001);

    // A sawtooth: spans most of [-1, 1], rises between a few sharp drops
    float lo = 1, hi = -1;
    int rises = 0, drops = 0;
    for (int i = 500; i < n; ++i) {   // after the attack
        lo = std::min(lo, buf[i]); hi = std::max(hi, buf[i]);
        if (buf[i] > buf[i - 1]) ++rises;
        else if (buf[i] < buf[i - 1] - 0.5f) ++drops;
    }
    ASSERT(lo < -0.8f && hi > 0.8f);
    ASSERT(drops >= 8 && drops <= 14);        // ~1 kHz over ~11 ms
    ASSERT(rises > (n - 500) * 8 / 10);

    // Silent voice 2 is flat; the output carries voice 1
    ASSERT(insp->scopeSamples(0, 1, buf.data(), 500) == 500);
    for (int i = 0; i < 500; ++i) ASSERT(buf[i] == 0.0f);
    ASSERT(insp->scopeSamples(0, 3, buf.data(), 500) == 500);
    float peak = 0;
    for (int i = 0; i < 500; ++i) peak = std::max(peak, std::fabs(buf[i]));
    ASSERT(peak > 0.2f);

    ASSERT(insp->scopeSamples(0, 4, buf.data(), 10) == 0);   // no such channel
}

TEST_CASE(sid_inspect_scope_voice3_off_and_wrap) {
    SID6581 sid("SID", BASE);
    sid.setClockHz(985248);
    sid.setSampleRate(44100);
    IAudioInspect* insp = &sid;

    // Only voice 3 plays, disconnected from the output ($D418 bit 7)
    w(sid, SID6581::V3_FREQ_LO, 0x00); w(sid, SID6581::V3_FREQ_HI, 0x20);
    w(sid, SID6581::V3_AD, 0x00);      w(sid, SID6581::V3_SR, 0xF0);
    w(sid, SID6581::V3_CR, SID6581::CR_TRI | SID6581::CR_GATE);
    w(sid, SID6581::MODE_VOL, 0x8F);

    run(sid, 200000);   // ~9000 samples: the history wraps
    int cap = insp->scopeCapacity();
    std::vector<float> all(cap + 100), last(1);
    ASSERT(insp->scopeSamples(0, 2, all.data(), cap + 100) == cap);
    ASSERT(insp->scopeSamples(0, 2, last.data(), 1) == 1);
    ASSERT(last[0] == all[cap - 1]);   // oldest first, newest last

    float peak = 0;
    for (int i = 0; i < cap; ++i) peak = std::max(peak, std::fabs(all[i]));
    ASSERT(peak > 0.8f);              // the scope shows voice 3...
    ASSERT(insp->scopeSamples(0, 3, all.data(), cap) == cap);
    for (int i = 0; i < cap; ++i) ASSERT(all[i] == 0.0f);   // ...but it is not heard

    sid.reset();
    ASSERT(insp->scopeSamples(0, 2, all.data(), 10) == 0);
}

TEST_CASE(sid_pair_inspect_two_chips) {
    SidPair pair;
    IAudioInspect* insp = &pair;
    ASSERT(insp->chipCount() == 2);
    ASSERT(insp->chipName(0) == "SID1 $D400");
    ASSERT(insp->chipName(1) == "SID2 $D420");
    ASSERT(insp->voiceCount(1) == 3);

    pair.ioWrite(nullptr, 0xD420 + SID6581::V2_FREQ_LO, 0x34);   // SID2 voice 2
    pair.ioWrite(nullptr, 0xD420 + SID6581::V2_FREQ_HI, 0x12);
    IAudioInspect::VoiceInfo vi;
    ASSERT(insp->voiceInfo(1, 1, vi) && vi.freq == 0x1234);
    ASSERT(insp->voiceInfo(0, 1, vi) && vi.freq == 0);
    ASSERT(!insp->voiceInfo(2, 0, vi));
}

// --- Scope helpers (libdebug/main/audio_scope) ---

TEST_CASE(scope_note_name) {
    int cents = 99;
    ASSERT(scope::noteName(440.0, &cents) == "A4" && cents == 0);
    ASSERT(scope::noteName(261.63) == "C4");
    ASSERT(scope::noteName(277.18) == "C#4");
    ASSERT(scope::noteName(32.70) == "C1");
    ASSERT(scope::noteName(452.0, &cents) == "A4" && cents == 47);
    ASSERT(scope::noteName(454.0, &cents) == "A#4" && cents == -46);
    ASSERT(scope::noteName(4.0, &cents).empty() && cents == 0);
    ASSERT(scope::noteName(0.0).empty());
}

TEST_CASE(scope_control_text) {
    ASSERT(scope::controlText(0x00) == "-");
    ASSERT(scope::controlText(0x41) == "Pulse Gate");
    ASSERT(scope::controlText(0x15) == "Tri Ring Gate");
    ASSERT(scope::controlText(0x62) == "Saw+Pulse Sync");
    ASSERT(scope::controlText(0x88) == "Noise Test");
}

TEST_CASE(scope_find_trigger) {
    // A 100-sample-period sine over 1000 samples
    std::vector<float> s(1000);
    for (int i = 0; i < 1000; ++i) s[i] = (float)std::sin(2 * 3.14159265 * (i + 30) / 100.0);
    // Rising crossings are at i = 70, 170, ... 970. With a 250-sample window
    // the latest start that fits is 750; the latest crossing at or before it is 670.
    int t = scope::findTrigger(s.data(), 1000, 250);
    ASSERT(t == 670 || t == 671);
    ASSERT(s[t - 1] < 0 && s[t] >= 0);

    // Too short for the window, flat, or only noise near zero: no trigger
    ASSERT(scope::findTrigger(s.data(), 200, 250) == -1);
    std::vector<float> flat(1000, 0.0f);
    ASSERT(scope::findTrigger(flat.data(), 1000, 250) == -1);
    std::vector<float> hum(1000);
    for (int i = 0; i < 1000; ++i) hum[i] = (i & 1) ? 0.005f : -0.005f;
    ASSERT(scope::findTrigger(hum.data(), 1000, 250) == -1);
}

TEST_CASE(scope_envelope_times) {
    ASSERT(scope::attackMs(0) == 2 && scope::attackMs(15) == 8000);
    ASSERT(scope::decayReleaseMs(0) == 6 && scope::decayReleaseMs(9) == 750);
}
