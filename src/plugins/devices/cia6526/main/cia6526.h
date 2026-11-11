#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/iport_device.h"
#include "libdevices/main/isignal_line.h"
#include "libdevices/main/idevice_inspectable.h"
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
 *     one-shot modes; Timer B may optionally count Timer A underflows.  The
 *     timers are cycle-exact for the old 6526 (C64 breadbox), after VICE's
 *     ciatimer: a per-cycle pipeline of start, force-load and one-shot bits
 *     (start counts from the third cycle after the write, a force load lands
 *     two cycles after it), PB6/PB7 timer outputs (pulse or toggle).
 *   - TOD (Time Of Day): 50/60 Hz BCD clock with alarm interrupt.
 *   - ICR (Interrupt Control Register): set/clear mask on write (bit 7 selects
 *     mode); read-to-clear.  The interrupt flags go through the old 6526's
 *     delay pipeline (VICE's ifr_delay): a timer underflow sets its flag at
 *     once, bit 7 and the IRQ output one cycle later; enabling the mask of a
 *     pending flag raises IRQ two cycles later; and the timer B bug (an ICR
 *     read the cycle before a timer B underflow loses its flag).
 *
 * Timing: tick(n) runs n cycles; an access after tick() sees the chip as of
 * the last cycle ticked (the C64 ticks through the access's own cycle).
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
class CIA6526 : public IOHandler, public IDeviceInspectable {
public:
    CIA6526() : m_name("CIA"), m_baseAddr(0) { CIA6526::reset(); }
    CIA6526(const std::string& name, uint32_t baseAddr);

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------

    void setName(const std::string& name) override { m_name = name; }
    void setBaseAddr(uint32_t addr) override       { m_baseAddr = addr; }
    void setClockHz(uint32_t hz) override {
        m_clockHz = hz ? hz : 1022727;
        m_powerHz = m_clockHz < 1000000 ? 50 : 60;      // PAL machines: 50 Hz mains
    }

    void setPortADevice(IPortDevice* d) override { m_portADevice = d; }
    void setPortBDevice(IPortDevice* d) override { m_portBDevice = d; }
    IPortDevice* portADevice() const override { return m_portADevice; }
    IPortDevice* portBDevice() const override { return m_portBDevice; }
    void setOutputsReadPins(bool on) override { m_outputsReadPins = on; }
    /** "model": "6526" (old, the default; breadbin C64s), "6526A" (new;
     *  C64C, C128) or "mega65". The 6526 and 6526A differ in the interrupt
     *  flags' timing; the MEGA65's finishes a serial port byte at once. */
    bool setOption(const std::string& key, const std::string& value, std::string& err) override;
    bool newModel() const { return m_newModel; }
    void setIrqLine(ISignalLine* line)  override { m_irqLine = line; }
    ISignalLine* signalFlag() { return &m_flagLine; }
    ISignalLine* getSignalLine(const char* name) override { if (std::string(name) == "flag") return &m_flagLine; return nullptr; }

    /** Called after every write to PRA or DDRA with the new (pra, ddra) values.
     *  Useful for CIA2 VIC-II bank switching in the C64 machine factory. */
    /** The serial port (SDR): `out` gets each byte shifted out (CRA bit 6
     *  set: output); receiveSerialByte() is a byte shifted in from outside
     *  (the C128's fast serial), raising the SP interrupt. */
    void setSerialOut(std::function<void(uint8_t)> out) { m_serialOut = std::move(out); }
    void receiveSerialByte(uint8_t b) {
        if (m_cra & 0x40) return;                       // output mode: not listening
        m_sdr = b;
        setIrqFlag(ICR_SP);
    }
    /** The CNT pin as the serial port drives it (high when idle). */
    bool cntOut() const { return m_cntOut; }
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
    bool ioPeek (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t  val) override;
    void reset()  override;
    void tick(uint64_t cycles) override;
    void getDeviceInfo(DeviceInfo& out) const override;

    // -----------------------------------------------------------------------
    // IDeviceInspectable interface (cycle advancement for testing)
    // -----------------------------------------------------------------------

    std::string advanceCycles(uint32_t cycles) override;
    std::string getStatusString() const override;

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
    static constexpr uint8_t ICR_SP  = 0x08; // serial port shift complete
    static constexpr uint8_t ICR_INT = 0x80; // any interrupt active
    static constexpr uint8_t CR_PBON   = 0x02; // timer output on PB6/PB7
    static constexpr uint8_t CR_TOGGLE = 0x04; // output toggles (else pulses)

private:
    /** One timer, as VICE's ciat_t: the latch, the counter and the state bits
     *  (T_* below) that pipeline start, force load and one-shot. */
    struct Timer {
        uint16_t state = 0;
        uint16_t latch = 0xFFFF;
        uint16_t cnt   = 0xFFFF;
    };
    // Timer state bits (VICE ciatimer.h CIAT_*): the CR bits as written, and
    // their delayed copies one and two cycles on
    static constexpr uint16_t T_START    = 0x001;
    static constexpr uint16_t T_COUNT2   = 0x002;
    static constexpr uint16_t T_STEP     = 0x004;   // one count (timer A underflow)
    static constexpr uint16_t T_ONESHOT  = 0x008;   // CR one-shot bit
    static constexpr uint16_t T_FLOAD    = 0x010;   // CR force load
    static constexpr uint16_t T_PHI2IN   = 0x020;   // counts phi2
    static constexpr uint16_t T_COUNT3   = 0x040;
    static constexpr uint16_t T_LOAD1    = 0x080;
    static constexpr uint16_t T_ONESHOT0 = 0x100;
    static constexpr uint16_t T_LOAD     = 0x200;
    static constexpr uint16_t T_OUT      = 0x400;   // underflowed this cycle
    static constexpr uint16_t T_COUNT    = 0x800;
    static constexpr uint16_t T_ONESHOT1 = 0x1000;
    static constexpr uint16_t T_CR_MASK  = T_START | T_ONESHOT | T_FLOAD | T_PHI2IN;

    static const uint16_t* stateTable();
    static const uint8_t* steadyTable();
    static bool stepTimer(Timer& t, const uint16_t* table);
    static bool timerSteady(uint16_t t);
    static bool timerCounting(uint16_t t);
    static void setCtrl(Timer& t, uint8_t byte) {
        t.state = (uint16_t)((t.state & ~T_CR_MASK) | ((byte & T_CR_MASK) ^ T_PHI2IN));
    }

    bool readReg(uint32_t addr, uint8_t* val, bool peek);
    void flagTrigger();
    void runCycle();
    void runTimers(uint64_t cycles);
    void runIfrCycle();
    void setIrqFlag(uint32_t bits);
    void setInt(bool on);
    uint8_t pb67(uint8_t byte) const;

    static bool    bcdEqual(uint8_t a, uint8_t b);

    std::string  m_name;
    uint32_t     m_baseAddr;
    uint32_t     m_clockHz = 1022727; // NTSC default
    uint32_t     m_powerHz = 60;      // the mains frequency the TOD counts

    // Port registers
    uint8_t m_pra  = 0xFF; // output latch A
    uint8_t m_prb  = 0xFF; // output latch B
    uint8_t m_ddra = 0x00;
    uint8_t m_ddrb = 0x00;

    Timer m_ta, m_tb;
    uint8_t m_tat = 0, m_tbt = 0;   // PB6/PB7 toggle outputs

    // ICR: the mask, the flags (bit 7 = IR; 0x100 = the timer B bug), flags
    // new this cycle, and VICE's ifr_delay pipeline (IFR_* in the .cpp)
    uint8_t  m_icrMask  = 0x00;
    uint32_t m_irqFlags = 0;
    uint32_t m_newFlags = 0;
    uint32_t m_ackFlags = 0;   // 6526A: the flags an ICR read clears next cycle
    bool     m_newModel = false;   // the 6526A ("new" CIA, C64C) instead of the 6526
    uint32_t m_ifrDelay = 0;
    bool     m_irqOut   = false;    // the IRQ output as driven
    uint64_t m_clk      = 0;        // cycles run
    uint64_t m_rdi      = ~0ull;    // the cycle of the last ICR read
    uint64_t m_quiet    = 0;        // cycles of nothing but steady counting ahead
    bool     m_quietA = false, m_quietB = false;   // ...in which timer A / B count

    // Control registers (as written, without the force-load strobe; START is
    // read back from the timer, which a one-shot underflow stops)
    uint8_t m_cra = 0x00;
    uint8_t m_sdr = 0x00;
    std::function<void(uint8_t)> m_serialOut;
    // The serial port shifting out, after VICE's ciacore (sdr_delay): timer A
    // underflows toggle CNT, a bit goes out on each falling edge
    bool     m_instantSdr = false; // the MEGA65: a byte written goes out at once
    uint32_t m_sdrDelay  = 0;      // the pipeline of shift register events
    uint16_t m_shifter   = 0;
    uint8_t  m_srBits    = 0;      // CNT half-periods left in this byte
    bool     m_sdrValid  = false;  // a byte written waits behind the one going out
    bool     m_sdrForceFinish = false;
    bool     m_cntOut    = true;
    bool     m_sdrActive = false;  // the pipeline runs this cycle
    bool     m_sdrRerun  = false;  // ...and again for this cycle, after a write
    uint32_t m_sdrDelayBefore = 0; // ...as it was before this cycle's step
    uint8_t  m_srBitsBefore = 0;
    uint64_t m_sdrStepClk = ~0ull; // the cycle of the last step
    uint64_t m_spClk     = ~0ull;  // the cycle the pipeline last set the SP flag
    uint32_t m_spPrior   = 0;      // ...and the flags before it
    void runSdrCycle();
    void scheduleSdr(uint32_t feed);
    void sdrModeChange(uint8_t cra);
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

    // The TOD's clock (VICE ciacore_inttod): a mains tick every
    // clockHz / powerHz cycles, kept to powerHz a second; a 3-bit ring
    // counter divides it to tenths (CRA bit 7: 50 or 60 Hz)
    bool     m_todStopped = true;   // writing hours stops it, tenths restarts it
    uint8_t  m_todTickCounter = 0;
    uint32_t m_powerTickCounter = 0;
    uint64_t m_powerTicks = 0;
    uint64_t m_todTicks = 0;        // cycles from the last mains tick to the next
    uint64_t m_todCountdown = 1;    // cycles to the next mains tick
    void powerTick();
    void checkTodAlarm();
    void runCycles(uint64_t cycles);

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
    bool m_outputsReadPins = false;
    ISignalLine* m_irqLine     = nullptr;

    std::function<void(uint8_t, uint8_t)> m_portAWriteCallback;
};
