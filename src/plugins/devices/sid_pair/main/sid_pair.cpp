#include "sid_pair.h"
#include <algorithm>
#include <cstring>

SidPair::SidPair()
    : m_sid1("SID1", 0xD400)
    , m_sid2("SID2", 0xD420)
{
}

void SidPair::setBaseAddr(uint32_t addr) {
    m_baseAddr = addr;
    m_sid1.setBaseAddr(addr);
    m_sid2.setBaseAddr(addr + 0x20);
}

void SidPair::setClockHz(uint32_t hz) {
    m_sid1.setClockHz(hz);
    m_sid2.setClockHz(hz);
}

void SidPair::setSampleRate(int hz) {
    m_sid1.setSampleRate(hz);
    m_sid2.setSampleRate(hz);
}

void SidPair::setPan(float sid1Right, float sid2Right) {
    m_sid1Right = sid1Right;
    m_sid1Left  = 1.0f - sid1Right;
    m_sid2Right = sid2Right;
    m_sid2Left  = 1.0f - sid2Right;
}

bool SidPair::ioRead(IBus* bus, uint32_t addr, uint8_t* val) {
    uint32_t offset = addr - m_baseAddr;
    if (offset >= 64) return false;
    if (offset < 32)
        return m_sid1.ioRead(bus, addr, val);
    else
        return m_sid2.ioRead(bus, addr, val);
}

bool SidPair::ioPeek(IBus* bus, uint32_t addr, uint8_t* val) {
    uint32_t offset = addr - m_baseAddr;
    if (offset >= 64) return false;
    return offset < 32 ? m_sid1.ioPeek(bus, addr, val) : m_sid2.ioPeek(bus, addr, val);
}

bool SidPair::ioWrite(IBus* bus, uint32_t addr, uint8_t val) {
    uint32_t offset = addr - m_baseAddr;
    if (offset >= 64) return false;
    if (offset < 32)
        return m_sid1.ioWrite(bus, addr, val);
    else
        return m_sid2.ioWrite(bus, addr, val);
}

void SidPair::reset() {
    m_sid1.reset();
    m_sid2.reset();
}

void SidPair::tick(uint64_t cycles) {
    m_sid1.tick(cycles);
    m_sid2.tick(cycles);
}

int SidPair::pullSamples(float* buffer, int maxSamples) {
    // Each SID produces mono samples. We pull from both, then mix to stereo.
    // maxSamples is the number of STEREO frames (L+R pairs) the caller wants.
    // We return interleaved L, R, L, R, ...
    int maxFrames = maxSamples / 2;
    if (maxFrames <= 0) return 0;

    float mono1[512];
    float mono2[512];

    int totalFrames = 0;
    float* out = buffer;

    while (totalFrames < maxFrames) {
        int batch = std::min(maxFrames - totalFrames, 512);
        int n1 = m_sid1.pullSamples(mono1, batch);
        int n2 = m_sid2.pullSamples(mono2, batch);
        int n = std::min(n1, n2);
        if (n <= 0) break;

        for (int i = 0; i < n; i++) {
            *out++ = mono1[i] * m_sid1Left  + mono2[i] * m_sid2Left;  // L
            *out++ = mono1[i] * m_sid1Right + mono2[i] * m_sid2Right; // R
        }
        totalFrames += n;
    }

    return totalFrames * 2; // return total float samples written
}

void SidPair::getDeviceInfo(DeviceInfo& out) const {
    out.name = name();
    out.baseAddr = m_baseAddr;
    out.addrMask = addrMask();

    out.registers.push_back({"SID1 base", 0, (uint8_t)(m_baseAddr & 0xFF), ""});
    out.registers.push_back({"SID2 base", 0x20, (uint8_t)((m_baseAddr + 0x20) & 0xFF), ""});
    out.registers.push_back({"Pan SID1 R%", 0, (uint8_t)(m_sid1Right * 100), ""});
    out.registers.push_back({"Pan SID2 R%", 0, (uint8_t)(m_sid2Right * 100), ""});

    DeviceInfo sid1Info, sid2Info;
    m_sid1.getDeviceInfo(sid1Info);
    m_sid2.getDeviceInfo(sid2Info);
    for (auto& r : sid1Info.registers)
        out.registers.push_back({"SID1." + r.name, r.offset, r.value, r.description});
    for (auto& r : sid2Info.registers)
        out.registers.push_back({"SID2." + r.name, r.offset, r.value, r.description});
}
