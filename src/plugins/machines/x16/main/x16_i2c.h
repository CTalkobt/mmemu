#pragma once

// Commander X16 I2C bus: the system management controller (SMC: keyboard,
// mouse, power/reset) at address $42 and the MCP7940N real-time clock at
// $6F, bit-banged by VIA1 port A (PA0 = SDA, PA1 = SCL).
//
// Ported from x16-emulator (src/i2c.c, smc.c, rtc.c, joystick.c),
// Copyright (c) 2021 Michael Steil and contributors, 2-clause BSD license.

#include "libdevices/main/iport_device.h"
#include <cstdint>
#include <functional>

namespace x16 {

/** The SMC: keyboard and mouse buffers, power and reset requests. */
class Smc {
public:
    void reset();
    void data(uint8_t v);       // a byte written after the register number
    uint8_t read();             // a read transaction
    void write();               // a write transaction ends

    /** Keyboard: IBM key number (1-127), bit 7 set for a release; $7F/$FF prefix extended keys. */
    void keyAdd(uint8_t code);
    bool keyBufferEmpty() const { return m_kbdHead == m_kbdTail; }

    std::function<void()> onReset;      // reset requested (register 1/2)
    std::function<void()> onNmi;        // NMI requested (register 3)
    std::function<void()> onPowerOff;   // power off (register 1 = 0)
    uint8_t activityLed() const { return m_activityLed; }

private:
    uint8_t kbdNext();
    static constexpr int KBD_SIZE = 16;
    uint8_t m_kbd[KBD_SIZE] = {};
    uint8_t m_kbdHead = 0, m_kbdTail = 0;
    uint8_t m_data[16] = {};
    uint8_t m_pos = 0;
    uint8_t m_defaultReadOp = 0x41;
    uint8_t m_defaultReadState = 0;
    uint8_t m_activityLed = 0;
};

/** MCP7940N: BCD time registers 0-6, 64 bytes of NVRAM at $20-$5F. */
class Rtc {
public:
    /** `fromHost`: start running at the host's local time (as x16-emulator does). */
    void reset(bool fromHost = true);
    void step(uint64_t cpuCycles, uint64_t cpuHz);
    void data(uint8_t v);
    uint8_t read();
    void write();
    uint8_t* nvram() { return m_nvram; }

private:
    bool leap() const { return !(m_year & 3); }
    bool m_running = false, m_vbaten = true, m_h24 = true;
    uint64_t m_clocks = 0;
    int m_sec = 0, m_min = 0, m_hour = 0, m_dow = 1, m_day = 1, m_month = 1, m_year = 0;
    uint8_t m_nvram[0x40] = {};
    uint8_t m_data[16] = {};
    uint8_t m_pos = 0;
};

/** The bus state machine: START/STOP, addressing, ACK/NACK, bytes in and out. */
class I2cBus {
public:
    I2cBus(Smc& smc, Rtc& rtc) : m_smc(smc), m_rtc(rtc) {}
    void reset();
    /** The lines as the master (VIA) leaves them: 1 = released (pulled up). */
    void setMaster(bool clk, bool data);
    /** The data line as the slave drives it (1 = released). */
    bool slaveData() const { return m_dataOut; }

private:
    void step();
    Smc& m_smc;
    Rtc& m_rtc;
    bool m_clkIn = true, m_dataIn = true, m_dataOut = true;
    bool m_oldClk = true, m_oldData = true;
    int m_state = -1;               // -1 STOP, 0-7 bit, 8 ACK
    bool m_readMode = false;
    uint8_t m_value = 0;
    int m_count = 0;
    uint8_t m_device = 0;
};

/**
 * VIA1 port A: PA0/PA1 the I2C bus (open collector: a line is low only
 * while its DDR bit is 1 and its output bit 0), PA2 NES latch, PA3 NES
 * clock, PA4-PA7 NES data for controllers 3-0 (no controllers: 1 after
 * the first latch).
 */
class Via1PortA : public IPortDevice {
public:
    explicit Via1PortA(I2cBus& bus) : m_bus(bus) {}
    uint8_t readPort() override;
    uint8_t peekPort() override;
    void writePort(uint8_t val) override { m_ora = val; update(); }
    void setDdr(uint8_t ddr) override { m_ddr = ddr; update(); }
    void reset() { m_ora = 0; m_ddr = 0; m_nesData = 0; m_latch = false; m_clk = false; update(); }

private:
    void update();
    I2cBus& m_bus;
    uint8_t m_ora = 0, m_ddr = 0, m_nesData = 0;
    bool m_latch = false, m_clk = false;
};

} // namespace x16
