#include "ym2151.h"
#include "libdevices/main/isignal_line.h"
#include "libdevices/main/device_info.h"
#include "ymfm_opm.h"
#include <algorithm>
#include <cstdio>

// x16-emulator src/ymglue.cpp's ym2151_interface
class Ym2151::Chip : public ymfm::ymfm_interface {
public:
    Chip() : m_chip(*this) { m_chip.reset(); }

    void ymfm_sync_mode_write(uint8_t data) override { m_engine->engine_mode_write(data); }
    void ymfm_sync_check_interrupts() override { m_engine->engine_check_interrupts(); }
    void ymfm_set_timer(uint32_t tnum, int32_t duration) override { if (tnum < 2) m_timers[tnum] = duration; }
    void ymfm_set_busy_end(uint32_t clocks) override { m_busy = (int32_t)clocks; }
    bool ymfm_is_busy() override { return m_busy > 0; }
    void ymfm_update_irq(bool asserted) override { m_irq = asserted; }

    void reset() {
        m_chip.reset();
        m_timers[0] = m_timers[1] = 0;
        m_busy = 0;
        m_irq = false;
    }
    // One sample: the busy flag and timers count 64 chip clocks
    void generate(int16_t& l, int16_t& r) {
        m_busy = std::max(0, m_busy - 64);
        for (int i = 0; i < 2; ++i) {
            if (m_timers[i] > 0) {
                m_timers[i] = std::max(0, m_timers[i] - 64);
                if (m_timers[i] <= 0) m_engine->engine_timer_expired(i);
            }
        }
        m_chip.generate(&m_out);
        l = (int16_t)std::clamp(m_out.data[0], -32768, 32767);
        r = (int16_t)std::clamp(m_out.data[1], -32768, 32767);
    }
    // A write while busy is lost, as on the chip (x16-emulator prints a warning)
    bool write(uint8_t addr, uint8_t value) {
        if (ymfm_is_busy()) return false;
        m_chip.write_address(addr);
        m_chip.write_data(value);
        return true;
    }
    uint8_t status() { return m_chip.read_status(); }
    bool irq() const { return m_irq; }

private:
    ymfm::ym2151 m_chip;
    ymfm::ym2151::output_data m_out;
    int32_t m_timers[2] = {0, 0};
    int32_t m_busy = 0;
    bool m_irq = false;
};

Ym2151::Ym2151(uint32_t base, uint32_t mask)
    : m_base(base), m_mask(mask), m_chip(std::make_unique<Chip>()), m_buf(BUF * 2, 0) {}

Ym2151::~Ym2151() = default;

void Ym2151::reset() {
    m_chip->reset();
    m_addr = 0;
    m_sampleAcc = 0;
    m_write = 0;
    m_readPos = 0.0;
    std::fill(m_buf.begin(), m_buf.end(), 0);
    updateIrq();
}

bool Ym2151::ioPeek(IBus*, uint32_t addr, uint8_t* val) {
    if ((addr & ~m_mask) != m_base) return false;
    *val = (addr & 1) ? m_chip->status() : 0x9F;   // even: open bus (x16-emulator)
    return true;
}

bool Ym2151::ioRead(IBus* bus, uint32_t addr, uint8_t* val) { return ioPeek(bus, addr, val); }

bool Ym2151::ioWrite(IBus*, uint32_t addr, uint8_t val) {
    if ((addr & ~m_mask) != m_base) return false;
    if (!(addr & 1)) m_addr = val;                  // partial decoding: even = address
    else { m_chip->write(m_addr, val); updateIrq(); }
    return true;
}

uint8_t Ym2151::status() const { return m_chip->status(); }
bool Ym2151::irqAsserted() const { return m_chip->irq(); }

void Ym2151::updateIrq() {
    bool irq = m_chip->irq();
    if (irq != m_lastIrq) {
        m_lastIrq = irq;
        if (m_irqLine) m_irqLine->set(irq);
    }
}

void Ym2151::generateOne() {
    int16_t l, r;
    m_chip->generate(l, r);
    uint32_t i = (m_write % BUF) * 2;
    m_buf[i] = l;
    m_buf[i + 1] = r;
    ++m_write;
}

void Ym2151::tick(uint64_t cpuCycles) {
    m_sampleAcc += cpuCycles * SAMPLE_RATE;
    while (m_sampleAcc >= m_cpuHz) {
        m_sampleAcc -= m_cpuHz;
        generateOne();
    }
    updateIrq();
}

void Ym2151::nextSample(uint32_t outRate, int32_t& left, int32_t& right) {
    // Keep the read position behind the newest sample; if the reader has
    // fallen far behind (no reader for a while), catch up
    double newest = (double)m_write - 1.0;
    if (newest < 0) { left = right = 0; return; }
    if (m_readPos > newest) m_readPos = newest;
    if (newest - m_readPos > BUF / 2) m_readPos = newest - BUF / 4;
    uint32_t i0 = (uint32_t)m_readPos;
    double f = m_readPos - i0;
    uint32_t i1 = std::min<uint32_t>(i0 + 1, (uint32_t)newest);
    const int16_t* a = &m_buf[(i0 % BUF) * 2];
    const int16_t* b = &m_buf[(i1 % BUF) * 2];
    left  = (int32_t)(a[0] + (b[0] - a[0]) * f);
    right = (int32_t)(a[1] + (b[1] - a[1]) * f);
    m_readPos += (double)SAMPLE_RATE / outRate;
}

void Ym2151::getDeviceInfo(DeviceInfo& out) const {
    out.name = "YM2151";
    out.baseAddr = m_base;
    out.addrMask = m_mask;
    char buf[16];
    snprintf(buf, sizeof buf, "$%02X", m_addr);
    out.state.push_back({"Address register", buf});
    snprintf(buf, sizeof buf, "$%02X", m_chip->status());
    out.state.push_back({"Status", buf});
    out.state.push_back({"IRQ", m_chip->irq() ? "asserted" : "clear"});
    out.state.push_back({"Samples generated", std::to_string(m_write)});
}
