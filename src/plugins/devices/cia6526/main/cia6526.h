#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/iport_device.h"
#include "libdevices/main/isignal_line.h"
#include <cstdint>
#include <functional>
#include <string>

/**
 * MOS 6526 Complex Interface Adapter (CIA).
 *
 * Used in the Commodore 64 as CIA #1 ($DC00) and CIA #2 ($DD00).
 *
 * Implements:
 *   - Port A / Port B with individual data-direction registers (DDRA/DDRB).
 *     Reads consult an optional IPortDevice* for physical pin state; DDR masks
 *     are applied so output bits come from the output latch, input bits from pins.
 *   - Timer A and Timer B: 16-bit countdown timers with latches; continuous or
 *     one-shot modes; Timer B may optionally count Timer A underflows.
 *   - TOD (Time Of Day): 50/60 Hz BCD clock with alarm interrupt.
 *   - ICR (Interrupt Control Register): set/clear mask on write (bit 7 selects
 *     mode); read-to-clear; fires IRQ via ISignalLine when any enabled bit is set.
 *   - CRA / CRB: control registers for timers and TOD.
 *   - SDR: serial data register stub (not fully implemented).
 *
 * Keyboard / joystick injection:
 *   Attach a KbdMatrix or Joystick as Port A / Port B devices; the CIA reads
 *   the current pin state via IPortDevice::readPort().
 *
 * Clock rate:
 *   setClockHz(hz) configures the Phi2 frequency used for TOD and timer
 *   calibration.  Defaults to 1 022 727 Hz (NTSC C64).
 */
class CIA6526 : public IOHandler {
public:
    CIA6526() : m_name("CIA"), m_baseAddr(0) { CIA6526::reset(); }
    CIA6526(const std::string& name, uint32_t baseAddr);

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------

    void setName(const std::string& name) override { m_name = name; }
    void setBaseAddr(uint32_t addr) override       { m_baseAddr = addr; }
    void setClockHz(uint32_t hz) override          { m_clockHz = hz; }

    void setPortADevice(IPortDevice* d) override { m_portADevice = d; }
    void setPortBDevice(IPortDevice* d) override { m_portBDevice = d; }
    void setIrqLine(ISignalLine* line)  override { m_irqLine = line; }
    ISignalLine* signalFlag() { return &m_flagLine; }
    ISignalLine* getSignalLine(const char* name) override { if (std::string(name) == "flag") return &m_flagLine; return nullptr; }

    /** Called after every write to PRA or DDRA with the new (pra, ddra) values.
     *  Useful for CIA2 VIC-II bank switching in the C64 machine factory. */
    void setPortAWriteCallback(std::function<void(uint8_t pra, uint8_t ddra)> cb) override {
        m_portAWriteCallback = std::move(cb);
    }

    // -----------------------------------------------------------------------
    // IOHandler interface
    // -----------------------------------------------------------------------

    const char* name()     const override { return m_name.c_str(); }
    uint32_t    baseAddr() const override { return m_baseAddr; }
    uint32_t    addrMask() const override { return 0x000F; }  // 16 registers

    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t  val) override;
    void reset()  override;
    void tick(uint64_t cycles) override;
    void getDeviceInfo(DeviceInfo& out) const override;

    // -----------------------------------------------------------------------
    // Register indices
    // -----------------------------------------------------------------------
    enum Reg {
        PRA    = 0x0,
        PRB    = 0x1,
        DDRA   = 0x2,
        DDRB   = 0x3,
        TALO   = 0x4,
        TAHI   = 0x5,
        TBLO   = 0x6,
        TBHI   = 0x7,
        TODTEN = 0x8,
        TODSEC = 0x9,
        TODMIN = 0xA,
        TODHR  = 0xB,
        SDR    = 0xC,
        ICR    = 0xD,
        CRA    = 0xE,
        CRB    = 0xF
    };

    // CRA/CRB bit masks
    static constexpr uint8_t CR_START   = 0x01; // timer running
    static constexpr uint8_t CR_ONESHOT = 0x08; // one-shot mode (else continuous)
    static constexpr uint8_t CR_LOAD    = 0x10; // force load latch into counter
    static constexpr uint8_t CRA_TODIN  = 0x80; // TOD freq: 0=60Hz, 1=50Hz
    static constexpr uint8_t CRB_ALARM  = 0x80; // TOD write selects alarm when set
    static constexpr uint8_t CRB_INMODE_MASK = 0x60; // bits 5-6
    static constexpr uint8_t CRB_INMODE_TA   = 0x40; // timer B counts timer A undeflow

    // ICR bit masks
    static constexpr uint8_t ICR_TA  = 0x01;
    static constexpr uint8_t ICR_TB  = 0x02;
    static constexpr uint8_t ICR_TOD = 0x04;
    static constexpr uint8_t ICR_INT = 0x80; // any interrupt active

private:
    void flagTrigger();
    void updateIrq();
    void tickTimerA(uint64_t cycles);
    void tickTimerB(uint64_t cycles, uint32_t taUnderflows);
    void tickTOD(uint64_t cycles);

    static uint8_t bcdInc(uint8_t bcd, uint8_t maxBcd);
    static bool    bcdEqual(uint8_t a, uint8_t b);

    std::string  m_name;
    uint32_t     m_baseAddr;
    uint32_t     m_clockHz = 1022727; // NTSC default

    // Port registers
    uint8_t m_pra  = 0xFF; // output latch A
    uint8_t m_prb  = 0xFF; // output latch B
    uint8_t m_ddra = 0x00;
    uint8_t m_ddrb = 0x00;

    // Timer A
    uint16_t m_taLatch   = 0xFFFF;
    uint16_t m_taCounter = 0xFFFF;
    bool     m_taRunning = false;

    // Timer B
    uint16_t m_tbLatch   = 0xFFFF;
    uint16_t m_tbCounter = 0xFFFF;
    bool     m_tbRunning = false;

    // ICR
    uint8_t m_icrMask   = 0x00; // enabled interrupt sources
    uint8_t m_icrPending = 0x00; // pending (fired) interrupt bits

    // Control registers
    uint8_t m_cra = 0x00;
    uint8_t m_crb = 0x00;

    // TOD clock (BCD)
    uint8_t m_todTen = 0x00;
    uint8_t m_todSec = 0x00;
    uint8_t m_todMin = 0x00;
    uint8_t m_todHr  = 0x01; // BCD hours 1–12 + bit7 AM/PM

    // TOD alarm (BCD)
    uint8_t m_alarmTen = 0x00;
    uint8_t m_alarmSec = 0x00;
    uint8_t m_alarmMin = 0x00;
    uint8_t m_alarmHr  = 0x01;

    // TOD latch (frozen when hours are read; released when tenths are read)
    bool    m_todLatched = false;
    uint8_t m_latchTen = 0x00;
    uint8_t m_latchSec = 0x00;
    uint8_t m_latchMin = 0x00;
    uint8_t m_latchHr  = 0x01;

    bool    m_todRunning = true;
    uint64_t m_todAccum  = 0; // accumulated cycles for TOD

    IPortDevice* m_portADevice = nullptr;
    struct SignalLine : public ISignalLine {
        CIA6526* m_owner;
        uint8_t  m_bit;
        bool     m_level = true;
        SignalLine(CIA6526* owner, uint8_t bit) : m_owner(owner), m_bit(bit) {}
        bool get() const override { return m_level; }
        void set(bool level) override {
            if (m_level && !level) m_owner->flagTrigger();
            m_level = level;
        }
        void pulse() override { set(false); set(true); }
    };
    SignalLine m_flagLine{this, 0x10};
    IPortDevice* m_portBDevice = nullptr;
    ISignalLine* m_irqLine     = nullptr;

    std::function<void(uint8_t, uint8_t)> m_portAWriteCallback;
};
