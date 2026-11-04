#pragma once

// Yamaha YM2151 (OPM), as on the Commander X16, using Aaron Giles's ymfm
// (src/third_party/ymfm, BSD-3-Clause). The glue follows x16-emulator's
// src/ymglue.cpp (2-clause BSD): timers and the busy flag count 64 chip
// clocks per sample; a write while busy is dropped.

#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <memory>
#include <vector>

class ISignalLine;

class Ym2151 : public IOHandler {
public:
    static constexpr uint32_t CHIP_CLOCK_HZ = 3579545;
    static constexpr uint32_t SAMPLE_RATE = CHIP_CLOCK_HZ / 64;     // 55,930 Hz

    /** At `base` (address register even, data/status odd, mirrored over `mask`). */
    explicit Ym2151(uint32_t base = 0x9F40, uint32_t mask = 0x1F);
    ~Ym2151() override;

    const char* name() const override { return "YM2151"; }
    uint32_t baseAddr() const override { return m_base; }
    uint32_t addrMask() const override { return m_mask; }
    void setIrqLine(ISignalLine* line) override { m_irqLine = line; }
    void setClockHz(uint32_t cpuHz) override { m_cpuHz = cpuHz ? cpuHz : 8000000; }

    bool ioRead(IBus*, uint32_t addr, uint8_t* val) override;
    bool ioPeek(IBus*, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus*, uint32_t addr, uint8_t val) override;
    void reset() override;
    /** CPU cycles: generates the samples that fall in them. */
    void tick(uint64_t cpuCycles) override;
    void getDeviceInfo(DeviceInfo& out) const override;

    /**
     * The next sample at `outRate` (stereo, int16 scale), linearly
     * interpolated from the chip's own rate; called once per output sample
     * by the mixer (the X16's VERA).
     */
    void nextSample(uint32_t outRate, int32_t& left, int32_t& right);

    bool irqAsserted() const;
    uint8_t status() const;

private:
    class Chip;                         // the ymfm interface and chip
    void generateOne();
    void updateIrq();

    uint32_t m_base, m_mask;
    uint32_t m_cpuHz = 8000000;
    ISignalLine* m_irqLine = nullptr;
    bool m_lastIrq = false;
    uint8_t m_addr = 0;
    std::unique_ptr<Chip> m_chip;

    uint64_t m_sampleAcc = 0;           // CPU cycles * SAMPLE_RATE, against m_cpuHz
    static constexpr int BUF = 4096;    // stereo frames
    std::vector<int16_t> m_buf;
    uint32_t m_write = 0;               // frames generated
    double   m_readPos = 0.0;           // in frames, fractional
};
