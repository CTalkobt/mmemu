#pragma once

#include "libdevices/main/io_handler.h"
#include "plugins/devices/via6522/main/via6522.h"
#include "plugins/machines/spectrum/main/spectrum_ay.h"
#include <cstdint>

namespace apple2 {

/**
 * Sweet Micro Systems' Mockingboard (the "A"/"C" sound boards), after
 * AppleWin's Mockingboard.cpp and MAME's a2mockingbd.cpp: two 6522 VIAs
 * each driving an AY-3-8910 (the AY core is the Spectrum 128's, after
 * Fuse), clocked at the CPU's 1.0205 MHz.
 *
 * - $Cn00-$Cn7F: VIA 1 (register = address & 15), $Cn80-$CnFF: VIA 2.
 * - A VIA's port A is its AY's data bus; port B bits 0-2 are BC1, BDIR and
 *   /RESET: with /RESET low the AY resets; else (BDIR, BC1) 00 inactive,
 *   01 read (port A reads the selected register), 10 write, 11 latch the
 *   register number.
 * - Both VIAs' IRQs go to the CPU's /IRQ (their timers pace most players).
 * - AY 1 is the left channel, AY 2 the right; mixed into the machine's
 *   mono sound with the speaker.
 * - No speech chip (the SSI-263 of the "C"/Sound-II).
 */
class Mockingboard : public IOHandler {
public:
    explicit Mockingboard(int slot = 4);
    int slot() const { return m_slot; }

    /** $Cn00-$CnFF; `at` is the access's cycle. */
    uint8_t read(uint16_t addr, uint64_t at);
    uint8_t peek(uint16_t addr);
    void write(uint16_t addr, uint8_t v, uint64_t at);
    /** Either VIA asserting /IRQ. */
    bool irq() const { return m_irq[0].get() || m_irq[1].get(); }
    /** The AYs run up to `until` (cycles); the average level since the last
     *  call, -1..1, both chips mixed. */
    float sample(uint64_t until);
    /** A chip's register (tests). */
    uint8_t ayReg(int chip, int r) const { return m_ay[chip & 1].reg(r); }

    // IOHandler
    const char* name() const override { return m_name.c_str(); }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override;
    void tick(uint64_t cycles) override;
    void getDeviceInfo(DeviceInfo& out) const override;

private:
    struct Line : ISignalLine {
        bool level = false;
        bool get() const override { return level; }
        void set(bool l) override { level = l; }
        void pulse() override {}
    };
    // An AY's data bus as its VIA's port A sees it (the register, while reading)
    struct AyBus : IPortDevice {
        Mockingboard* mb = nullptr;
        int chip = 0;
        uint8_t readPort() override { return mb->m_reading[chip] ? mb->m_ay[chip].read() : 0xFF; }
        void writePort(uint8_t) override {}
        void setDdr(uint8_t) override {}
    };
    void control(int chip, uint8_t portB, uint64_t at);
    void sync(uint64_t to);
    uint8_t portA(int chip);

    std::string m_name;
    int m_slot;
    VIA6522 m_via[2];
    spectrum::SpectrumAy m_ay[2];
    Line m_irq[2];
    AyBus m_bus[2];
    bool m_reading[2] = {false, false};
    uint8_t m_oraLatch[2] = {0, 0};          // what was written to ORA
    uint64_t m_ayTime = 0;                  // cycles the AYs have run to
    double m_sum = 0;
    unsigned m_count = 0;
    float m_last = 0;
};

}  // namespace apple2
