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
    TapArchive m_tape;
    uint32_t   m_offset = 0;
    int32_t    m_pulseRemaining = 0;
    bool       m_pulseLevel = true;
    bool       m_playing = false;
    bool       m_motorOn = false;
    bool       m_buttonPressed = false;

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
