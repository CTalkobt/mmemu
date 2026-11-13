#include "test_harness.h"
#include "plugins/devices/wd17xx/main/wd17xx.h"
#include "libdevices/main/isignal_line.h"
#include <vector>

// The controller driven as a CPU would: a 1 MHz clock, one tick per cycle

namespace {

class Line : public ISignalLine {
public:
    bool get() const override { return level; }
    void set(bool v) override { level = v; ++changes; }
    void pulse() override {}
    bool level = false;
    int changes = 0;
};

constexpr uint64_t REV = 200000;            // cycles a turn at 1 MHz, 300 RPM

uint8_t patternByte(int cyl, int head, int sec, int i) { return (uint8_t)(cyl * 7 + head * 13 + sec * 31 + i); }

/** A formatted disk: `cyls` x `heads`, `spt` sectors from 1 of 256 bytes, MFM or FM. */
std::unique_ptr<FloppyDisk> makeDisk(int cyls, int heads = 1, int spt = 18, bool dd = true, int first = 1) {
    auto d = std::make_unique<FloppyDisk>(cyls, heads);
    for (int c = 0; c < cyls; ++c)
        for (int h = 0; h < heads; ++h) {
            std::vector<FloppySectorSpec> specs;
            for (int s = 0; s < spt; ++s) {
                FloppySectorSpec sp;
                sp.track = (uint8_t)c; sp.side = (uint8_t)h; sp.sector = (uint8_t)(first + s);
                sp.sizeCode = 1; sp.dd = dd;
                for (int i = 0; i < 256; ++i) sp.data.push_back(patternByte(c, h, first + s, i));
                specs.push_back(sp);
            }
            ASSERT(d->formatTrack(c, h, specs));
        }
    return d;
}

struct Rig {
    Wd17xx f;
    explicit Rig(Wd17xx::Variant v = Wd17xx::Variant::WD2793, std::unique_ptr<FloppyDisk> d = makeDisk(35))
        : f(v) {
        f.setClockHz(1000000);
        if (d) f.insertDisk(0, std::move(d));
        f.selectDrive(0);
    }
    uint64_t now = 0;
    void run(uint64_t n) { for (uint64_t i = 0; i < n; ++i) { f.tick(1); ++now; } }
    /** Tick until INTRQ (or the limit); the cycles it took. */
    uint64_t waitIntrq(uint64_t limit = 3 * 1000 * 1000) {
        uint64_t t = 0;
        while (!f.intrq() && t < limit) { f.tick(1); ++t; ++now; }
        return t;
    }
    /** A reading command, taking each byte `delay` cycles after DRQ. */
    uint64_t readCmd(uint8_t cmd, std::vector<uint8_t>& out, int delay = 0, std::vector<uint64_t>* drqTimes = nullptr,
                     uint64_t limit = 3 * 1000 * 1000) {
        f.writeRegister(0, cmd);
        uint64_t t = 0, drqAt = 0;
        bool pending = false;
        while (t < limit) {
            if (f.drq() && !pending) { pending = true; drqAt = t; if (drqTimes) drqTimes->push_back(t); }
            if (pending && t - drqAt >= (uint64_t)delay) { out.push_back(f.readRegister(3)); pending = false; }
            if (f.intrq() && !f.drq()) break;
            f.tick(1); ++t; ++now;
        }
        return t;
    }
    /** A writing command fed from `data` (then `fill`), `delay` cycles after each DRQ. */
    uint64_t writeCmd(uint8_t cmd, const std::vector<uint8_t>& data, int delay = 0, uint8_t fill = 0x4E,
                      std::vector<uint64_t>* drqTimes = nullptr, bool preload = false) {
        size_t idx = 0;
        if (preload && !data.empty()) f.writeRegister(3, data[idx++]);
        f.writeRegister(0, cmd);
        uint64_t t = 0, drqAt = 0;
        bool pending = false;
        while (t < 3 * 1000 * 1000) {
            if (f.drq() && !pending) { pending = true; drqAt = t; if (drqTimes) drqTimes->push_back(t); }
            if (pending && t - drqAt >= (uint64_t)delay) {
                f.writeRegister(3, idx < data.size() ? data[idx] : fill);
                ++idx;
                pending = false;
            }
            if (f.intrq()) break;
            f.tick(1); ++t; ++now;
        }
        return t;
    }
    uint8_t status() { return f.readRegister(0); }
};

std::vector<uint8_t> sectorData(uint8_t seed) {
    std::vector<uint8_t> v(256);
    for (int i = 0; i < 256; ++i) v[i] = (uint8_t)(seed ^ (i * 3));
    return v;
}

/** A Write Track stream laying out an MFM track, as RS-DOS's DSKINI does. */
std::vector<uint8_t> mfmFormatStream(int cyl, int side, int spt, uint8_t fill) {
    std::vector<uint8_t> s(32, 0x4E);
    for (int sec = 1; sec <= spt; ++sec) {
        s.insert(s.end(), 12, 0x00);
        s.insert(s.end(), 3, 0xF5);
        s.push_back(0xFE); s.push_back((uint8_t)cyl); s.push_back((uint8_t)side);
        s.push_back((uint8_t)sec); s.push_back(0x01); s.push_back(0xF7);
        s.insert(s.end(), 22, 0x4E);
        s.insert(s.end(), 12, 0x00);
        s.insert(s.end(), 3, 0xF5);
        s.push_back(0xFB);
        s.insert(s.end(), 256, (uint8_t)(fill + sec));
        s.push_back(0xF7);
        s.insert(s.end(), 24, 0x4E);
    }
    return s;
}

/** The same in FM (single density) for the FD1771. */
std::vector<uint8_t> fmFormatStream(int cyl, int spt, int first, uint8_t fill) {
    std::vector<uint8_t> s(16, 0xFF);
    for (int sec = first; sec < first + spt; ++sec) {
        s.insert(s.end(), 6, 0x00);
        s.push_back(0xFE); s.push_back((uint8_t)cyl); s.push_back(0);
        s.push_back((uint8_t)sec); s.push_back(0x01); s.push_back(0xF7);
        s.insert(s.end(), 11, 0xFF);
        s.insert(s.end(), 6, 0x00);
        s.push_back(0xFB);
        s.insert(s.end(), 256, (uint8_t)(fill + sec));
        s.push_back(0xF7);
        s.insert(s.end(), 12, 0xFF);
    }
    return s;
}

} // namespace

// Track, sector and data registers hold what is written
TEST_CASE(wd17xx_registers) {
    Rig r;
    r.f.writeRegister(1, 0x12);
    r.f.writeRegister(2, 0x34);
    r.f.writeRegister(3, 0x56);
    ASSERT_EQ(r.f.readRegister(1), 0x12);
    ASSERT_EQ(r.f.readRegister(2), 0x34);
    ASSERT_EQ(r.f.readRegister(3), 0x56);
    ASSERT_EQ(r.f.peekRegister(1), 0x12);
}

// Master reset: sector 1, then a Restore that ends with INTRQ on track 0
TEST_CASE(wd17xx_reset_restores) {
    Rig r;
    r.f.setDriveCylinder(0, 3);
    r.f.reset();
    ASSERT(r.f.busy());
    ASSERT_EQ(r.f.sectorRegister(), 1);
    r.waitIntrq();
    ASSERT(!r.f.busy());
    ASSERT_EQ(r.f.driveCylinder(0), 0);
    ASSERT_EQ(r.f.trackRegister(), 0);
    ASSERT(r.status() & Wd17xx::ST_TRACK0);
}

// Restore steps out at the step rate until track 0
TEST_CASE(wd17xx_restore_step_rate) {
    Rig r;
    r.f.setDriveCylinder(0, 10);
    r.f.writeRegister(0, 0x00);             // restore, 6 ms steps, no verify
    uint64_t t = r.waitIntrq();
    ASSERT_EQ(r.f.driveCylinder(0), 0);
    ASSERT_EQ(r.f.trackRegister(), 0);
    ASSERT(t >= 10 * 6000 && t <= 12 * 6000);
    uint8_t st = r.status();
    ASSERT(st & Wd17xx::ST_TRACK0);
    ASSERT(!(st & Wd17xx::ST_BUSY));
    ASSERT(!(st & Wd17xx::ST_SEEK));
    // The slowest rate (r1 r0 = 3): 30 ms a step
    r.f.setDriveCylinder(0, 2);
    r.f.writeRegister(1, 2);
    r.f.writeRegister(0, 0x03);
    t = r.waitIntrq();
    ASSERT(t >= 2 * 30000 && t <= 4 * 30000);
}

// Seek moves the head to the data register's track; with verify it finds the ID
TEST_CASE(wd17xx_seek_verify) {
    Rig r;
    r.f.writeRegister(3, 20);
    r.f.writeRegister(0, 0x10);
    r.waitIntrq();
    ASSERT_EQ(r.f.driveCylinder(0), 20);
    ASSERT_EQ(r.f.trackRegister(), 20);
    ASSERT(!(r.status() & Wd17xx::ST_TRACK0));
    r.f.writeRegister(3, 5);
    r.f.writeRegister(0, 0x1C);             // seek, h, V
    r.waitIntrq();
    uint8_t st = r.status();
    ASSERT_EQ(r.f.driveCylinder(0), 5);
    ASSERT(!(st & (Wd17xx::ST_SEEK | Wd17xx::ST_CRC)));
    ASSERT(st & Wd17xx::ST_HEADLOADED);
    // A track the disk doesn't have: seek error after five index pulses
    r.f.writeRegister(3, 38);
    r.f.writeRegister(0, 0x14);
    uint64_t t = r.waitIntrq();
    ASSERT(r.status() & Wd17xx::ST_SEEK);
    ASSERT(t >= 4 * REV);
    // Track register out of step with the head: verify fails too
    r.f.setDriveCylinder(0, 10);
    r.f.writeRegister(1, 12);
    r.f.writeRegister(3, 12);
    r.f.writeRegister(0, 0x14);
    r.waitIntrq();
    ASSERT(r.status() & Wd17xx::ST_SEEK);
}

// Step, Step In and Step Out; u updates the track register
TEST_CASE(wd17xx_step_commands) {
    Rig r;
    r.f.writeRegister(0, 0x50);             // step in, u
    r.waitIntrq();
    ASSERT_EQ(r.f.driveCylinder(0), 1);
    ASSERT_EQ(r.f.trackRegister(), 1);
    r.f.writeRegister(0, 0x40);             // step in, no update
    r.waitIntrq();
    ASSERT_EQ(r.f.driveCylinder(0), 2);
    ASSERT_EQ(r.f.trackRegister(), 1);
    r.f.writeRegister(0, 0x30);             // step (last direction: in), u
    r.waitIntrq();
    ASSERT_EQ(r.f.driveCylinder(0), 3);
    ASSERT_EQ(r.f.trackRegister(), 2);
    r.f.writeRegister(0, 0x70);             // step out, u
    r.waitIntrq();
    ASSERT_EQ(r.f.driveCylinder(0), 2);
    ASSERT_EQ(r.f.trackRegister(), 1);
    r.f.writeRegister(0, 0x20);             // step (out), no update
    r.waitIntrq();
    ASSERT_EQ(r.f.driveCylinder(0), 1);
    ASSERT_EQ(r.f.trackRegister(), 1);
}

// Type I status: index pulses each turn, write protect, head loaded, not ready
TEST_CASE(wd17xx_type1_status_bits) {
    Rig r;
    r.f.writeRegister(0, 0x08);             // restore with the head loaded
    r.waitIntrq();
    int pulses = 0;
    bool last = false;
    for (uint64_t i = 0; i < 2 * REV; i += 100) {
        bool idx = r.f.peekRegister(0) & Wd17xx::ST_INDEX;
        if (idx && !last) ++pulses;
        last = idx;
        r.run(100);
    }
    ASSERT_EQ(pulses, 2);
    ASSERT(r.status() & Wd17xx::ST_HEADLOADED);
    ASSERT(!(r.status() & Wd17xx::ST_WRITEPROTECT));
    r.f.setWriteProtect(0, true);
    ASSERT(r.status() & Wd17xx::ST_WRITEPROTECT);
    ASSERT(!(r.status() & Wd17xx::ST_NOTREADY));
    r.f.setMotor(false);
    ASSERT(r.status() & Wd17xx::ST_NOTREADY);
    r.f.setMotor(true);
    r.f.selectDrive(1);                     // empty
    ASSERT(r.status() & Wd17xx::ST_NOTREADY);
    // The head unloads after 15 idle turns
    r.f.selectDrive(0);
    r.run(16 * REV);
    ASSERT(!(r.status() & Wd17xx::ST_HEADLOADED));
}

// Read Sector: 256 bytes by DRQ, a byte every 32 us, then INTRQ
TEST_CASE(wd17xx_read_sector) {
    Rig r;
    r.f.setDriveCylinder(0, 7);
    r.f.writeRegister(1, 7);
    r.f.writeRegister(2, 5);
    std::vector<uint8_t> got;
    std::vector<uint64_t> drq;
    r.readCmd(0x80, got, 0, &drq);
    ASSERT_EQ(got.size(), 256u);
    for (int i = 0; i < 256; ++i) ASSERT_EQ(got[i], patternByte(7, 0, 5, i));
    uint8_t st = r.status();
    ASSERT_EQ(st & 0x7F, 0);                // no errors, not busy
    for (size_t i = 1; i < drq.size(); ++i) {
        uint64_t d = drq[i] - drq[i - 1];
        ASSERT(d >= 31 && d <= 33);
    }
}

// A sector that isn't there: record not found after five index pulses
TEST_CASE(wd17xx_record_not_found) {
    Rig r;
    r.f.writeRegister(1, 0);
    r.f.writeRegister(2, 30);
    std::vector<uint8_t> got;
    uint64_t t = r.readCmd(0x80, got);
    ASSERT(got.empty());
    ASSERT(r.status() & Wd17xx::ST_RNF);
    ASSERT(t >= 4 * REV && t <= 5 * REV + 1000);
    // The track register doesn't match the ID: also not found
    r.f.writeRegister(1, 3);
    r.f.writeRegister(2, 1);
    r.readCmd(0x80, got);
    ASSERT(r.status() & Wd17xx::ST_RNF);
}

// m: sector after sector until there are no more, then record not found
TEST_CASE(wd17xx_multi_sector_read) {
    Rig r;
    r.f.writeRegister(1, 0);
    r.f.writeRegister(2, 1);
    std::vector<uint8_t> got;
    r.readCmd(0x90, got);
    ASSERT_EQ(got.size(), 18u * 256);
    for (int s = 0; s < 18; ++s) ASSERT_EQ(got[s * 256 + 9], patternByte(0, 0, s + 1, 9));
    ASSERT(r.status() & Wd17xx::ST_RNF);
    ASSERT_EQ(r.f.sectorRegister(), 19);
}

// Write Sector then read it back, through the controller and from the image
TEST_CASE(wd17xx_write_sector) {
    Rig r;
    r.f.setDriveCylinder(0, 2);
    r.f.writeRegister(1, 2);
    r.f.writeRegister(2, 9);
    auto data = sectorData(0x5A);
    r.writeCmd(0xA0, data);
    ASSERT_EQ(r.status() & 0x7F, 0);
    std::vector<uint8_t> img;
    ASSERT(r.f.disk(0)->readSector(2, 0, 9, img));
    ASSERT(img == data);
    ASSERT(r.f.disk(0)->dirty());
    std::vector<uint8_t> got;
    r.readCmd(0x80, got);
    ASSERT(got == data);
    // Neighbours untouched
    ASSERT(r.f.disk(0)->readSector(2, 0, 10, img));
    ASSERT_EQ(img[0], patternByte(2, 0, 10, 0));
    // Multi-sector write: sectors 17 and 18, then not found
    std::vector<uint8_t> two = sectorData(1);
    auto b = sectorData(2);
    two.insert(two.end(), b.begin(), b.end());
    r.f.writeRegister(2, 17);
    r.writeCmd(0xB0, two);
    ASSERT(r.status() & Wd17xx::ST_RNF);
    ASSERT(r.f.disk(0)->readSector(2, 0, 18, img));
    ASSERT(img == b);
}

// A write-protected disk: write protect status, nothing written
TEST_CASE(wd17xx_write_protect) {
    Rig r;
    r.f.disk(0)->setWriteProtected(true);
    r.f.writeRegister(1, 0);
    r.f.writeRegister(2, 1);
    r.writeCmd(0xA0, sectorData(9));
    ASSERT(r.status() & Wd17xx::ST_WRITEPROTECT);
    std::vector<uint8_t> img;
    r.f.disk(0)->readSector(0, 0, 1, img);
    ASSERT_EQ(img[0], patternByte(0, 0, 1, 0));
    r.writeCmd(0xF0, {});
    ASSERT(r.status() & Wd17xx::ST_WRITEPROTECT);
}

// a0 writes the deleted data mark; reading reports the record type
TEST_CASE(wd17xx_deleted_data_mark) {
    Rig r;
    r.f.writeRegister(1, 0);
    r.f.writeRegister(2, 4);
    r.writeCmd(0xA1, sectorData(3));
    std::vector<uint8_t> got;
    r.readCmd(0x80, got);
    ASSERT(r.status() & Wd17xx::ST_RECTYPE);
    uint8_t dam = 0;
    std::vector<uint8_t> img;
    r.f.disk(0)->readSector(0, 0, 4, img, &dam);
    ASSERT_EQ(dam, 0xF8);
    r.f.writeRegister(2, 5);
    r.readCmd(0x80, got);
    ASSERT(!(r.status() & Wd17xx::ST_RECTYPE));
}

// Lost data: the CPU slower than a byte time
TEST_CASE(wd17xx_lost_data) {
    Rig r;
    r.f.writeRegister(1, 0);
    r.f.writeRegister(2, 2);
    std::vector<uint8_t> got;
    r.readCmd(0x80, got, 20);               // in time
    ASSERT(!(r.status() & Wd17xx::ST_LOST));
    ASSERT_EQ(got.size(), 256u);
    got.clear();
    r.readCmd(0x80, got, 40);               // too slow: lost, but the command runs on
    uint8_t st = r.status();
    ASSERT(st & Wd17xx::ST_LOST);
    ASSERT(!(st & Wd17xx::ST_BUSY));
    // Write: no first byte in time ends the command at once
    r.writeCmd(0xA0, {}, 1000000);
    st = r.status();
    ASSERT(st & Wd17xx::ST_LOST);
    // Write: later bytes late are written as zeros
    r.f.writeRegister(2, 3);
    r.writeCmd(0xA0, sectorData(7), 40);
    ASSERT(r.status() & Wd17xx::ST_LOST);
    std::vector<uint8_t> img;
    r.f.disk(0)->readSector(0, 0, 3, img);
    int zeros = 0;
    for (uint8_t b : img) if (b == 0) ++zeros;
    ASSERT(zeros > 100);
}

// Read Address: the next ID's six bytes; its track goes to the sector register
TEST_CASE(wd17xx_read_address) {
    Rig r;
    r.f.setDriveCylinder(0, 12);
    r.f.writeRegister(1, 0);
    std::vector<uint8_t> got;
    r.readCmd(0xC0, got);
    ASSERT_EQ(got.size(), 6u);
    ASSERT_EQ(got[0], 12);
    ASSERT_EQ(got[1], 0);
    ASSERT(got[2] >= 1 && got[2] <= 18);
    ASSERT_EQ(got[3], 1);
    ASSERT_EQ(r.f.sectorRegister(), 12);
    ASSERT_EQ(r.status() & 0x7F, 0);
    // Wrong density: no IDs to be found
    r.f.setDoubleDensity(false);
    got.clear();
    r.readCmd(0xC0, got);
    ASSERT(got.empty());
    ASSERT(r.status() & Wd17xx::ST_RNF);
}

// Write Track formats from the raw stream ($F5 $F6 $F7 $FE); then read it back
TEST_CASE(wd17xx_write_track_mfm) {
    Rig r(Wd17xx::Variant::WD2793, std::make_unique<FloppyDisk>(35, 1));   // unformatted
    r.f.setDriveCylinder(0, 4);
    r.f.writeRegister(1, 4);
    auto stream = mfmFormatStream(4, 0, 18, 0x40);
    r.writeCmd(0xF4, stream, 0, 0x4E, nullptr, true);
    ASSERT_EQ(r.status() & 0x7F, 0);
    auto secs = r.f.disk(0)->scanTrack(4, 0);
    ASSERT_EQ(secs.size(), 18u);
    for (auto& s : secs) {
        ASSERT(s.idCrcOk && s.hasData && s.dataCrcOk && s.dd);
        ASSERT_EQ(s.track, 4);
    }
    for (int sec = 1; sec <= 18; ++sec) {
        r.f.writeRegister(2, (uint8_t)sec);
        std::vector<uint8_t> got;
        r.readCmd(0x80, got);
        ASSERT_EQ(got.size(), 256u);
        ASSERT_EQ(got[100], (uint8_t)(0x40 + sec));
        ASSERT_EQ(r.status() & 0x7F, 0);
    }
    // Write Track on a cylinder past the image's end extends it
    r.f.setDriveCylinder(0, 36);
    r.f.writeRegister(1, 36);
    r.writeCmd(0xF0, mfmFormatStream(36, 0, 18, 0), 0, 0x4E, nullptr, true);
    ASSERT_EQ(r.f.disk(0)->cylinders(), 37);
    ASSERT_EQ(r.f.disk(0)->scanTrack(36, 0).size(), 18u);
}

// Write Track not given its first byte within 3 byte times: lost data
TEST_CASE(wd17xx_write_track_lost) {
    Rig r;
    r.writeCmd(0xF0, {}, 200);
    ASSERT(r.status() & Wd17xx::ST_LOST);
}

// Read Track: index to index, every byte of the turn
TEST_CASE(wd17xx_read_track) {
    Rig r;
    std::vector<uint8_t> got;
    r.readCmd(0xE0, got);
    ASSERT(got.size() >= (size_t)FloppyDisk::TRACK_LEN_DD - 2 && got.size() <= (size_t)FloppyDisk::TRACK_LEN_DD);
    ASSERT_EQ(got[0], 0x4E);
    int ids = 0;
    for (size_t i = 3; i < got.size(); ++i)
        if (got[i] == 0xFE && got[i - 1] == 0xA1 && got[i - 2] == 0xA1 && got[i - 3] == 0xA1) ++ids;
    ASSERT_EQ(ids, 18);
}

// Force Interrupt: $D0 stops without INTRQ, $D8 at once (held), $D4 every index
TEST_CASE(wd17xx_force_interrupt) {
    Rig r;
    r.f.writeRegister(1, 0);
    r.f.writeRegister(2, 30);               // a long search
    r.f.writeRegister(0, 0x80);
    r.run(1000);
    ASSERT(r.f.busy());
    r.f.writeRegister(0, 0xD0);
    ASSERT(!r.f.busy());
    ASSERT(!r.f.intrq());
    r.run(6 * REV);
    ASSERT(!r.f.intrq());
    // A Type II status until a Force Interrupt while idle turns it back to Type I
    ASSERT(!(r.status() & Wd17xx::ST_TRACK0));
    r.f.writeRegister(0, 0xD0);
    ASSERT(r.status() & Wd17xx::ST_TRACK0);
    // Immediate: status reads don't clear it; another $D0 does
    r.f.writeRegister(0, 0xD8);
    ASSERT(r.f.intrq());
    r.status();
    ASSERT(r.f.intrq());
    r.f.writeRegister(0, 0xD0);
    ASSERT(!r.f.intrq());
    // Every index pulse
    r.f.writeRegister(0, 0xD4);
    int irqs = 0;
    for (uint64_t i = 0; i < 3 * REV; i += 50) {
        r.run(50);
        if (r.f.intrq()) { ++irqs; r.status(); }
    }
    ASSERT(irqs >= 2 && irqs <= 4);
    r.f.writeRegister(0, 0xD0);
    // Not ready to ready ($D1)
    r.f.writeRegister(0, 0xD1);
    r.f.setMotor(false);
    ASSERT(!r.f.intrq());
    r.f.setMotor(true);
    ASSERT(r.f.intrq());
}

// Type II/III on a drive that isn't ready: INTRQ at once, not ready
TEST_CASE(wd17xx_not_ready) {
    Rig r(Wd17xx::Variant::WD2793, nullptr);
    r.f.writeRegister(0, 0x80);
    ASSERT(r.f.intrq());
    ASSERT(!r.f.busy());
    ASSERT(r.status() & Wd17xx::ST_NOTREADY);
}

// E: 30 ms of head settling before the search
TEST_CASE(wd17xx_head_settle_delay) {
    Rig r;
    r.f.writeRegister(1, 0);
    r.f.writeRegister(2, 1);
    std::vector<uint64_t> drq;
    std::vector<uint8_t> got;
    r.readCmd(0x84, got, 0, &drq);
    ASSERT(!drq.empty() && drq[0] >= 30000);
}

// INTRQ and DRQ as signal lines and callbacks
TEST_CASE(wd17xx_signal_outputs) {
    Rig r;
    Line irq, drqLine;
    int drqEdges = 0, irqEdges = 0;
    r.f.setIrqLine(&irq);
    r.f.setSignalLine("drq", &drqLine);
    r.f.onDrq([&](bool on) { if (on) ++drqEdges; });
    r.f.onIntrq([&](bool on) { if (on) ++irqEdges; });
    r.f.writeRegister(1, 0);
    r.f.writeRegister(2, 1);
    std::vector<uint8_t> got;
    r.readCmd(0x80, got);
    ASSERT_EQ(drqEdges, 256);
    ASSERT_EQ(irqEdges, 1);
    ASSERT(irq.level);
    ASSERT(!drqLine.level);
    r.status();
    ASSERT(!irq.level);
}

// 1791/1793 side compare (S, C); 1797 U drives the side select output
TEST_CASE(wd17xx_sides) {
    Rig r(Wd17xx::Variant::FD1793, makeDisk(10, 2));
    r.f.writeRegister(1, 0);
    r.f.writeRegister(2, 3);
    r.f.setSide(1);
    std::vector<uint8_t> got;
    r.readCmd(0x8A, got);                   // compare side 1
    ASSERT_EQ(got.size(), 256u);
    ASSERT_EQ(got[0], patternByte(0, 1, 3, 0));
    got.clear();
    r.readCmd(0x82, got);                   // compare side 0: the IDs say 1
    ASSERT(got.empty());
    ASSERT(r.status() & Wd17xx::ST_RNF);
    got.clear();
    r.readCmd(0x80, got);                   // no compare
    ASSERT_EQ(got.size(), 256u);

    Rig q(Wd17xx::Variant::FD1797, makeDisk(10, 2));
    q.f.writeRegister(1, 0);
    q.f.writeRegister(2, 3);
    q.f.setSide(0);                         // ignored: the SSO picks the side
    got.clear();
    q.readCmd(0x8A, got);                   // L=1, U=1
    ASSERT_EQ(q.f.sideSelectOutput(), 1);
    ASSERT_EQ(got.size(), 256u);
    ASSERT_EQ(got[0], patternByte(0, 1, 3, 0));
    // L = 0: length code 1 means 512 bytes on the 1795/1797; it runs into the next sector
    got.clear();
    q.readCmd(0x80, got);
    ASSERT_EQ(q.f.sideSelectOutput(), 0);
    ASSERT_EQ(got.size(), 512u);
    ASSERT(q.status() & Wd17xx::ST_CRC);
}

// 1791/1795/2791/2795: every register through an inverted bus
TEST_CASE(wd17xx_inverted_bus) {
    Rig r(Wd17xx::Variant::FD1791);
    r.f.writeRegister(1, 0x05);
    ASSERT_EQ(r.f.trackRegister(), 0xFA);
    ASSERT_EQ(r.f.readRegister(1), 0x05);
    uint8_t bus = 0;
    r.f.ioWrite(nullptr, 0xFF4A, 0x0F);
    ASSERT_EQ(r.f.sectorRegister(), 0xF0);
    r.f.ioRead(nullptr, 0xFF4A, &bus);
    ASSERT_EQ(bus, 0x0F);
    ASSERT(!r.f.ioRead(nullptr, 0xFF4C, &bus));
    // A command goes through it too: ~$80 is Read Sector
    r.f.writeRegister(1, (uint8_t)~0);
    r.f.writeRegister(2, (uint8_t)~1);
    std::vector<uint8_t> got;
    r.readCmd((uint8_t)~0x80, got);
    ASSERT_EQ(got.size(), 256u);
    ASSERT_EQ(got[0], (uint8_t)~patternByte(0, 0, 1, 0));
}

// FD1771: single density only, the data mark in status bits 6-5, b lengths
TEST_CASE(wd17xx_fd1771) {
    Rig r(Wd17xx::Variant::FD1771, makeDisk(20, 1, 10, false, 0));
    r.f.setDoubleDensity(true);             // no DDEN on the 1771
    r.f.writeRegister(1, 0);
    r.f.writeRegister(2, 4);
    std::vector<uint8_t> got;
    std::vector<uint64_t> drq;
    r.readCmd(0x88, got, 0, &drq);          // b = 1: IBM lengths
    ASSERT_EQ(got.size(), 256u);
    ASSERT_EQ(got[1], patternByte(0, 0, 4, 1));
    ASSERT_EQ(r.status() & 0x7F, 0);
    for (size_t i = 1; i < drq.size(); ++i) {
        uint64_t d = drq[i] - drq[i - 1];
        ASSERT(d >= 63 && d <= 65);          // 64 us FM bytes
    }
    // b = 0: 16 x the length byte (1 -> 16 bytes)
    got.clear();
    r.readCmd(0x80, got);
    ASSERT_EQ(got.size(), 16u);
    // Write with a1 a0 = 01: the $FA mark, read back as record type 01
    r.f.writeRegister(2, 5);
    r.writeCmd(0xA9, sectorData(4));
    uint8_t dam = 0;
    std::vector<uint8_t> img;
    ASSERT(r.f.disk(0)->readSector(0, 0, 5, img, &dam));
    ASSERT_EQ(dam, 0xFA);
    got.clear();
    r.readCmd(0x88, got);
    ASSERT_EQ((r.status() >> 5) & 3, 1);
    // $F8 (deleted) is 11
    r.f.writeRegister(2, 6);
    r.writeCmd(0xAB, sectorData(4));
    got.clear();
    r.readCmd(0x88, got);
    ASSERT_EQ((r.status() >> 5) & 3, 3);
    // Write Track in FM
    r.f.setDriveCylinder(0, 3);
    r.f.writeRegister(1, 3);
    r.writeCmd(0xF4, fmFormatStream(3, 10, 0, 0x20), 0, 0xFF, nullptr, true);
    auto secs = r.f.disk(0)->scanTrack(3, 0);
    ASSERT_EQ(secs.size(), 10u);
    for (auto& s : secs) ASSERT(s.idCrcOk && s.dataCrcOk && !s.dd);
    r.f.writeRegister(2, 7);
    got.clear();
    r.readCmd(0x88, got);
    ASSERT_EQ(got.size(), 256u);
    ASSERT_EQ(got[0], 0x27);
}

// A CRC error in the data field
TEST_CASE(wd17xx_crc_error) {
    auto d = makeDisk(2);
    FloppySectorSpec bad;
    bad.track = 1; bad.sector = 1; bad.badCrc = true;
    bad.data.assign(256, 0x11);
    d->formatTrack(1, 0, {bad});
    Rig r(Wd17xx::Variant::WD2793, std::move(d));
    r.f.setDriveCylinder(0, 1);
    r.f.writeRegister(1, 1);
    r.f.writeRegister(2, 1);
    std::vector<uint8_t> got;
    r.readCmd(0x90, got);                   // multi: a CRC error ends it
    ASSERT_EQ(got.size(), 256u);
    ASSERT(r.status() & Wd17xx::ST_CRC);
    ASSERT_EQ(r.f.sectorRegister(), 1);
}

// The drive's motor off: no index pulses, not ready
TEST_CASE(wd17xx_motor_and_drives) {
    Rig r;
    r.f.insertDisk(2, makeDisk(5, 1, 18));
    r.f.selectDrive(2);
    r.f.writeRegister(1, 0);
    r.f.writeRegister(2, 2);
    std::vector<uint8_t> got;
    r.readCmd(0x80, got);
    ASSERT_EQ(got.size(), 256u);
    // The heads of each drive are separate
    r.f.writeRegister(3, 3);
    r.f.writeRegister(0, 0x10);
    r.waitIntrq();
    ASSERT_EQ(r.f.driveCylinder(2), 3);
    ASSERT_EQ(r.f.driveCylinder(0), 0);
    r.f.setReadyMode(Wd17xx::ReadyMode::Always);
    r.f.selectDrive(3);
    ASSERT(!(r.status() & Wd17xx::ST_NOTREADY));
    r.f.setReadyMode(Wd17xx::ReadyMode::DiskAndMotor);
    ASSERT(r.status() & Wd17xx::ST_NOTREADY);
    int track = 0, sector = 0;
    bool led = false;
    ASSERT(r.f.getDiskStatus(2, track, sector, led));
    ASSERT_EQ(track, 3);
    ASSERT(!r.f.getDiskStatus(3, track, sector, led));
}

// Variant names and options
TEST_CASE(wd17xx_variants_and_options) {
    Wd17xx::Variant v;
    ASSERT(Wd17xx::parseVariant("wd1793", v) && v == Wd17xx::Variant::FD1793);
    ASSERT(Wd17xx::parseVariant("FD1771", v) && v == Wd17xx::Variant::FD1771);
    ASSERT(Wd17xx::parseVariant("2797", v) && v == Wd17xx::Variant::WD2797);
    ASSERT(!Wd17xx::parseVariant("8272", v));
    Wd17xx f;
    std::string err;
    ASSERT(f.setOption("variant", "fd1795", err));
    ASSERT(f.variant() == Wd17xx::Variant::FD1795);
    ASSERT_EQ(std::string(f.name()), "FD1795");
    ASSERT(f.setOption("base", "0x37EC", err));
    ASSERT_EQ(f.baseAddr(), 0x37ECu);
    ASSERT(!f.setOption("drive0", "/nonexistent/disk.dsk", err));
    ASSERT(!f.setOption("colour", "red", err));
    DeviceInfo info;
    f.getDeviceInfo(info);
    ASSERT_EQ(info.registers.size(), 4u);
}
