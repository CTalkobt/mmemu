#include "drive1541.h"
#include "gcr.h"
#include "plugins/6502/main/cpu6502.h"
#include "plugins/devices/via6522/main/via6522.h"
#include "libdevices/main/shared_irq_manager.h"
#include "libdevices/main/disk_file.h"
#include <algorithm>
#include <cstring>
#include <fstream>

// ---------------------------------------------------------------------------
// The drive's bus (VICE testprogs drive/openbus): below $8000, A11 and A12
// low select the RAM ($0000-$07FF, again at $2000, $4000, $6000), both high
// a VIA (A10: VIA1 $1800, VIA2 $1C00, 16 registers mirrored, again at $3800,
// $5800, $7800); the rest is open, reading the bus's last byte. The ROM is
// $C000-$FFFF, mirrored at $8000.
// ---------------------------------------------------------------------------

class Drive1541::Bus : public IBus {
public:
    explicit Bus(Drive1541* d) : m_d(d) {
        m_cfg.addrBits = 16; m_cfg.dataBits = 8; m_cfg.littleEndian = true; m_cfg.addrMask = 0xFFFF;
    }
    // No snapshots or write log of its own
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}
    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "drive"; }
    uint8_t read8(uint32_t a) override { return m_last = access(a & 0xFFFF, false); }
    uint8_t peek8(uint32_t a) override { return access(a & 0xFFFF, true); }
    void write8(uint32_t a, uint8_t v) override {
        a &= 0xFFFF;
        m_last = v;
        if (a >= 0x8000) return;
        if ((a & 0x1800) == 0) { m_d->m_ram[a & 0x7FF] = v; return; }
        if ((a & 0x1800) != 0x1800) return;
        if (a & 0x0400) m_d->m_via2->ioWrite(this, 0x1C00 | (a & 0xF), v);
        else            m_d->m_via1->ioWrite(this, 0x1800 | (a & 0xF), v);
    }
    // Only the ROM: the CPU's dummy reads of RAM put its bytes on the bus,
    // and an open read gets them (testprogs drive/openbus: LDA $07FF,X)
    bool isPlainMemory(uint32_t a) const override { return (a & 0xFFFF) >= 0x8000; }
private:
    uint8_t access(uint32_t a, bool peek) {
        if (a >= 0x8000) return m_d->m_rom[a & 0x3FFF];
        if ((a & 0x1800) == 0) return m_d->m_ram[a & 0x7FF];
        uint8_t v = m_last;                             // open bus: the last byte on it
        if ((a & 0x1800) == 0x1800) {
            VIA6522* via = (a & 0x0400) ? m_d->m_via2.get() : m_d->m_via1.get();
            const uint32_t r = ((a & 0x0400) ? 0x1C00 : 0x1800) | (a & 0xF);
            if (peek) via->ioPeek(this, r, &v);
            else      via->ioRead(this, r, &v);
        }
        return v;
    }
    Drive1541* m_d;
    BusConfig m_cfg{};
    uint8_t m_last = 0xFF;          // the data bus's last value (open bus)
};

IBus* Drive1541::bus() const { return m_bus.get(); }

// VIA1 port B: the serial bus
class Drive1541::Via1Port : public IPortDevice {
public:
    explicit Via1Port(Drive1541* d) : m_d(d) {}
    uint8_t readPort() override { return m_d->via1Pins(); }
    void writePort(uint8_t v) override { m_orb = v; update(); }
    void setDdr(uint8_t v) override { m_ddr = v; update(); }
private:
    void update() {
        // An input bit floats high, so its 7406 pulls the line (as a 1 does)
        const uint8_t out = (uint8_t)((m_orb & m_ddr) | ~m_ddr);
        m_d->driveLines(out & 0x08, out & 0x02, out & 0x10);
    }
    Drive1541* m_d;
    uint8_t m_orb = 0, m_ddr = 0;
};

// VIA2 port A: the byte the head read
class Drive1541::Via2PortA : public IPortDevice {
public:
    explicit Via2PortA(Drive1541* d) : m_d(d) {}
    uint8_t readPort() override { return m_d->m_readByte; }
    void writePort(uint8_t v) override { out = v; }
    void setDdr(uint8_t) override {}
    uint8_t out = 0x55;            // what a write puts on the disk
private:
    Drive1541* m_d;
};

// VIA2 port B: stepper, motor, LED, zone out; write protect, SYNC in
class Drive1541::Via2PortB : public IPortDevice {
public:
    explicit Via2PortB(Drive1541* d) : m_d(d) {}
    uint8_t readPort() override {
        uint8_t v = 0x6F;
        if (!m_d->m_writeProtect) v |= 0x10;
        if (!m_d->m_sync) v |= 0x80;
        return v;
    }
    void writePort(uint8_t v) override { orb = v; m_d->stepper((uint8_t)(effective() & 3)); }
    void setDdr(uint8_t v) override { ddr = v; m_d->stepper((uint8_t)(effective() & 3)); }
    uint8_t effective() const { return (uint8_t)((orb & ddr) | ~ddr); }
    uint8_t orb = 0, ddr = 0;
private:
    Drive1541* m_d;
};

// ---------------------------------------------------------------------------

Drive1541::Drive1541(int unit) : m_unit(unit) {
    m_name = "Drive" + std::to_string(unit);
    m_cpu = std::make_unique<MOS6502>();
    m_bus = std::make_unique<Bus>(this);
    m_via1 = std::make_unique<VIA6522>("VIA1", 0x1800);
    m_via2 = std::make_unique<VIA6522>("VIA2", 0x1C00);
    m_irq = std::make_unique<SharedIrqManager>(m_cpu.get());
    m_via1Port = std::make_unique<Via1Port>(this);
    m_via2A = std::make_unique<Via2PortA>(this);
    m_via2B = std::make_unique<Via2PortB>(this);
    m_cpu->setDataBus(m_bus.get());
    m_cpu->setCodeBus(m_bus.get());
    m_via1->setPortBDevice(m_via1Port.get());
    m_via2->setPortADevice(m_via2A.get());
    m_via2->setPortBDevice(m_via2B.get());
    m_via2->setCA2Line(&m_soe);
    m_via2->setCB2Line(&m_mode);
    m_via1->setIrqLine(m_irq->createLine());
    m_via2->setIrqLine(m_irq->createLine());
    // Each access brings the VIAs and the head to its cycle
    m_cpu->setAccessSync([this](int k) -> uint32_t {
        if (k >= 0) runDevicesTo(m_instrStart + (uint64_t)k + 1);
        return 0;
    });
    m_tracks.assign(84, {});
    m_dirty.assign(84, false);
    loadRom("roms/c64/dos1541.bin");
}

Drive1541::~Drive1541() { flushTrack(); }

bool Drive1541::loadRom(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.read((char*)m_rom, sizeof m_rom);
    m_romLoaded = f.gcount() == (std::streamsize)sizeof m_rom;
    return m_romLoaded;
}

void Drive1541::reset() {
    std::memset(m_ram, 0, sizeof m_ram);
    m_via1->reset();
    m_via2->reset();
    m_cpu->reset();
    m_clock = m_devClock = m_target = 0;
    m_hostCycles = 0;
    m_frac = 0;
    m_hostEvents.clear();
    m_driveEvents.clear();
    m_hostSeen = m_host;
    m_driveSeen = m_drive;
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

void Drive1541::tick(uint64_t cycles) {
    if (!m_romLoaded) return;
    m_hostCycles += cycles;
    m_frac += cycles * 1000000ULL;
    m_target += m_frac / m_hostHz;
    m_frac %= m_hostHz;
    // The drive stays up to an instruction behind; the computer's accesses
    // to the port bring it up to their time
    while (m_clock + 8 <= m_target) {
        m_instrStart = m_clock;
        const int n = m_cpu->step();
        m_clock = m_instrStart + (uint64_t)(n > 0 ? n : 1);
        runDevicesTo(m_clock);
    }
}

void Drive1541::catchUp() {
    while (m_clock < m_target) {
        m_instrStart = m_clock;
        const int n = m_cpu->step();
        m_clock = m_instrStart + (uint64_t)(n > 0 ? n : 1);
        runDevicesTo(m_clock);
    }
}

void Drive1541::runDevicesTo(uint64_t t) {
    while (m_devClock < t) {
        // The computer's line changes up to this cycle; ATN to VIA1 CA1
        // (seen by this cycle's access if they came before the cycle began:
        // the VIA samples its port early in the cycle)
        while (!m_hostEvents.empty() && m_hostEvents.front().first <= driveAt(m_devClock)) {
            m_hostSeen = m_hostEvents.front().second;
            m_hostEvents.pop_front();
        }
        if (m_hostSeen.atn != m_atnSeen) {
            m_atnSeen = m_hostSeen.atn;
            if (ISignalLine* ca1 = m_via1->getSignalLine("ca1")) ca1->set(m_atnSeen);
        }
        m_via1->tick(1);
        m_via2->tick(1);
        diskCycle();
        ++m_devClock;
    }
}

// ---------------------------------------------------------------------------
// The serial bus
// ---------------------------------------------------------------------------

void Drive1541::hostLines() {
    // Bits 3-5 drive ATN, CLK and DATA through 7406s; an input bit floats
    // high and so pulls its line too
    const uint8_t out = (uint8_t)((m_hostPra & m_hostDdr) | ~m_hostDdr);
    HostLines h;
    h.atn = out & 0x08;
    h.clk = out & 0x10;
    h.data = out & 0x20;
    m_host = h;
    // The drive sees it from now (at once if it is already past)
    if (driveAt(m_devClock) >= hostNow()) {
        m_hostEvents.clear();
        m_hostSeen = h;
    } else {
        m_hostEvents.emplace_back(hostNow(), h);
    }
    catchUp();
}

void Drive1541::driveLines(bool clk, bool data, bool atna) {
    DriveLines d;
    d.clk = clk; d.data = data; d.atna = atna;
    m_drive = d;
    // The computer sees it from this cycle on
    // (the access is in the cycle the devices were brought through)
    if (driveAt(m_devClock) <= hostNow()) { m_driveEvents.clear(); m_driveSeen = d; }
    else m_driveEvents.emplace_back(driveAt(m_devClock), d);
}

uint8_t Drive1541::via1Pins() const {
    // The bus as the drive sees it now: the computer's lines as they were at
    // this cycle, its own as they are. DATA is also held low while ATN and
    // ATNA differ (the ATN acknowledge)
    const bool atn = m_hostSeen.atn;
    const bool clk = m_hostSeen.clk || m_drive.clk;
    const bool data = m_hostSeen.data || m_drive.data || (atn != m_drive.atna);
    uint8_t v = 0;
    if (data) v |= 0x01;
    if (clk) v |= 0x04;
    v |= (uint8_t)(((m_unit - 8) & 3) << 5);
    if (atn) v |= 0x80;
    return (uint8_t)(v | 0x1A);   // the output bits read back from ORB
}

uint8_t Drive1541::hostView() const {
    const bool clk = m_host.clk || m_driveSeen.clk;
    const bool data = m_host.data || m_driveSeen.data || (m_host.atn != m_driveSeen.atna);
    uint8_t v = 0x3F;
    if (!clk) v |= 0x40;
    if (!data) v |= 0x80;
    return v;
}

uint8_t Drive1541::readPort() {
    // The drive up to now, and its changes up to now
    catchUp();
    while (!m_driveEvents.empty() && m_driveEvents.front().first <= hostNow()) {
        m_driveSeen = m_driveEvents.front().second;
        m_driveEvents.pop_front();
    }
    return hostView();
}

// ---------------------------------------------------------------------------
// The disk
// ---------------------------------------------------------------------------

void Drive1541::stepper(uint8_t phase) {
    // The coil's phase; the head follows it once it has been on long enough
    if (phase == m_coil) return;
    m_coil = phase;
    m_coilSince = m_devClock;
}

void Drive1541::headMotion() {
    // The head has inertia (VICE testprogs drive/inertia): it moves a half
    // track toward the coil only once the coil's phase has held for
    // STEP_CYCLES, so a phase held for less (a step and back, or round all
    // four phases) leaves it where it was. A coil two phases away doesn't
    // pull it either way.
    if (m_coil == m_phase || m_devClock - m_coilSince < STEP_CYCLES) return;
    const uint8_t d = (uint8_t)((m_coil - m_phase) & 3);
    if (d == 2) return;
    m_phase = (uint8_t)((m_phase + (d == 1 ? 1 : 3)) & 3);
    m_coilSince = m_devClock;                              // the next half track takes as long
    const int ht = std::clamp(m_halfTrack + (d == 1 ? 1 : -1), 0, 83);
    if (ht == m_halfTrack) return;
    flushTrack();
    const size_t from = trackSize(m_halfTrack);
    m_halfTrack = ht;
    // The disk turns on under the head: the same angle on the new track
    m_pos = m_pos * trackSize(ht) / from;
    loadTrack();
}

size_t Drive1541::trackSize(int halfTrack) const {
    const auto& t = m_tracks[(size_t)halfTrack];
    return t.empty() ? (size_t)gcr::trackBytes(halfTrack / 2 + 1) : t.size();
}

void Drive1541::loadTrack() {
    m_pos %= trackSize(m_halfTrack) * 8;
}

void Drive1541::diskCycle() {
    headMotion();
    const uint8_t pb = m_via2B->effective();
    if (!(pb & 0x04)) return;                              // motor off
    // The disk turns at 300 rpm, a turn every 200,000 cycles, whatever the
    // speed zone: the head passes the track's 8 * size bit cells in that time
    const uint32_t bits = (uint32_t)trackSize(m_halfTrack) * 8;
    m_rot += bits;
    if (m_rot < ROTATION_CYCLES) return;
    m_rot -= ROTATION_CYCLES;
    m_pos = (m_pos + 1) % bits;
    auto& t = m_tracks[(size_t)m_halfTrack];
    const bool writing = !m_mode.level;
    uint8_t& cell = t.empty() ? m_noCell : t[m_pos >> 3];
    const uint8_t mask = (uint8_t)(0x80 >> (m_pos & 7));
    if (writing) {
        // A bit of the byte from PA into the cell; the next byte at each byte
        if (!m_writeProtect && !t.empty()) {
            cell = (m_writeShift & 0x80) ? (uint8_t)(cell | mask) : (uint8_t)(cell & ~mask);
            m_dirty[(size_t)m_halfTrack] = true;
        }
        m_writeShift = (uint8_t)(m_writeShift << 1);
        m_readShift = 0;
        m_sync = false;
        if (++m_bitCount < 8) return;
        m_bitCount = 0;
        m_writeShift = m_via2A->out;
    } else {
        // Ten 1 bits in a row are a SYNC, and restart the byte; a byte is
        // ready every 8 bits after one
        m_readShift = (uint16_t)(((m_readShift << 1) | ((cell & mask) ? 1 : 0)) & 0x3FF);
        if (m_readShift == 0x3FF) { m_sync = true; m_bitCount = 0; return; }
        m_sync = false;
        if (++m_bitCount < 8) return;
        m_bitCount = 0;
        m_readByte = (uint8_t)m_readShift;
    }
    // Byte ready: VIA2 CA1, and V through SO while CA2 enables it
    if (ISignalLine* ca1 = m_via2->getSignalLine("ca1")) { ca1->set(false); ca1->set(true); }
    if (m_soe.level) m_cpu->setOverflow();
}

bool Drive1541::mountDisk(int unit, const std::string& path) {
    if (unit != m_unit) return false;
    std::vector<uint8_t> data;
    if (!diskfile::read(path, data)) return false;
    ejectDisk(unit);
    const std::string ext = diskfile::imageExt(path);
    m_tracks.assign(84, {});
    m_dirty.assign(84, false);
    if (ext == "g64") {
        // "GCR-1541", version, half-track count, max track size, then the
        // offset and speed tables
        if (data.size() < 12 || std::memcmp(data.data(), "GCR-1541", 8) != 0) return false;
        const int halves = std::min<int>(data[9], 84);
        for (int h = 0; h < halves; ++h) {
            const size_t o = 12 + (size_t)h * 4;
            if (o + 4 > data.size()) break;
            const uint32_t off = data[o] | (data[o + 1] << 8) | (data[o + 2] << 16) | ((uint32_t)data[o + 3] << 24);
            if (!off || off + 2 > data.size()) continue;
            const size_t len = data[off] | (data[off + 1] << 8);
            if (off + 2 + len > data.size()) continue;
            m_tracks[(size_t)h].assign(data.begin() + off + 2, data.begin() + off + 2 + len);
        }
        m_isD64 = false;
    } else {
        // D64: 35 or 40 (or 42) tracks, error bytes after the sectors if there
        int tracks = 0;
        size_t sectors = 0;
        for (int t = 35; t <= 42; ++t) {
            size_t s = 0;
            for (int k = 1; k <= t; ++k) s += (size_t)gcr::sectorsOnTrack(k);
            if (data.size() == s * 256 || data.size() == s * 257) { tracks = t; sectors = s; break; }
        }
        if (!tracks) return false;
        m_d64Tracks = tracks;
        m_d64Errors = data.size() == sectors * 257;
        const size_t bam = 357 * 256;                       // track 18 sector 0
        const uint8_t id1 = data[bam + 0xA2], id2 = data[bam + 0xA3];
        size_t at = 0, skew = 0;
        for (int t = 1; t <= tracks; ++t) {
            const int n = gcr::sectorsOnTrack(t);
            const uint8_t* err = m_d64Errors ? &data[sectors * 256 + at / 256] : nullptr;
            skew = gcr::trackSkew(t, skew);
            m_tracks[(size_t)(t - 1) * 2] = gcr::encodeTrack(t, &data[at], id1, id2, err, skew);
            at += (size_t)n * 256;
        }
        // Tracks past the image's are unformatted ($55, as VICE's)
        for (int t = tracks + 1; t <= 42; ++t)
            m_tracks[(size_t)(t - 1) * 2].assign((size_t)gcr::trackBytes(t), 0x55);
        m_image = std::move(data);
        m_isD64 = true;
    }
    // Half-tracks with nothing on them have no flux: zeros (they can be written)
    for (int h = 0; h < 84; ++h)
        if (m_tracks[(size_t)h].empty()) m_tracks[(size_t)h].assign((size_t)gcr::trackBytes(h / 2 + 1), 0x00);
    m_path = path;
    m_writeProtect = !diskfile::writable(path);
    loadTrack();
    return true;
}

void Drive1541::flushTrack() {
    // A changed track of a D64 decoded back into its sectors and the file
    if (!m_isD64 || m_path.empty()) return;
    const int h = m_halfTrack;
    if (!m_dirty[(size_t)h] || (h & 1)) return;
    m_dirty[(size_t)h] = false;
    const int track = h / 2 + 1;
    if (track > m_d64Tracks) return;
    std::vector<uint8_t> out;
    std::vector<bool> found;
    gcr::decodeTrack(track, m_tracks[(size_t)h], out, found);
    size_t at = 0;
    for (int t = 1; t < track; ++t) at += (size_t)gcr::sectorsOnTrack(t) * 256;
    for (size_t s = 0; s < found.size(); ++s)
        if (found[s]) std::memcpy(&m_image[at + s * 256], &out[s * 256], 256);
    diskfile::patch(m_path, m_image, at, found.size() * 256);
}

void Drive1541::ejectDisk(int unit) {
    if (unit != m_unit) return;
    flushTrack();
    m_tracks.assign(84, {});
    m_dirty.assign(84, false);
    m_image.clear();
    m_path.clear();
    m_isD64 = false;
}

bool Drive1541::getDiskStatus(int unit, int& track, int& sector, bool& led) {
    if (unit != m_unit) return false;
    track = m_halfTrack / 2 + 1;
    sector = 0;
    led = (m_via2B->effective() & 0x08) != 0;
    return true;
}

void Drive1541::getDeviceInfo(DeviceInfo& out) const {
    out.name = m_name;
    out.state.push_back({"Type", "1541 (true drive)"});
    out.state.push_back({"PC", std::to_string(m_cpu->pc())});
    out.state.push_back({"Track", std::to_string(m_halfTrack / 2 + 1) + ((m_halfTrack & 1) ? ".5" : "")});
    out.state.push_back({"Disk", m_path.empty() ? "none" : m_path});
}
