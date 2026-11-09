#pragma once

// Parts shared by the Apple II models (machine_apple2.cpp) and the IIgs
// (machine_apple2gs.cpp): the ROM loader, the speaker, the display device.

#include "libdevices/main/io_handler.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/iaudio_output.h"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace apple2 {

// The CPU clock: 14.318181 MHz / 14, with a long cycle every 65 (AppleWin
// CLK_6502_NTSC)
inline constexpr uint32_t CPU_HZ = 1020484;
inline constexpr int SAMPLE_RATE = 44100;

inline bool loadFile(const std::string& path, size_t size, std::vector<uint8_t>& out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (f) out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (!f || out.size() != size) {
        if (err) *err = "missing or wrong-size ROM " + path;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Speaker (Speaker.cpp): each access to $C030 flips the cone; the level is
// averaged over each sample period, then a DC-blocking filter (the cone
// rests at either position).
// ---------------------------------------------------------------------------
class Apple2Speaker : public IOHandler, public IAudioOutput {
public:
    const char* name() const override { return "Speaker"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override { m_out.clear(); }
    void tick(uint64_t) override {}

    void toggle(uint64_t at) { advance(at); m_level = -m_level; ++m_toggles; }
    void advance(uint64_t to) {
        while (m_time < to) {
            const double room = PER_SAMPLE - m_phase;
            const double step = std::min<double>(room, (double)(to - m_time));
            m_acc += step * m_level;
            m_phase += step;
            m_time += (uint64_t)std::max(1.0, step);
            if (m_phase >= PER_SAMPLE - 1e-9) {
                const float x = (float)(m_acc / PER_SAMPLE) * 0.3f;
                const float y = x - m_prevX + 0.995f * m_prevY;     // DC blocker
                m_prevX = x;
                m_prevY = y;
                m_out.push_back(y);
                if (m_out.size() > SAMPLE_RATE) m_out.pop_front();
                m_acc = 0;
                m_phase -= PER_SAMPLE;
            }
        }
    }
    uint64_t toggles() const { return m_toggles; }

    int nativeSampleRate() const override { return SAMPLE_RATE; }
    int pullSamples(float* buffer, int maxSamples) override {
        int n = 0;
        while (n < maxSamples && !m_out.empty()) { buffer[n++] = m_out.front(); m_out.pop_front(); }
        return n;
    }

private:
    static constexpr double PER_SAMPLE = (double)CPU_HZ / SAMPLE_RATE;
    uint64_t m_time = 0, m_toggles = 0;
    double m_phase = 0, m_acc = 0;
    float m_level = 1.0f, m_prevX = 0, m_prevY = 0;
    std::deque<float> m_out;
};

// The video, as the frontends find it (an I/O device with a picture)
class Apple2Display : public IOHandler, public IVideoOutput {
public:
    explicit Apple2Display(IVideoOutput* v) : m_video(v) {}
    const char* name() const override { return "Video"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    VideoDimensions getDimensions() const override { return m_video->getDimensions(); }
    void renderFrame(uint32_t* buffer) override { m_video->renderFrame(buffer); }
private:
    IVideoOutput* m_video;
};

}  // namespace apple2
