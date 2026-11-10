// The MSU-1, as Mesen: found by <rom>.msu or msu1.rom beside the ROM; its ID, status, data port and audio
// tracks (44.1 kHz PCM, resampled into the DSP's output), playing once or looping.

#include "test_harness.h"
#include "plugins/machines/snes/main/machine_snes.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/io_registry.h"
#include "libmem/main/ibus.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

const std::string DIR = "/tmp/mmemu_msu1_test";

void writeFile(const std::string& path, const std::vector<uint8_t>& d) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write((const char*)d.data(), (std::streamsize)d.size());
}
/** A 256 KB LoROM that loops at $8000. */
std::vector<uint8_t> rom() {
    std::vector<uint8_t> r(0x40000, 0);
    std::memcpy(r.data() + 0x7FC0, "MSU1 TEST            ", 21);
    r[0x7FD5] = 0x20; r[0x7FD6] = 0x00; r[0x7FD7] = 0x08; r[0x7FD8] = 0x00; r[0x7FD9] = 0x01;
    r[0x7FFC] = 0x00; r[0x7FFD] = 0x80;
    r[0] = 0x80; r[1] = 0xFE;
    return r;
}
/** A track: "MSU1", the loop point, `n` samples of (l, r). */
std::vector<uint8_t> track(uint32_t n, int16_t l, int16_t r, uint32_t loop = 0) {
    std::vector<uint8_t> d = {'M', 'S', 'U', '1', (uint8_t)loop, (uint8_t)(loop >> 8), (uint8_t)(loop >> 16), (uint8_t)(loop >> 24)};
    for (uint32_t i = 0; i < n; ++i) {
        d.push_back((uint8_t)l); d.push_back((uint8_t)((uint16_t)l >> 8));
        d.push_back((uint8_t)r); d.push_back((uint8_t)((uint16_t)r >> 8));
    }
    return d;
}
IAudioOutput* audioOf(MachineDescriptor* m) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if (auto* a = dynamic_cast<IAudioOutput*>(h)) return a;
    return nullptr;
}
/** Run `sec` seconds; the mean of the last 1000 output samples. */
double run(MachineDescriptor* m, IAudioOutput* audio, double sec) {
    const uint64_t to = snes::masterClock(m) + (uint64_t)(21477272.0 * sec);
    std::vector<float> buf(4096), last;
    while (snes::masterClock(m) < to) {
        m->schedulerStep(*m);
        int n;
        while ((n = audio->pullSamples(buf.data(), (int)buf.size())) > 0) last.insert(last.end(), buf.begin(), buf.begin() + n);
        if (last.size() > 4000) last.erase(last.begin(), last.end() - 1000);
    }
    if (last.size() > 1000) last.erase(last.begin(), last.end() - 1000);
    double s = 0;
    for (float v : last) s += v;
    return last.empty() ? 0 : s / (double)last.size();
}

}  // namespace

TEST_CASE(snes_msu1) {
    mkdir(DIR.c_str(), 0755);
    mkdir((DIR + "/shared").c_str(), 0755);
    std::remove((DIR + "/game.msu").c_str());
    std::string err;
    MachineDescriptor* m = snes::create(false, &err);
    ASSERT(m);
    IBus* bus = m->buses[0].bus;
    IAudioOutput* audio = audioOf(m);
    ASSERT(audio);

    // Nothing beside the ROM: no MSU-1, $2002 is open bus
    ASSERT(snes::insertCartridge(m, rom(), "game.sfc", err, "", DIR + "/game.sfc"));
    EXPECT_TRUE(snes::cartridgeInfo(m).find("MSU-1") == std::string::npos);
    bus->write8(0x004300, 0x77);
    EXPECT_TRUE(bus->read8(0x002002) != 'S');

    // <rom>.msu: the ID at $2002-$2007 (banks $00-$3F/$80-$BF only)
    std::vector<uint8_t> data(64);
    for (size_t i = 0; i < data.size(); ++i) data[i] = (uint8_t)(0xA0 + i);
    writeFile(DIR + "/game.msu", data);
    writeFile(DIR + "/game-1.pcm", track(22050, 16000, 16000));       // half a second
    writeFile(DIR + "/game-3.pcm", track(4410, -8000, -8000, 2205));  // 0.1 s, looping its second half
    writeFile(DIR + "/game-4.pcm", {'M', 'S', 'U', '1', 0, 0, 0, 0});  // under 12 bytes: missing
    ASSERT(snes::insertCartridge(m, rom(), "game.sfc", err, "", DIR + "/game.sfc"));
    EXPECT_TRUE(snes::cartridgeInfo(m).find(", MSU-1") != std::string::npos);
    std::string id;
    for (uint32_t a = 0x2002; a < 0x2008; ++a) id += (char)bus->read8(a);
    EXPECT_TRUE(id == "S-MSU1");
    EXPECT_EQ(bus->read8(0x802007), '1');
    EXPECT_TRUE(bus->read8(0x402002) != 'S');

    // Status: revision 1; "playing" at power-on (as Mesen) until the first sample finds no track
    EXPECT_EQ(bus->read8(0x002000), 0x11);
    EXPECT_NEAR(run(m, audio, 0.02), 0.0, 1e-6);
    EXPECT_EQ(bus->read8(0x002000), 0x01);

    // The data port: the pointer takes effect at $2003; peeks don't move it; past the end, 0
    bus->write8(0x002000, 0x3E); bus->write8(0x002001, 0); bus->write8(0x002002, 0);
    EXPECT_EQ(bus->read8(0x002001), data[0]);
    bus->write8(0x002003, 0);
    EXPECT_EQ(bus->peek8(0x002001), data[0x3E]);
    EXPECT_EQ(bus->read8(0x002001), data[0x3E]);
    EXPECT_EQ(bus->read8(0x002001), data[0x3F]);
    EXPECT_EQ(bus->read8(0x002001), 0);
    bus->write8(0x002000, 5); bus->write8(0x002003, 0);
    EXPECT_EQ(bus->read8(0x002001), data[5]);

    // A missing track (none, or too short), then track 1 at full volume: the DSP is muted at power-on,
    // so what comes out is the MSU-1's (16000 + 16000) / 65536
    bus->write8(0x002004, 2); bus->write8(0x002005, 0);
    EXPECT_EQ(bus->read8(0x002000), 0x09);
    bus->write8(0x002004, 4); bus->write8(0x002005, 0);
    EXPECT_EQ(bus->read8(0x002000), 0x09);
    bus->write8(0x002004, 1); bus->write8(0x002005, 0);
    EXPECT_EQ(bus->read8(0x002000), 0x01);
    bus->write8(0x002006, 0xFF);
    bus->write8(0x002007, 0x01);
    EXPECT_EQ(bus->read8(0x002000), 0x11);
    EXPECT_NEAR(run(m, audio, 0.2), 32000.0 / 65536.0, 0.002);
    // Half volume
    bus->write8(0x002006, 0x80);
    EXPECT_NEAR(run(m, audio, 0.1), 2 * (16000 * 128 / 255) / 65536.0, 0.002);
    // Paused, then on again; it ends after half a second and stops
    bus->write8(0x002007, 0x00);
    EXPECT_NEAR(run(m, audio, 0.05), 0.0, 1e-6);
    EXPECT_EQ(bus->read8(0x002000), 0x01);
    bus->write8(0x002007, 0x01);
    run(m, audio, 0.3);
    EXPECT_EQ(bus->read8(0x002000), 0x01);
    EXPECT_NEAR(run(m, audio, 0.02), 0.0, 1e-6);

    // Track 3 repeating: past its end it goes on from the loop point
    bus->write8(0x002006, 0xFF);
    bus->write8(0x002004, 3); bus->write8(0x002005, 0);
    bus->write8(0x002007, 0x03);
    EXPECT_EQ(bus->read8(0x002000), 0x31);
    EXPECT_NEAR(run(m, audio, 0.5), -16000.0 / 65536.0, 0.002);
    EXPECT_EQ(bus->read8(0x002000), 0x31);
    // Repeat off: it ends at the track's end
    bus->write8(0x002007, 0x01);
    run(m, audio, 0.15);
    EXPECT_EQ(bus->read8(0x002000), 0x01);

    // Power on again: the MSU-1 as new (volume 100, track 0, the pointer at 0)
    bus->write8(0x002000, 9); bus->write8(0x002003, 0);
    snes::setRegion(m, "auto");
    EXPECT_EQ(bus->read8(0x002001), data[0]);

    // msu1.rom: tracks are track-<n>.pcm
    writeFile(DIR + "/shared/msu1.rom", {0x42, 0x43});
    writeFile(DIR + "/shared/track-7.pcm", track(4410, 4000, 4000));
    ASSERT(snes::insertCartridge(m, rom(), "other.sfc", err, "", DIR + "/shared/other.sfc"));
    EXPECT_TRUE(snes::cartridgeInfo(m).find(", MSU-1") != std::string::npos);
    EXPECT_EQ(bus->read8(0x002001), 0x42);
    bus->write8(0x002004, 7); bus->write8(0x002005, 0);
    EXPECT_EQ(bus->read8(0x002000) & 0x08, 0);

    // Taking the cartridge out takes the MSU-1 with it
    snes::removeCartridge(m);
    EXPECT_TRUE(bus->read8(0x002002) != 'S');
    delete m;
}
