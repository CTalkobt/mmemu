#include "test_harness.h"
#include "plugins/machines/x16/main/x16_i2c.h"
#include "plugins/devices/via6522/main/via6522.h"

// The I2C bus as the KERNAL drives it through VIA1 port A: lines are open
// collector (ORA bits 0, a DDR bit of 1 pulls the line low).

namespace {
struct Rig {
    x16::Smc smc;
    x16::Rtc rtc;
    x16::I2cBus bus{smc, rtc};
    x16::Via1PortA port{bus};
    VIA6522 via{"VIA1", 0x9F00};
    uint8_t ddr = 0;
    Rig() {
        smc.reset();
        rtc.reset(false);
        bus.reset();
        via.reset();
        via.setPortADevice(&port);
        via.ioWrite(nullptr, 0x9F01, 0x00);        // ORA: outputs low when enabled
        setLines(true, true);
    }
    void setLines(bool scl, bool sda) {
        ddr = (uint8_t)((scl ? 0 : 0x02) | (sda ? 0 : 0x01));
        via.ioWrite(nullptr, 0x9F03, ddr);
    }
    bool sda() { uint8_t v = 0; via.ioRead(nullptr, 0x9F01, &v); return v & 1; }
    void start() { setLines(true, true); setLines(true, false); setLines(false, false); }
    void stop() { setLines(false, false); setLines(true, false); setLines(true, true); }
    bool writeByte(uint8_t b) {
        for (int i = 7; i >= 0; i--) {
            bool bit = (b >> i) & 1;
            setLines(false, bit); setLines(true, bit); setLines(false, bit);
        }
        setLines(false, true); setLines(true, true);
        bool ack = !sda();
        setLines(false, true);
        return ack;
    }
    uint8_t readByte(bool ack) {
        uint8_t v = 0;
        for (int i = 0; i < 8; i++) {
            setLines(false, true); setLines(true, true);
            v = (uint8_t)((v << 1) | (sda() ? 1 : 0));
            setLines(false, true);
        }
        setLines(false, !ack); setLines(true, !ack); setLines(false, !ack);
        return v;
    }
    uint8_t readReg(uint8_t dev, uint8_t reg) {
        start();
        writeByte((uint8_t)(dev << 1));
        writeByte(reg);
        stop();                                     // as the KERNAL does (i2c_read_first_byte)
        start();
        writeByte((uint8_t)((dev << 1) | 1));
        uint8_t v = readByte(false);
        stop();
        return v;
    }
    void writeReg(uint8_t dev, uint8_t reg, uint8_t v) {
        start();
        writeByte((uint8_t)(dev << 1));
        writeByte(reg);
        writeByte(v);
        stop();
    }
};
}

TEST_CASE(x16_i2c_unknown_device_nacks_smc_acks) {
    Rig r;
    r.start();
    ASSERT(!r.writeByte(0x50 << 1));               // nothing at $50
    r.stop();
    r.start();
    ASSERT(r.writeByte(0x42 << 1));                // the SMC answers
    r.stop();
}

TEST_CASE(x16_i2c_smc_keyboard_and_version) {
    Rig r;
    ASSERT_EQ((int)r.readReg(0x42, 0x30), 47);     // SMC version major
    r.smc.keyAdd(31);                              // 'A' down
    r.smc.keyAdd(31 | 0x80);
    ASSERT_EQ((int)r.readReg(0x42, 0x07), 31);
    ASSERT_EQ((int)r.readReg(0x42, 0x07), 31 | 0x80);
    ASSERT_EQ((int)r.readReg(0x42, 0x07), 0);      // empty
}

TEST_CASE(x16_i2c_rtc_time_and_nvram) {
    Rig r;
    r.writeReg(0x6F, 0x02, 0x13);                  // 13 h (24-hour)
    r.writeReg(0x6F, 0x01, 0x45);
    r.writeReg(0x6F, 0x00, 0x80 | 0x30);           // start, 30 s
    ASSERT_EQ((int)r.readReg(0x6F, 0x02), 0x13);
    ASSERT_EQ((int)r.readReg(0x6F, 0x01), 0x45);
    ASSERT_EQ((int)r.readReg(0x6F, 0x00), 0xB0);
    r.rtc.step(8000000ull * 31, 8000000);          // 31 s later
    ASSERT_EQ((int)r.readReg(0x6F, 0x00), 0x81);   // 01 s, running
    ASSERT_EQ((int)r.readReg(0x6F, 0x01), 0x46);
    r.writeReg(0x6F, 0x25, 0x5A);
    ASSERT_EQ((int)r.readReg(0x6F, 0x25), 0x5A);
}
