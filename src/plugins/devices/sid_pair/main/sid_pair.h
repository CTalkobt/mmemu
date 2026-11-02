#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/iaudio_inspect.h"
#include "plugins/devices/sid6581/main/sid6581.h"
#include <string>

/**
 * Dual SID wrapper for MEGA65.
 *
 * Instantiates two SID6581 objects:
 *   SID1 at $D400–$D41F (default)
 *   SID2 at $D420–$D43F
 *
 * Dispatches to SID1 or SID2 by bit 5 of the address offset.
 * Stereo mix: SID1 panned right-biased, SID2 panned left-biased.
 * Pan weights configurable (default 0.75/0.25 split).
 *
 * IAudioOutput produces interleaved stereo float samples.
 * IAudioInspect exposes the two SIDs as chips 0 and 1.
 */
class SidPair : public IOHandler, public IAudioOutput, public IAudioInspect {
public:
    SidPair();
    ~SidPair() override = default;

    void setName(const std::string& n) override { m_name = n; }
    void setBaseAddr(uint32_t addr) override;
    void setClockHz(uint32_t hz) override;
    void setSampleRate(int hz);
    void setPan(float sid1Right, float sid2Right);

    // IOHandler
    const char* name()     const override { return m_name.c_str(); }
    uint32_t    baseAddr() const override { return m_baseAddr; }
    uint32_t    addrMask() const override { return 0x003F; } // 64-byte window

    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t  val) override;
    void reset()  override;
    void tick(uint64_t cycles) override;
    void getDeviceInfo(DeviceInfo& out) const override;

    // IAudioOutput (stereo interleaved: L, R, L, R, ...)
    int nativeSampleRate() const override { return m_sid1.nativeSampleRate(); }
    int pullSamples(float* buffer, int maxSamples) override;

    // IAudioInspect (#30): chip 0 = SID1, chip 1 = SID2
    int  chipCount() const override { return 2; }
    std::string chipName(int chip) const override { return chip ? m_sid2.chipName(0) : m_sid1.chipName(0); }
    int  voiceCount(int chip) const override { return chip ? m_sid2.voiceCount(0) : m_sid1.voiceCount(0); }
    bool voiceInfo(int chip, int voice, VoiceInfo& out) const override {
        if (chip < 0 || chip > 1) return false;
        return (chip ? m_sid2 : m_sid1).voiceInfo(0, voice, out);
    }
    int  scopeSampleRate() const override { return m_sid1.scopeSampleRate(); }
    int  scopeCapacity() const override { return m_sid1.scopeCapacity(); }
    int  scopeSamples(int chip, int channel, float* out, int n) const override {
        if (chip < 0 || chip > 1) return 0;
        return (chip ? m_sid2 : m_sid1).scopeSamples(0, channel, out, n);
    }
    uint64_t scopeTotal(int chip) const override { return chip ? m_sid2.scopeTotal(0) : m_sid1.scopeTotal(0); }

private:
    std::string m_name{"Dual SID"};
    uint32_t    m_baseAddr = 0xD400;

    SID6581 m_sid1;
    SID6581 m_sid2;

    // Pan weights: sid1Left + sid1Right = 1.0, sid2Left + sid2Right = 1.0
    float m_sid1Right = 0.75f; // SID1 biased right
    float m_sid1Left  = 0.25f;
    float m_sid2Right = 0.25f; // SID2 biased left
    float m_sid2Left  = 0.75f;
};
