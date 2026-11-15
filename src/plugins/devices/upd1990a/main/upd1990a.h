#pragma once

#include <array>
#include <cstdint>
#include <ctime>
#include <functional>
#include <vector>

/**
 * NEC uPD1990AC serial calendar clock (the Model 100/102's RTC), and its
 * successor the uPD4990A (serial command mode, year register, 4096 Hz and
 * interval timing pulses) as a variant.
 *
 * After the NEC uPD1990AC / uPD4990A data sheets; MAME's upd1990a.cpp (Curt
 * Coder, hap) and VirtualT's clock.cpp consulted. mmsim's own code.
 *
 * Pins: C0-C2 (command), DATA IN, CLK, STB, CS, OE in; DATA OUT and TP out.
 * A command on C0-C2 takes effect on STB rising (with CS high):
 *  - 0 register hold:  DATA OUT = 1 Hz square wave; TP keeps its mode
 *  - 1 register shift: DATA OUT = shift register bit 0; each CLK rising
 *        edge shifts right, DATA IN entering at the top (bit 39 / 47)
 *  - 2 time set:  the shift register is loaded into the time counter,
 *        which is held until register hold, shift or time read
 *  - 3 time read: the time counter is copied to the shift register
 *  - 4 / 5 / 6: TP = 64 / 256 / 2048 Hz
 *  - 7 test mode on the uPD1990AC (the counter runs at 1024 s/s); on the
 *        uPD4990A, C0-C2 all high selects serial command mode: the command
 *        is the 4 bits last shifted in through DATA IN (by CLK), and adds
 *        7 TP 4096 Hz, 8-B TP 1 / 10 / 30 / 60 s interval (square wave of
 *        that period), C interval reset (TP high, restart), D interval run,
 *        E interval stop, F test mode.
 *
 * Shift register / time counter layout, LSB first: seconds (BCD), minutes
 * (BCD), hours (BCD, 24 h), day of month (BCD), day of week (0-6) in bits
 * 32-35 and month (1-12, binary) in bits 36-39; the uPD4990A adds the year
 * (BCD 00-99) in bits 40-47.
 *
 * DATA OUT reads high (high impedance pulled up) while OE is low. CS low
 * ignores STB and CLK.
 *
 * Time: the counter runs on emulated time (tick(), in units of the input
 * clock set with setInputHz; 32768 Hz crystal behind it), so it stays
 * consistent with the emulation's speed. It starts from the host's local
 * time plus a user offset (syncToHost / setOffsetSeconds); a time set
 * moves the offset, so the next syncToHost keeps the time the program set.
 * The uPD1990AC has no year: a hidden year (from the host) only decides
 * whether February has 29 days.
 *
 * Time set holds the upper divider stages: the next second comes 31/32 s
 * after the hold ends (MAME's analysis of the 32-step divider).
 */
class Upd1990a {
public:
    enum class Variant { Upd1990ac, Upd4990a };

    explicit Upd1990a(Variant v = Variant::Upd1990ac);

    Variant variant() const { return m_variant; }

    /** The CPU clock tick() counts (default 2.4576 MHz, the Model 100's). */
    void setInputHz(double hz) { m_inputHz = hz; }
    void tick(uint64_t cycles);
    /** The 32768 Hz crystal directly. */
    void tickCrystal(uint64_t xtalTicks);

    void setC(uint8_t c) { m_cPins = c & 7; }
    void setDataIn(bool v) { m_dataIn = v; }
    void setClk(bool level);
    void setStb(bool level);
    void setCs(bool level) { m_cs = level; }
    void setOe(bool level) { m_oe = level; }
    /** C0-C2, DATA IN, then CLK (the order the Model 100's port A update needs). */
    void setPins(uint8_t c, bool dataIn, bool clk) {
        setC(c);
        setDataIn(dataIn);
        setClk(clk);
    }

    bool dataOut() const;
    bool tp() const { return m_tp; }
    /** The TP pin changed (the Model 100: RST 7.5). */
    std::function<void(bool)> onTp;

    /** The current command (0-7, or 0-15 in serial mode). */
    int command() const { return m_cmd; }
    bool held() const { return m_held; }
    bool testMode() const { return m_test; }

    /** The time counter as BCD bytes (sec, min, hour, day, wday|month<<4, year). */
    std::array<uint8_t, 6> counter() const { return m_counter; }
    void setCounter(const std::array<uint8_t, 6>& c) { m_counter = c; }
    std::array<uint8_t, 6> shiftRegister() const { return m_shift; }

    /** Seconds added to host time (persist this in the machine's settings). */
    void setOffsetSeconds(int64_t s) { m_offset = s; }
    int64_t offsetSeconds() const { return m_offset; }
    /** Loads the counter from the host clock + the offset. */
    void syncToHost();
    /** The host clock (tests replace it). */
    std::function<std::time_t()> hostClock;

    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    int shiftBytes() const { return m_variant == Variant::Upd4990a ? 6 : 5; }
    void execute(int cmd);
    void releaseHold();
    void advanceSecond();
    void crystal();
    void setTp(bool v);
    void setTpPeriod(uint64_t halfTicks);
    std::tm counterTm() const;

    Variant m_variant;
    double m_inputHz = 2457600.0;
    double m_frac = 0;                    // input cycles not yet worth a crystal tick (x 32768)
    std::array<uint8_t, 6> m_counter{};   // sec min hour day wday|mon year
    std::array<uint8_t, 6> m_shift{};
    uint8_t m_serialCmd = 0;              // uPD4990A command shift register
    int m_hiddenYear = 2000;              // uPD1990AC: for February only
    uint8_t m_cPins = 0;
    int m_cmd = 0;
    int m_doutMode = 0;                   // what DATA OUT shows: the last of commands 0-3
    bool m_dataIn = false, m_clk = false, m_stb = false, m_cs = true, m_oe = true;
    bool m_held = false, m_test = false;
    bool m_tp = false, m_tpInterval = false, m_tpRun = true;
    uint64_t m_tpHalf = 256;              // crystal ticks per TP half period (64 Hz)
    uint64_t m_tpPhase = 0;
    uint64_t m_secPhase = 0;              // crystal ticks into the current second
    uint64_t m_xtal = 0;                  // crystal ticks since power-on (DATA OUT waveforms)
    int64_t m_offset = 0;
};
