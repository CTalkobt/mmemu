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

    /**
     * Mouse (x16-emulator i2c.c's "fake mouse"): movement accumulates and
     * buttons hold until sendMouseState(), called once per frame, queues
     * PS/2 packets (4 bytes with a wheel for device ID 3, else 3).
     * `dy` is in screen direction (down positive), as the host reports it.
     */
    void mouseMove(int dx, int dy) { m_mouseDx += dx; m_mouseDy -= dy; }
    void mouseButtons(uint8_t buttons) { m_mouseButtons = buttons & 7; }   // bit 0 left, 1 right, 2 middle
    void mouseWheel(int y);
    void sendMouseState();
    uint8_t mouseDeviceId() const { return m_mouseId; }

    std::function<void()> onReset;      // reset requested (register 1/2)
    std::function<void()> onNmi;        // NMI requested (register 3)
    std::function<void()> onPowerOff;   // power off (register 1 = 0)
    uint8_t activityLed() const { return m_activityLed; }

private:
    uint8_t kbdNext();
    void mseAdd(uint8_t v);
    uint8_t mseNext();
    int mseCount() const { return (MSE_SIZE + m_mseHead - m_mseTail) & (MSE_SIZE - 1); }
    void mouseSend(int x, int y, int b, uint8_t w);
    void setMouseId(uint8_t id);
    int mousePacketSize() const { return (m_mouseId == 3 || m_mouseId == 4) ? 4 : 3; }

    static constexpr int MSE_SIZE = 16;
    uint8_t m_mse[MSE_SIZE] = {};
    uint8_t m_mseHead = 0, m_mseTail = 0, m_mseCount = 0;
    int m_mouseDx = 0, m_mouseDy = 0;
    uint8_t m_mouseButtons = 0, m_mouseId = 3;
    int8_t m_wheel = 0;

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
 * clock, PA7-PA4 NES data for controllers 1-4.
 *
 * SNES controllers (x16-emulator joystick.c): the latch loads each
 * connected pad's shift register with its 12 buttons (0 = pressed) and
 * ID bits 12-15 = 1; each rising clock edge shifts in the next bit, 0s
 * after the 16th. A slot with no pad reads 1 throughout, which the KERNAL
 * takes as "not present".
 */
class Via1PortA : public IPortDevice {
public:
    explicit Via1PortA(I2cBus& bus) : m_bus(bus) {}
    uint8_t readPort() override;
    uint8_t peekPort() override;
    void writePort(uint8_t val) override { m_ora = val; update(); }
    void setDdr(uint8_t ddr) override { m_ddr = ddr; update(); }
    void reset() { m_ora = 0; m_ddr = 0; m_nesData = 0; m_latch = false; m_clk = false; update(); }

    /** SNES button bits, in the order the pad sends them (1 = pressed here). */
    enum : uint16_t {
        SNES_B = 1 << 0, SNES_Y = 1 << 1, SNES_SELECT = 1 << 2, SNES_START = 1 << 3,
        SNES_UP = 1 << 4, SNES_DOWN = 1 << 5, SNES_LEFT = 1 << 6, SNES_RIGHT = 1 << 7,
        SNES_A = 1 << 8, SNES_X = 1 << 9, SNES_L = 1 << 10, SNES_R = 1 << 11,
    };
    static constexpr int NUM_PADS = 4;
    /** Plug a pad into slot 0-3 (KERNAL joystick 1-4) or unplug it. */
    void setPadConnected(int slot, bool on);
    bool padConnected(int slot) const { return slot >= 0 && slot < NUM_PADS && m_padOn[slot]; }
    /** The buttons held on slot 0-3 (SNES_* bits, 1 = pressed). */
    void setPadButtons(int slot, uint16_t pressed);

private:
    void update();
    void shiftPads();
    I2cBus& m_bus;
    bool m_padOn[NUM_PADS] = {};
    uint16_t m_padPressed[NUM_PADS] = {};
    uint32_t m_padShift[NUM_PADS] = {};
    uint8_t m_ora = 0, m_ddr = 0, m_nesData = 0;
    bool m_latch = false, m_clk = false;
};

} // namespace x16
