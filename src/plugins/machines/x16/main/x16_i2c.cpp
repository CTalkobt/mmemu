// Ported from x16-emulator (src/i2c.c, smc.c, rtc.c, joystick.c),
// Copyright (c) 2021 Michael Steil and contributors, 2-clause BSD license.

#include "x16_i2c.h"
#include <ctime>

namespace x16 {

static constexpr uint8_t DEVICE_SMC = 0x42;
static constexpr uint8_t DEVICE_RTC = 0x6F;
static constexpr uint8_t SMC_VERSION_MAJOR = 47, SMC_VERSION_MINOR = 0, SMC_VERSION_PATCH = 0;   // x16-emulator smc.h

// ---------------------------------------------------------------------------
// SMC
// ---------------------------------------------------------------------------

void Smc::reset() {
    m_kbdHead = m_kbdTail = 0;
    m_pos = 0;
    m_defaultReadOp = 0x41;
    m_defaultReadState = 0;
    m_data[0] = m_defaultReadOp;
}

void Smc::keyAdd(uint8_t code) {
    uint8_t next = (uint8_t)((m_kbdHead + 1) & (KBD_SIZE - 1));
    if (next == m_kbdTail) return;              // full: dropped
    m_kbd[m_kbdHead] = code;
    m_kbdHead = next;
}

uint8_t Smc::kbdNext() {
    if (m_kbdHead == m_kbdTail) return 0;
    uint8_t v = m_kbd[m_kbdTail];
    m_kbdTail = (uint8_t)((m_kbdTail + 1) & (KBD_SIZE - 1));
    return v;
}

void Smc::data(uint8_t v) {
    if (m_pos < sizeof(m_data)) m_data[m_pos++] = v;
}

// No mouse yet: the mouse paths answer "no packet" (a single 0)
uint8_t Smc::read() {
    uint8_t ret;
    switch (m_data[0]) {
        case 0x43:                              // keycode, then mouse
            if (m_defaultReadState == 0) {
                m_defaultReadState = 1;
                ret = kbdNext();
                break;
            }
            [[fallthrough]];
        case 0x42:                              // mouse only
        case 0x21:
            m_defaultReadState = 0;
            ret = 0x00;
            break;
        case 0x41:                              // keycode only
            m_defaultReadState = 0;
            [[fallthrough]];
        case 0x07:
            ret = kbdNext();
            break;
        case 0x09: ret = 0; break;              // power-on by long press: no
        case 0x22: ret = 0; break;              // mouse device id
        case 0x30: ret = SMC_VERSION_MAJOR; break;
        case 0x31: ret = SMC_VERSION_MINOR; break;
        case 0x32: ret = SMC_VERSION_PATCH; break;
        default:   ret = 0xFF; break;
    }
    m_data[0] = m_defaultReadOp;
    m_pos = 0;
    return ret;
}

void Smc::write() {
    switch (m_data[0]) {
        case 0x01:
            if (m_data[1] == 0 && onPowerOff) onPowerOff();
            else if (m_data[1] == 1 && onReset) onReset();
            break;
        case 0x02: if (m_data[1] == 0 && onReset) onReset(); break;
        case 0x03: if (m_data[1] == 0 && onNmi) onNmi(); break;
        case 0x05: m_activityLed = m_data[1] >= 128 ? 255 : 0; break;
        case 0x40: m_defaultReadOp = m_data[1]; m_defaultReadState = 0; break;
        default: break;
    }
    m_data[0] = m_defaultReadOp;
    m_pos = 0;
}

// ---------------------------------------------------------------------------
// RTC (MCP7940N)
// ---------------------------------------------------------------------------

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int unbcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

void Rtc::reset(bool fromHost) {
    m_vbaten = true;
    m_h24 = true;
    m_clocks = 0;
    m_pos = 0;
    if (fromHost) {
        std::time_t t = std::time(nullptr);
        std::tm tm = *std::localtime(&t);
        m_running = true;
        m_sec = tm.tm_sec; m_min = tm.tm_min; m_hour = tm.tm_hour;
        m_dow = tm.tm_wday == 0 ? 7 : tm.tm_wday;
        m_day = tm.tm_mday; m_month = tm.tm_mon + 1; m_year = tm.tm_year - 100;
    } else {
        m_running = false;                      // the MCP7940N starts stopped
        m_sec = m_min = m_hour = 0;
        m_dow = m_day = m_month = 1;
        m_year = 0;
    }
}

void Rtc::step(uint64_t cpuCycles, uint64_t cpuHz) {
    static const uint8_t dpm[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (!m_running) return;
    m_clocks += cpuCycles;
    while (m_clocks >= cpuHz) {
        m_clocks -= cpuHz;
        if (++m_sec < 60) continue;
        m_sec = 0;
        if (++m_min < 60) continue;
        m_min = 0;
        if (++m_hour < 24) continue;
        m_hour = 0;
        if (++m_dow > 7) m_dow = 1;
        int days = dpm[m_month - 1] + (m_month == 2 && leap() ? 1 : 0);
        if (++m_day <= days) continue;
        m_day = 1;
        if (++m_month <= 12) continue;
        m_month = 1;
        if (++m_year == 100) m_year = 0;
    }
}

void Rtc::data(uint8_t v) {
    if (m_pos < sizeof(m_data)) m_data[m_pos++] = v;
}

uint8_t Rtc::read() {
    uint8_t r = m_data[0];
    uint8_t ret;
    switch (r) {
        case 0: ret = (uint8_t)(bcd(m_sec) | (m_running << 7)); break;
        case 1: ret = bcd(m_min); break;
        case 2: {
            int h = m_hour;
            bool pm = false;
            if (!m_h24) {
                if (h >= 12) { pm = true; h -= 12; }
                if (h == 0) h = 12;
            }
            ret = (uint8_t)(bcd(h) | (pm << 5) | ((!m_h24) << 6));
            break;
        }
        case 3: ret = (uint8_t)(m_dow | (m_vbaten << 3) | (m_running << 5)); break;
        case 4: ret = bcd(m_day); break;
        case 5: ret = (uint8_t)(bcd(m_month) | (leap() << 5)); break;
        case 6: ret = bcd(m_year); break;
        default:
            if (r >= 0x20 && r < 0x60) ret = m_nvram[r - 0x20];
            else if (r >= 0x60) ret = 0xFF;
            else ret = 0;
            break;
    }
    m_pos = 0;
    return ret;
}

void Rtc::write() {
    uint8_t r = m_data[0], v = m_data[1];
    switch (r) {
        case 0: m_running = v & 0x80; m_sec = unbcd(v & 0x7F); break;
        case 1: m_min = unbcd(v); break;
        case 2: {
            m_h24 = !(v & 0x40);
            uint8_t h = v & 0x3F;
            bool pm = false;
            if (!m_h24) { pm = v & 0x20; h &= 0x1F; }
            int hh = unbcd(h);
            if (!m_h24 && hh == 12) hh = 0;
            if (pm) hh += 12;
            m_hour = hh;
            break;
        }
        case 3: m_dow = v & 7; m_vbaten = v & 0x20; break;
        case 4: m_day = unbcd(v); break;
        case 5: m_month = unbcd(v); break;
        case 6: m_year = unbcd(v); break;
        default:
            if (r >= 0x20 && r < 0x60) m_nvram[r - 0x20] = v;
            break;
    }
    m_pos = 0;
}

// ---------------------------------------------------------------------------
// I2C bus
// ---------------------------------------------------------------------------

void I2cBus::reset() {
    m_state = -1;
    m_readMode = false;
    m_value = 0;
    m_count = 0;
    m_clkIn = m_dataIn = m_dataOut = true;
    m_oldClk = m_oldData = true;
}

void I2cBus::setMaster(bool clk, bool data) {
    m_clkIn = clk;
    m_dataIn = data;
    step();
}

// x16-emulator i2c_step(): sample on SCL rising edges
void I2cBus::step() {
    if (m_oldClk == m_clkIn && m_oldData == m_dataIn) return;
    if (m_state == -1 && !m_clkIn && !m_dataIn) m_state = 0;                 // START
    if (m_state == 1 && m_clkIn && m_dataIn && !m_oldData) {                  // STOP
        m_state = -1;
        m_count = 0;
        m_readMode = false;
    }
    if (m_state != -1 && m_clkIn && !m_oldClk) {
        m_dataOut = true;
        if (m_state < 8) {
            if (m_readMode) {
                if (m_state == 0) {
                    m_value = m_device == DEVICE_SMC ? m_smc.read()
                            : m_device == DEVICE_RTC ? m_rtc.read() : 0xFF;
                }
                m_dataOut = (m_value & 0x80) != 0;
                m_value <<= 1;
            } else {
                m_value = (uint8_t)((m_value << 1) | (m_dataIn ? 1 : 0));
            }
            m_state++;
        } else {                                                              // ACK bit
            if (m_readMode) {
                if (m_dataIn) {                                               // NACK from the master
                    m_count = 0;
                    m_readMode = false;
                }
            } else {
                bool ack = true;
                if (m_count == 0) {
                    m_device = m_value >> 1;
                    m_readMode = m_value & 1;
                    if (m_device != DEVICE_SMC && m_device != DEVICE_RTC) ack = false;
                } else {
                    if (m_device == DEVICE_SMC) m_smc.data(m_value);
                    else if (m_device == DEVICE_RTC) m_rtc.data(m_value);
                    if (m_count > 1) {
                        if (m_device == DEVICE_SMC) m_smc.write();
                        else if (m_device == DEVICE_RTC) m_rtc.write();
                    }
                }
                if (ack) {
                    m_dataOut = false;
                    m_count++;
                } else {
                    m_count = 0;
                    m_readMode = false;
                }
            }
            m_state = 0;
        }
    }
    m_oldClk = m_clkIn;
    m_oldData = m_dataIn;
}

// ---------------------------------------------------------------------------
// VIA1 port A
// ---------------------------------------------------------------------------

void Via1PortA::update() {
    uint8_t lines = (uint8_t)(m_ora | ~m_ddr);   // open collector: released bits read 1
    m_bus.setMaster((lines & 0x02) != 0, (lines & 0x01) != 0);
    // NES controllers: latch (PA2) loads the shift registers; clock (PA3)
    // shifts. With no controllers every data line reads 1 once latched.
    bool latch = m_ora & 0x04, clk = m_ora & 0x08;
    if (latch) m_nesData = 0xF0;
    else if (clk && !m_clk) m_nesData = 0xF0;
    m_latch = latch;
    m_clk = clk;
}

uint8_t Via1PortA::peekPort() {
    // SDA: the slave's level when the VIA lets it float; SCL is pulled up
    return (uint8_t)((m_bus.slaveData() ? 0x01 : 0) | 0x02 | (m_ora & 0x0C) | m_nesData);
}

uint8_t Via1PortA::readPort() { return peekPort(); }

} // namespace x16
