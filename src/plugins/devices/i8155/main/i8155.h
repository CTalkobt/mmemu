#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

/**
 * Intel 8155 / Oki MSM81C55 RAM, I/O ports and timer (the Model 100/102/200's
 * keyboard-scan, LCD-select and sound/baud timer chip).
 *
 * After the Intel 8155/8156 and Oki MSM81C55 data sheets; MAME's i8155.cpp
 * (Curt Coder, AJR) consulted for the timer readback and the reset state.
 * mmsim's own code.
 *
 * RAM: 256 bytes (IO/M low): readRam / writeRam.
 *
 * I/O registers (IO/M high, address & 7):
 *  - 0 write: command. Bit 0 PA output, 1 PB output, 2-3 port C mode
 *        (00 ALT1 all input, 11 ALT2 all output, 01 ALT3 port A strobed +
 *        PC3-5 output, 10 ALT4 ports A and B strobed), 4 IEA, 5 IEB
 *        (strobed-mode interrupt enables), 6-7 timer command (00 no
 *        effect, 01 stop now, 10 stop after the current TC, 11 start: load
 *        mode and count and start, or, if running, take them at the next TC)
 *  - 0 read: status. Bit 0 INTR A, 1 A BF, 2 INTE A, 3 INTR B, 4 B BF,
 *        5 INTE B, 6 TIMER (terminal count reached since the last status
 *        read; reading clears it)
 *  - 1, 2: ports A, B; 3: port C (6 bits; reads with bits 6-7 set)
 *  - 4: count length low; 5: bits 0-5 count length high, 6-7 the timer
 *        output mode (00 single square wave, 01 continuous square wave,
 *        10 single pulse at TC, 11 continuous pulse). Reads return the
 *        count in progress (see timerReadback) and the mode.
 *
 * Timer: counts TIMER IN clocks (tick()). A count of n (2-$3FFF) gives a
 * period of n clocks: in the square-wave modes TIMER OUT is high for the
 * first ceil(n/2) clocks and low for the rest (the longer half first);
 * in the pulse modes it is high except for the last clock of the period.
 * TC sets the TIMER status flag; single modes then stop (TIMER OUT high),
 * continuous ones reload. Stop drives TIMER OUT high.
 *
 * Strobed modes (ALT3/ALT4): port C carries INTR (PC0 / PC3), BF (PC1 /
 * PC4) and /STB (PC2 / PC5, an input: setStrobeA/B). An input port latches
 * its pins on /STB falling (BF set) and raises INTR on /STB rising if
 * enabled; reading the port clears BF and INTR. An output port sets BF
 * and clears INTR when written; /STB falling (the peripheral took the
 * data) clears BF, /STB rising raises INTR if enabled.
 *
 * Reset: all ports input (the command register cleared), the timer stopped
 * with TIMER OUT high, the TIMER flag cleared; RAM and the count length
 * register keep their contents.
 *
 * Outside world: inPa/inPb/inPc give the pins of input ports (default
 * $FF); outPa/outPb/outPc are called with an output port's value when it
 * changes or becomes an output; onTimerOut on every TIMER OUT change;
 * onIntrA/B when the INTR pins change.
 */
class I8155 {
public:
    enum class PcMode { Alt1, Alt2, Alt3, Alt4 };

    I8155() { reset(); }

    void reset();

    uint8_t readRam(uint8_t addr) const { return m_ram[addr]; }
    void writeRam(uint8_t addr, uint8_t v) { m_ram[addr] = v; }

    /** An I/O register read (address & 7), with side effects. */
    uint8_t readIo(uint8_t reg);
    /** An I/O register read without side effects. */
    uint8_t peekIo(uint8_t reg) const;
    void writeIo(uint8_t reg, uint8_t v);

    /**
     * TIMER IN pulses. Returns how many rising edges TIMER OUT made (a
     * UART clocked by TIMER OUT can be advanced by that many clocks).
     */
    uint64_t tick(uint64_t clocks);

    /** The /STB inputs of the strobed modes (true = high, the idle level). */
    void setStrobeA(bool level);
    void setStrobeB(bool level);

    std::function<uint8_t()> inPa, inPb, inPc;
    std::function<void(uint8_t)> outPa, outPb, outPc;
    std::function<void(bool)> onTimerOut;
    std::function<void(bool)> onIntrA, onIntrB;

    uint8_t command() const { return m_command; }
    uint8_t status() const;
    PcMode pcMode() const;
    bool portAOutput() const { return m_command & 0x01; }
    bool portBOutput() const { return m_command & 0x02; }
    /** The output latches. */
    uint8_t outputA() const { return m_out[0]; }
    uint8_t outputB() const { return m_out[1]; }
    uint8_t outputC() const { return m_out[2]; }
    /** What port C's six pins carry (outputs and handshake lines). */
    uint8_t pcPins() const;

    bool timerOut() const { return m_to; }
    bool timerRunning() const { return m_running; }
    bool timerFlag() const { return m_tcFlag; }
    /** The count length register: 14 bits of count, bits 14-15 the mode. */
    uint16_t countRegister() const { return m_countReg; }
    /** The count and mode the running timer uses. */
    uint16_t loadedCount() const { return m_n; }
    uint8_t loadedMode() const { return m_mode; }
    /** The count readback (registers 4/5): after MAME, the count left in the
     *  current half x 2, bit 0 set in the first half; the register when stopped. */
    uint16_t timerReadback() const;

    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    struct Strobe {
        bool stb = true, bf = false, intr = false;
        uint8_t latched = 0xFF;
    };

    uint8_t readPortRaw(int port) const;
    uint8_t inputPins(int port) const;
    void writeCommand(uint8_t v);
    void writePort(int port, uint8_t v);
    void emitPc();
    bool strobed(int port) const;
    void setStrobe(int port, bool level);
    void setIntr(int port, bool v);

    void loadTimer();
    void stopTimer();
    void setTo(bool level);
    bool toForPos(uint32_t pos) const;

    std::array<uint8_t, 256> m_ram{};
    uint8_t m_command = 0;
    std::array<uint8_t, 3> m_out{};
    std::array<Strobe, 2> m_strobe{};

    uint16_t m_countReg = 0;      // as written (mode in 14-15)
    uint16_t m_n = 0;             // the loaded count (14 bits)
    uint8_t m_mode = 0;           // the loaded mode (0-3)
    uint32_t m_pos = 0;           // clocks into the period
    bool m_running = false, m_to = true, m_tcFlag = false;
    bool m_reloadPending = false, m_stopAtTc = false;
    uint64_t m_edges = 0;         // TIMER OUT rising edges during one tick()
};
