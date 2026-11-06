#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/isignal_line.h"
#include "plugins/cbm-loader/main/tap_parser.h"
#include <vector>
#include <string>

class Datasette : public IOHandler {
public:
    Datasette();
    ~Datasette() override = default;

    // IOHandler interface
    const char* name() const override { return m_name.c_str(); }
    void setName(const std::string& name) override { m_name = name; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus* bus, uint32_t addr, uint8_t* val) override { return false; }
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override { return false; }
    void reset() override;
    void tick(uint64_t cycles) override;
    void getDeviceInfo(DeviceInfo& out) const override;

    // Tape controls
    bool mount(const std::string& path);
    void play();
    void stop();
    void rewind();

    // Recording controls
    bool startRecord();
    void stopRecord();
    bool saveRecording(const std::string& path);

    /**
     * Plus/4 and C16 behaviour (VICE's TAPE_BEHAVIOUR_C16): the read line is
     * a level that flips every half-wave (the 7501 port's cassette input),
     * not a pulse per wave. A version 2 TAP gives one half-wave per value
     * (value x 16 cycles of the TED clock), version 0/1 a full wave as two
     * equal halves (value x 8 each). Recordings are C16 version 2 TAPs of
     * the write line's half-waves. Cycles are those `tick` counts (the TED's
     * 1,773,447 Hz on a PAL Plus/4, as in VICE).
     */
    void setHalfWaveMode(bool on, bool ntsc = false) { m_halfWave = on; m_tape.setRecordC16(on, ntsc); }

    // Wiring
    void setSenseLine(ISignalLine* line) { m_senseLine = line; }
    void setMotorLine(ISignalLine* line) { m_motorLine = line; }
    void setWriteLine(ISignalLine* line) { m_writeLine = line; }
    void setReadPulseLine(ISignalLine* line) { m_readPulseLine = line; }

    bool mountTape(const std::string& path) override { return mount(path); }
    void controlTape(const std::string& op) override {
        if      (op == "play")       play();
        else if (op == "stop")       stop();
        else if (op == "rewind")     rewind();
        else if (op == "record")     startRecord();
        else if (op == "stoprecord") stopRecord();
    }

    bool startTapeRecord()                          override { return startRecord(); }
    void stopTapeRecord()                           override { stopRecord(); }
    bool saveTapeRecording(const std::string& path) override { return saveRecording(path); }
    bool isTapeRecording() const                    override { return m_recording; }

    void setSignalLine(const char* name, ISignalLine* line) override {
        std::string n(name);
        if (n == "sense")     m_senseLine     = line;
        if (n == "motor")     m_motorLine     = line;
        if (n == "write")     m_writeLine     = line;
        if (n == "readPulse") m_readPulseLine = line;
    }

private:
    uint32_t nextHalfWave();

    TapArchive m_tape;
    uint32_t   m_offset = 0;
    int32_t    m_pulseRemaining = 0;
    bool       m_pulseLevel = true;
    bool       m_playing = false;
    bool       m_motorOn = false;
    bool       m_buttonPressed = false;
    bool       m_halfWave = false;        // Plus/4 / C16 read and record
    bool       m_readLevel = true;
    bool       m_secondHalf = false;      // v0/v1 TAP in half-wave mode: the wave's second half
    uint32_t   m_fullGap = 0;

    bool       m_recording = false;
    bool       m_lastWriteLevel = true;
    uint64_t   m_writePulseStart = 0;
    bool       m_haveWriteEdge = false;   // a 0->1 edge seen since recording began
    uint64_t   m_totalCycles = 0;

    ISignalLine* m_senseLine     = nullptr;
    ISignalLine* m_motorLine     = nullptr;
    ISignalLine* m_writeLine     = nullptr;
    ISignalLine* m_readPulseLine = nullptr;
    std::string  m_name = "Datasette";
};
