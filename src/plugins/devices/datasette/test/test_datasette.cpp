#include <iterator>
#include <vector>
#include "test_harness.h"
#include "plugins/devices/datasette/main/datasette.h"
#include "libdevices/main/isignal_line.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

class MockSignalLine : public ISignalLine {
public:
    bool get() const override { return m_level; }
    void set(bool v) override { m_level = v; m_setCount++; }
    void pulse() override { set(!m_level); set(!m_level); }
    bool m_level = true;
    int  m_setCount = 0;
};

// Write a minimal valid TAP file
fs::path writeTapFile(const std::vector<uint8_t>& pulseData) {
    fs::path p = fs::temp_directory_path() / "mmemu_test_datasette.tap";
    std::ofstream f(p, std::ios::binary);
    f.write("C64-TAPE-RAW", 12);
    uint8_t ver = 1;
    f.write((const char*)&ver, 1);
    uint8_t reserved[3] = {};
    f.write((const char*)reserved, 3);
    uint32_t sz = (uint32_t)pulseData.size();
    uint8_t sizeBytes[4] = {
        (uint8_t)(sz & 0xFF), (uint8_t)((sz >> 8) & 0xFF),
        (uint8_t)((sz >> 16) & 0xFF), (uint8_t)((sz >> 24) & 0xFF)
    };
    f.write((const char*)sizeBytes, 4);
    if (!pulseData.empty())
        f.write((const char*)pulseData.data(), pulseData.size());
    return p;
}

} // namespace

TEST_CASE(datasette_initial_state) {
    Datasette ds;
    DeviceInfo info;
    ds.getDeviceInfo(info);
    ASSERT(info.name == "Datasette");

    // Check state entries
    bool foundPlaying = false;
    for (auto& kv : info.state) {
        if (kv.first == "Playing") {
            ASSERT(kv.second == "no");
            foundPlaying = true;
        }
    }
    ASSERT(foundPlaying);
}

TEST_CASE(datasette_mount_and_play) {
    auto path = writeTapFile({0x30, 0x40, 0x50});

    MockSignalLine sense, motor, readPulse;
    motor.m_level = false; // motor on (active low)

    Datasette ds;
    ds.setSenseLine(&sense);
    ds.setMotorLine(&motor);
    ds.setReadPulseLine(&readPulse);

    ASSERT(ds.mount(path.string()));
    // Sense should go low (button pressed)
    ASSERT(!sense.m_level);

    // Tick enough cycles to generate pulses
    // First pulse: 0x30 * 8 = 384 cycles
    for (int i = 0; i < 400; i++) ds.tick(1);
    ASSERT(readPulse.m_setCount > 0);

    fs::remove(path);
}

TEST_CASE(datasette_mount_invalid) {
    Datasette ds;
    ASSERT(!ds.mount("/tmp/mmemu_nonexistent.tap"));
}

TEST_CASE(datasette_stop_and_rewind) {
    auto path = writeTapFile({0x10, 0x20});

    Datasette ds;
    ASSERT(ds.mount(path.string()));

    ds.stop();
    DeviceInfo info;
    ds.getDeviceInfo(info);
    bool playing = true;
    for (auto& kv : info.state) {
        if (kv.first == "Playing") playing = (kv.second == "yes");
    }
    ASSERT(!playing);

    ds.rewind();
    ds.play();
    ds.getDeviceInfo(info);
    for (auto& kv : info.state) {
        if (kv.first == "Playing") playing = (kv.second == "yes");
    }
    ASSERT(playing);

    fs::remove(path);
}

TEST_CASE(datasette_reset) {
    auto path = writeTapFile({0x10});
    Datasette ds;
    ds.mount(path.string());
    ds.reset();

    DeviceInfo info;
    ds.getDeviceInfo(info);
    for (auto& kv : info.state) {
        if (kv.first == "Playing") ASSERT(kv.second == "no");
        if (kv.first == "Motor On") ASSERT(kv.second == "no");
    }
    fs::remove(path);
}

TEST_CASE(datasette_recording) {
    MockSignalLine sense, motor, write, readPulse;
    motor.m_level = false; // motor on
    write.m_level = true;

    Datasette ds;
    ds.setSenseLine(&sense);
    ds.setMotorLine(&motor);
    ds.setWriteLine(&write);
    ds.setReadPulseLine(&readPulse);

    ASSERT(ds.startRecord());
    ASSERT(ds.isTapeRecording());

    // Two full cycles of the write line, 400 and 240 cycles long: one pulse
    // each, from one 0->1 edge of the write line to the next. The datasette
    // sees a level at the end of a tick, so each edge is followed by a
    // 1-cycle tick.
    auto edge = [&](bool level, int gap) { ds.tick(gap - 1); write.m_level = level; ds.tick(1); };
    edge(false, 100);
    edge(true, 50);             // first 0->1: starts the first pulse
    edge(false, 200);
    edge(true, 200);            // pulse of 400
    edge(false, 120);
    edge(true, 120);            // pulse of 240

    ds.stopTapeRecord();
    ASSERT(!ds.isTapeRecording());

    // Save the recording: TAP v1 stores pulse length / 8
    fs::path p = fs::temp_directory_path() / "mmemu_test_datasette_rec.tap";
    ASSERT(ds.saveTapeRecording(p.string()));
    std::ifstream f(p, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ASSERT_EQ((int)bytes.size(), 22);
    ASSERT_EQ((int)bytes[20], 400 / 8);
    ASSERT_EQ((int)bytes[21], 240 / 8);

    fs::remove(p);
}

TEST_CASE(datasette_record_no_write_line) {
    Datasette ds;
    // No write line set — startRecord should fail
    ASSERT(!ds.startRecord());
}

TEST_CASE(datasette_control_tape) {
    auto path = writeTapFile({0x10});
    Datasette ds;
    ds.mount(path.string());

    ds.controlTape("stop");
    ds.controlTape("rewind");
    ds.controlTape("play");

    DeviceInfo info;
    ds.getDeviceInfo(info);
    for (auto& kv : info.state) {
        if (kv.first == "Playing") ASSERT(kv.second == "yes");
    }
    fs::remove(path);
}

TEST_CASE(datasette_set_signal_line) {
    MockSignalLine sense, motor, write, readPulse;
    Datasette ds;

    ds.setSignalLine("sense", &sense);
    ds.setSignalLine("motor", &motor);
    ds.setSignalLine("write", &write);
    ds.setSignalLine("readPulse", &readPulse);

    DeviceInfo info;
    ds.getDeviceInfo(info);
    for (auto& kv : info.dependencies) {
        if (kv.first == "Sense Line") ASSERT(kv.second == "connected");
        if (kv.first == "Motor Line") ASSERT(kv.second == "connected");
    }
}

TEST_CASE(datasette_end_of_tape) {
    // Single short pulse — tape should end quickly
    auto path = writeTapFile({0x01});

    MockSignalLine sense, motor, readPulse;
    motor.m_level = false; // motor on
    sense.m_level = false;

    Datasette ds;
    ds.setSenseLine(&sense);
    ds.setMotorLine(&motor);
    ds.setReadPulseLine(&readPulse);
    ds.mount(path.string());

    // Tick past the pulse
    for (int i = 0; i < 100; i++) ds.tick(1);

    // Sense should go high (tape ended, button released)
    ASSERT(sense.m_level);

    fs::remove(path);
}
