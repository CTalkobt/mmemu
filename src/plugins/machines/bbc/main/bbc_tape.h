#pragma once

#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bbc {

/**
 * The 6850 ACIA at &FE08 (b-em acia.c, sysacia.c): the receive register and
 * its interrupt (control bit 7), the transmit register, DCD (the cassette
 * carrier: its rising edge interrupts), CTS. A master reset (control 3)
 * clears all but DCD and CTS.
 *
 * Sending takes a byte time in cassette mode: 10 bits at 1200 baud (300
 * with the ACIA dividing by 64), as the OS times its output by the
 * transmit register (b-em empties it at once, and the OS then writes each
 * byte several times). For RS423 the register empties at the next poll.
 */
class Acia6850 {
public:
    static constexpr uint8_t RDRF = 0x01, TDRE = 0x02, DCD = 0x04, CTS = 0x08, IRQ = 0x80;
    std::function<void(uint8_t)> onTransmit;

    void reset() { m_status = 0; m_control = 0; m_rx = 0; m_txLeft = 0; }
    uint8_t read(uint16_t addr);
    void write(uint16_t addr, uint8_t v);
    void poll(int cycles);           // every 128 cycles
    void setCassette(bool on) { m_cassette = on; }
    void receive(uint8_t v);
    void dcdHigh();
    void dcdLow();
    void clearStatus(uint8_t bits) { m_status &= (uint8_t)~bits; }
    bool irq() const { return rxInt() || txInt(); }
    uint8_t status() const { return m_status; }
    uint8_t control() const { return m_control; }

private:
    bool rxInt() const { return (m_status & IRQ) && (m_control & 0x80); }
    bool txInt() const { return (m_status & TDRE) && ((m_control & 0x60) == 0x20); }
    uint8_t m_status = 0, m_control = 0, m_rx = 0;
    bool m_cassette = true;
    int64_t m_txLeft = 0;            // cycles until the byte has gone
};

/**
 * A UEF cassette (b-em tape.c, uef.c): gzipped or not, played while the
 * machine's motor relay is on and PLAY is down, a byte time at a time (1200
 * baud: 120 bytes a second; the baud-rate chunk changes it). The machine
 * takes what it hears through the hooks: carrier (each byte time of tone),
 * the start of a data chunk, each data byte; gaps are silence. The tape
 * stops at its end. The BBC Micro wires the hooks to its 6850 (DCD, the
 * receive register), the Electron to its ULA.
 *
 * Recording (CLI `tape record` / `tape save`, the GUI tape pane): bytes the
 * machine sends while the motor runs in output mode, with the idle time
 * between them as carrier tone, saved as a UEF.
 */
class UefTape : public IOHandler {
public:
    std::function<void()> onCarrier, onDataStart;
    std::function<void(uint8_t)> onByte;

    UefTape();
    const char* name() const override { return "Tape"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override;
    void tick(uint64_t) override {}

    /** The motor relay; `output`: the machine sends to tape (recording takes it). */
    void setMotor(bool on, bool output);
    /** Run `cycles` 2 MHz cycles. */
    void clock(uint64_t cycles);
    /** A byte the machine sent. */
    void transmitted(uint8_t v);

    bool mountTape(const std::string& path) override;
    void controlTape(const std::string& op) override;
    bool startTapeRecord() override;
    void stopTapeRecord() override;
    bool saveTapeRecording(const std::string& path) override;
    bool isTapeRecording() const override { return m_recording; }

    /** A tape from bytes (tests); a gzipped UEF is unpacked. */
    bool load(const std::vector<uint8_t>& file, std::string* err = nullptr);
    bool motor() const { return m_motor; }
    bool playing() const { return m_play; }
    size_t position() const { return m_pos; }
    bool atEnd() const { return m_pos >= m_uef.size() && !m_inChunk; }
    const std::vector<uint8_t>& recording() const { return m_rec; }

private:
    void poll();                     // a byte time
    uint8_t next() { return m_pos < m_uef.size() ? m_uef[m_pos++] : 0; }
    uint16_t next16() { uint16_t v = next(); return (uint16_t)(v | (next() << 8)); }
    uint32_t next32() { uint32_t v = next16(); return v | ((uint32_t)next16() << 16); }
    float nextFloat();
    void skip(uint32_t n) { m_pos = std::min(m_uef.size(), m_pos + n); }
    void setBaud(double baud);
    // Recording
    void recTone(uint64_t cycles);
    void recChunk(uint16_t id, const std::vector<uint8_t>& body);
    void recFlushData();

    std::vector<uint8_t> m_uef;      // the chunks (after the 12-byte header)
    size_t m_pos = 0;
    bool m_motor = false, m_play = false, m_output = true;
    // b-em uef_poll state
    bool m_inChunk = false, m_startChunk = false, m_secondTone = false;
    uint16_t m_chunkId = 0;
    int64_t m_chunkLen = 0;
    int m_chunkPos = 0, m_dataBits = 8, m_inTone = 0;
    float m_gapLeft = 0;
    double m_pps = 120;              // byte times a second
    // Timing: b-em polls every 128 cycles, a byte every `latch` of those
    uint64_t m_cycleAcc = 0;
    int m_latch = 130, m_latchCount = 0;
    uint64_t m_now = 0;
    // Recording
    bool m_recording = false;
    std::vector<uint8_t> m_rec, m_recData;
    uint64_t m_recIdleFrom = 0;
};

}  // namespace bbc
