#include "wd17xx.h"

#include "libdevices/main/isignal_line.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace {
__extension__ typedef unsigned __int128 u128;

// Step rates in ms at a 1 MHz controller clock (r1 r0)
const int STEP_179X[4] = {6, 12, 20, 30};
const int STEP_1771[4] = {12, 12, 20, 40};

std::string hex2(unsigned v) {
    char b[8];
    snprintf(b, sizeof b, "$%02X", v & 0xFF);
    return b;
}
} // namespace

Wd17xx::Wd17xx(Variant v, uint32_t base) : m_base(base), m_variant(v) {
    m_name = variantName(v);
    setClockHz(1000000);
    m_sector = 1;
}

Wd17xx::~Wd17xx() { flush(); }

const char* Wd17xx::variantName(Variant v) {
    switch (v) {
    case Variant::FD1771: return "FD1771";
    case Variant::FD1791: return "FD1791";
    case Variant::FD1793: return "FD1793";
    case Variant::FD1795: return "FD1795";
    case Variant::FD1797: return "FD1797";
    case Variant::WD2791: return "WD2791";
    case Variant::WD2793: return "WD2793";
    case Variant::WD2795: return "WD2795";
    case Variant::WD2797: return "WD2797";
    case Variant::WD1773: return "WD1773";
    }
    return "WD17xx";
}

bool Wd17xx::parseVariant(const std::string& name, Variant& out) {
    std::string n;
    for (char c : name) if (std::isdigit((unsigned char)c)) n += c;
    static const std::pair<const char*, Variant> table[] = {
        {"1771", Variant::FD1771}, {"1791", Variant::FD1791}, {"1793", Variant::FD1793},
        {"1795", Variant::FD1795}, {"1797", Variant::FD1797}, {"2791", Variant::WD2791},
        {"2793", Variant::WD2793}, {"2795", Variant::WD2795}, {"2797", Variant::WD2797},
        {"1773", Variant::WD1773},
    };
    for (auto& [k, v] : table)
        if (n == k) { out = v; return true; }
    return false;
}

void Wd17xx::setVariant(Variant v) {
    bool rename = m_name == variantName(m_variant);
    m_variant = v;
    if (rename) m_name = variantName(v);
}

bool Wd17xx::hasSso() const {
    return m_variant == Variant::FD1795 || m_variant == Variant::FD1797 ||
           m_variant == Variant::WD2795 || m_variant == Variant::WD2797;
}

bool Wd17xx::hasSideCompare() const {
    return m_variant == Variant::FD1791 || m_variant == Variant::FD1793 || m_variant == Variant::WD2791 ||
           m_variant == Variant::WD2793 || m_variant == Variant::WD1773;
}

bool Wd17xx::invertedBus() const {
    return m_variant == Variant::FD1791 || m_variant == Variant::FD1795 ||
           m_variant == Variant::WD2791 || m_variant == Variant::WD2795;
}

bool Wd17xx::doubleDensity() const { return !is1771() && m_dden; }

// ---------------------------------------------------------------------------
// Time and the head
// ---------------------------------------------------------------------------

void Wd17xx::setClockHz(uint32_t hz) {
    m_clockHz = hz ? hz : 1000000;
    m_rev = (uint64_t)m_clockHz * 60 / (uint64_t)m_rpm;
}

void Wd17xx::setRpm(int rpm) {
    m_rpm = rpm > 0 ? rpm : 300;
    setClockHz(m_clockHz);
}

uint64_t Wd17xx::usToCycles(uint64_t us) const { return (uint64_t)((u128)us * m_clockHz / 1000000); }

uint64_t Wd17xx::fdcMs(int ms) const {
    uint64_t us = (uint64_t)ms * 1000;
    if (m_variant != Variant::WD1773) us = us * 1000000 / m_fdcClock;   // the 1773 times from its own 8 MHz
    return usToCycles(us);
}

int Wd17xx::trackLen() const {
    if (FloppyTrack* t = curTrack()) return t->length();
    if (m_drive >= 0 && m_drives[m_drive].disk) return m_drives[m_drive].disk->trackLength();
    return FloppyDisk::TRACK_LEN_DD;
}

uint64_t Wd17xx::slotAt(uint64_t t) const { return (uint64_t)((u128)t * trackLen() / m_rev); }

uint64_t Wd17xx::timeOf(uint64_t slot) const {
    u128 L = (u128)trackLen();
    return (uint64_t)(((u128)slot * m_rev + L - 1) / L);
}

int Wd17xx::head() const { return hasSso() ? m_sso : m_extSide; }

FloppyTrack* Wd17xx::curTrack() const {
    if (m_drive < 0 || !m_motor) return nullptr;
    const Drive& d = m_drives[m_drive];
    if (!d.disk || head() >= d.sides) return nullptr;
    return d.disk->track(d.cyl, head());
}

uint8_t Wd17xx::readByte(uint64_t slot) const {
    FloppyTrack* t = curTrack();
    return t ? t->at(slot) : 0;
}

void Wd17xx::writeByte(uint64_t slot, uint8_t b) {
    FloppyTrack* t = curTrack();
    if (!t) return;
    int len = t->length();
    for (int i = 0; i < step(); ++i) {
        int idx = (int)((slot + i) % len);
        t->data[idx] = b;
        if (!t->idams.empty()) t->clearIdams(idx, 1);
    }
    m_drives[m_drive].disk->markDirty();
    m_writing = true;
}

bool Wd17xx::driveWriteProtected() const {
    if (m_drive < 0) return false;
    const Drive& d = m_drives[m_drive];
    return d.disk && (d.writeProtect || d.disk->writeProtected());
}

bool Wd17xx::trackZero() const { return m_drive >= 0 && m_drives[m_drive].cyl == 0; }

bool Wd17xx::ready() const {
    if (m_readyMode == ReadyMode::Always) return true;
    if (m_drive < 0 || !m_drives[m_drive].disk) return false;
    return m_readyMode == ReadyMode::DiskPresent || m_motor;
}

bool Wd17xx::indexPulse() const {
    if (m_drive < 0 || !m_motor || !m_drives[m_drive].disk) return false;
    return m_now % m_rev < m_rev / 40;      // the hole passes in about 5 ms
}

// ---------------------------------------------------------------------------
// Signals and registers
// ---------------------------------------------------------------------------

void Wd17xx::setSignalLine(const char* name, ISignalLine* line) {
    std::string n = name ? name : "";
    if (n == "intrq" || n == "irq") m_intrqLine = line;
    else if (n == "drq") m_drqLine = line;
}

void Wd17xx::setIntrq(bool on) {
    if (m_intrq == on) return;
    m_intrq = on;
    if (m_intrqLine) m_intrqLine->set(on);
    if (m_intrqHook) m_intrqHook(on);
}

void Wd17xx::setDrq(bool on) {
    if (on) m_status |= ST_DRQ;
    else if (!m_type1Status) m_status &= ~ST_DRQ;
    if (m_drq == on) return;
    m_drq = on;
    if (m_drqLine) m_drqLine->set(on);
    if (m_drqHook) m_drqHook(on);
}

uint8_t Wd17xx::composeStatus() const {
    uint8_t s = m_status;
    if (ready()) s &= ~ST_NOTREADY; else s |= ST_NOTREADY;
    if (m_type1Status) {
        s &= ~(ST_INDEX | ST_TRACK0 | ST_HEADLOADED | ST_WRITEPROTECT);
        if (indexPulse()) s |= ST_INDEX;
        if (trackZero()) s |= ST_TRACK0;
        if (m_headLoaded) s |= ST_HEADLOADED;
        if (driveWriteProtected()) s |= ST_WRITEPROTECT;
    } else {
        s = (uint8_t)((s & ~ST_DRQ) | (m_drq ? ST_DRQ : 0));
    }
    return s;
}

uint8_t Wd17xx::peekRegister(int reg) const {
    uint8_t v = 0;
    switch (reg & 3) {
    case 0: v = composeStatus(); break;
    case 1: v = m_track; break;
    case 2: v = m_sector; break;
    case 3: v = m_data; break;
    }
    return invertedBus() ? (uint8_t)~v : v;
}

uint8_t Wd17xx::readRegister(int reg) {
    uint8_t v = peekRegister(reg);
    if ((reg & 3) == 0) {
        if (!m_intImmediate) setIntrq(false);
    } else if ((reg & 3) == 3) {
        setDrq(false);
    }
    return v;
}

void Wd17xx::writeRegister(int reg, uint8_t v) {
    if (invertedBus()) v = (uint8_t)~v;
    switch (reg & 3) {
    case 0: command(v); break;
    case 1: m_track = v; break;
    case 2: m_sector = v; break;
    case 3: m_data = v; setDrq(false); break;
    }
}

bool Wd17xx::ioRead(IBus*, uint32_t addr, uint8_t* val) {
    if ((addr & ~3u) != (m_base & ~3u)) return false;
    *val = readRegister((int)(addr & 3));
    return true;
}

bool Wd17xx::ioPeek(IBus*, uint32_t addr, uint8_t* val) {
    if ((addr & ~3u) != (m_base & ~3u)) return false;
    *val = peekRegister((int)(addr & 3));
    return true;
}

bool Wd17xx::ioWrite(IBus*, uint32_t addr, uint8_t val) {
    if ((addr & ~3u) != (m_base & ~3u)) return false;
    writeRegister((int)(addr & 3), val);
    return true;
}

// ---------------------------------------------------------------------------
// Drives
// ---------------------------------------------------------------------------

void Wd17xx::checkReadyChange() {
    bool r = ready();
    if (r == m_lastReady) return;
    if ((r && m_intNotReadyToReady) || (!r && m_intReadyToNotReady)) {
        if (m_state != State::Idle) { m_state = State::Idle; m_status &= ~ST_BUSY; }
        setIntrq(true);
    }
    m_lastReady = r;
}

void Wd17xx::selectDrive(int drive) {
    m_drive = (drive >= 0 && drive < MAX_DRIVES) ? drive : -1;
    checkReadyChange();
}

void Wd17xx::setMotor(bool on) {
    m_motor = on;
    checkReadyChange();
}

void Wd17xx::setReadyMode(ReadyMode m) {
    m_readyMode = m;
    checkReadyChange();
}

void Wd17xx::setDriveGeometry(int drive, int cylinders, int sides) {
    if (drive < 0 || drive >= MAX_DRIVES) return;
    m_drives[drive].cylinders = std::max(1, cylinders);
    m_drives[drive].sides = std::clamp(sides, 1, 2);
    m_drives[drive].cyl = std::min(m_drives[drive].cyl, m_drives[drive].cylinders - 1);
}

void Wd17xx::insertDisk(int drive, std::unique_ptr<FloppyDisk> disk) {
    if (drive < 0 || drive >= MAX_DRIVES) return;
    removeDisk(drive);
    m_drives[drive].disk = std::move(disk);
    checkReadyChange();
}

std::unique_ptr<FloppyDisk> Wd17xx::removeDisk(int drive) {
    if (drive < 0 || drive >= MAX_DRIVES) return nullptr;
    auto& d = m_drives[drive].disk;
    if (d && d->dirty()) d->save();
    std::unique_ptr<FloppyDisk> out = std::move(d);
    checkReadyChange();
    return out;
}

FloppyDisk* Wd17xx::disk(int drive) const {
    return (drive >= 0 && drive < MAX_DRIVES) ? m_drives[drive].disk.get() : nullptr;
}

void Wd17xx::setWriteProtect(int drive, bool wp) {
    if (drive >= 0 && drive < MAX_DRIVES) m_drives[drive].writeProtect = wp;
}

int Wd17xx::driveCylinder(int drive) const {
    return (drive >= 0 && drive < MAX_DRIVES) ? m_drives[drive].cyl : 0;
}

void Wd17xx::setDriveCylinder(int drive, int cyl) {
    if (drive >= 0 && drive < MAX_DRIVES)
        m_drives[drive].cyl = std::clamp(cyl, 0, m_drives[drive].cylinders - 1);
}

void Wd17xx::flush() {
    for (Drive& d : m_drives)
        if (d.disk && d.disk->dirty()) d.disk->save();
    m_flushAt = 0;
}

bool Wd17xx::mountDisk(int unit, const std::string& path) {
    if (unit < 0 || unit >= MAX_DRIVES) return false;
    std::string err;
    auto d = FloppyDisk::load(path, &err);
    if (!d) return false;
    insertDisk(unit, std::move(d));
    return true;
}

void Wd17xx::ejectDisk(int unit) { removeDisk(unit); }

bool Wd17xx::getDiskStatus(int unit, int& track, int& sector, bool& led) {
    if (unit < 0 || unit >= MAX_DRIVES) return false;
    track = m_drives[unit].cyl;
    sector = m_sector;
    led = busy() && unit == m_drive;
    return m_drives[unit].disk != nullptr;
}

std::string Wd17xx::getMountedDiskPath(int unit) {
    FloppyDisk* d = disk(unit);
    return d ? d->path() : "";
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void Wd17xx::reset() {
    m_state = State::Idle;
    m_status = 0;
    m_track = 0;
    m_sector = 1;
    m_data = 0;
    m_command = 0;
    m_type1Status = true;
    m_intImmediate = m_intIndex = m_intNotReadyToReady = m_intReadyToNotReady = false;
    m_sso = 0;
    setDrq(false);
    setIntrq(false);
    m_lastReady = ready();
    command(0x03);                          // MR loads $03 and runs a Restore
}

void Wd17xx::schedule(State s, uint64_t at) {
    m_state = s;
    m_eventAt = std::max(at, m_now);
}

void Wd17xx::finish(uint8_t setBits) {
    m_status = (uint8_t)((m_status | setBits) & ~ST_BUSY);
    m_state = State::Idle;
    m_lastActive = m_now;
    if (m_writing) {
        m_writing = false;
        m_flushAt = m_now + m_clockHz / 2;
    }
    setIntrq(true);
}

void Wd17xx::forceInterrupt(uint8_t cmd) {
    m_command = cmd;
    m_intNotReadyToReady = cmd & 0x01;
    m_intReadyToNotReady = cmd & 0x02;
    m_intIndex = cmd & 0x04;
    m_intImmediate = cmd & 0x08;
    if (m_state == State::Idle && !(m_status & ST_BUSY)) {
        m_type1Status = true;
    } else {
        m_state = State::Idle;
        m_status &= ~ST_BUSY;
        if (m_writing) { m_writing = false; m_flushAt = m_now + m_clockHz / 2; }
    }
    m_lastActive = m_now;
    m_lastReady = ready();
    setIntrq(m_intImmediate);
}

void Wd17xx::stepDrive(int dir) {
    if (m_drive < 0) return;
    Drive& d = m_drives[m_drive];
    d.cyl = std::clamp(d.cyl + dir, 0, d.cylinders - 1);
}

int Wd17xx::sectorLength(uint8_t n) const {
    bool lflag = m_command & 0x08;
    if (is1771()) return lflag ? 128 << (n & 3) : (n ? n * 16 : 4096);
    if (hasSso() && !lflag) {
        static const int len[4] = {256, 512, 1024, 128};
        return len[n & 3];
    }
    return 128 << (n & 3);
}

void Wd17xx::command(uint8_t cmd) {
    if ((cmd & 0xF0) == 0xD0) { forceInterrupt(cmd); return; }
    if (m_status & ST_BUSY) return;         // ignored while busy
    m_command = cmd;
    if (!m_intImmediate) setIntrq(false);
    m_lastActive = m_now;
    m_writing = false;

    if (!(cmd & 0x80)) {                    // Type I
        m_type1Status = true;
        m_status = ST_BUSY;
        setDrq(false);
        m_headLoaded = (cmd & 0x08) != 0;
        m_isStep = (cmd & 0xE0) != 0;
        if ((cmd & 0xE0) == 0x40) m_dir = 1;
        else if ((cmd & 0xE0) == 0x60) m_dir = -1;
        if ((cmd & 0xF0) == 0x00) { m_track = 0xFF; m_data = 0; }
        schedule(State::StepLoop, m_now);
        run();
        return;
    }

    m_type1Status = false;
    m_status = ST_BUSY;
    setDrq(false);
    if (!ready()) {                         // Type II/III need a ready drive
        m_status = 0;
        m_state = State::Idle;
        setIntrq(true);
        return;
    }
    if (hasSso()) m_sso = (cmd >> 1) & 1;
    m_headLoaded = true;
    uint64_t delay = (cmd & 0x04) ? fdcMs(is1771() ? 20 : 30) : 0;
    schedule(State::T2Start, m_now + delay);
    run();
}

void Wd17xx::startSearch(Search mode) {
    m_search = mode;
    m_slot = slotAt(m_now) + 1;
    m_searchRev = m_slot / trackLen();
    searchNext();
}

void Wd17xx::searchNext() {
    int L = trackLen();
    bool dd = doubleDensity();
    uint64_t limit = (m_searchRev + 5) * (uint64_t)L;   // the fifth index pulse
    uint64_t found = UINT64_MAX;
    if (FloppyTrack* t = curTrack()) {
        uint64_t pos = m_slot % L, base = m_slot - pos;
        uint64_t first = UINT64_MAX;
        for (uint16_t v : t->idams) {
            if (((v & FloppyTrack::IDAM_DD) != 0) != dd) continue;
            uint64_t off = v & FloppyTrack::IDAM_OFFSET;
            if (first == UINT64_MAX) first = off;
            if (off >= pos) { found = base + off; break; }
        }
        if (found == UINT64_MAX && first != UINT64_MAX) found = base + L + first;
    }
    if (found == UINT64_MAX || found >= limit) {
        m_slot = limit;
        schedule(State::Search, timeOf(limit));      // fails there
        return;
    }
    m_idSlot = found;
    int st = step();
    if (m_search == Search::Address) {
        m_slot = found + st;                // the track byte
        m_crc = dd ? floppyCrcAfterA1() : FLOPPY_CRC_INIT;
        crcByte(0xFE);
        m_left = 6;
        schedule(State::ReadAddrByte, timeOf(m_slot + st));
        return;
    }
    schedule(State::IdFound, timeOf(found + 7 * (uint64_t)st));
}

void Wd17xx::idFound() {
    bool dd = doubleDensity();
    int st = step();
    uint16_t crc = dd ? floppyCrcAfterA1() : FLOPPY_CRC_INIT;
    uint8_t id[7];
    for (int k = 0; k < 7; ++k) {
        id[k] = readByte(m_idSlot + (uint64_t)k * st);
        crc = floppyCrc(crc, id[k]);
    }
    m_slot = m_idSlot + 7 * (uint64_t)st;
    bool crcOk = crc == 0;

    if (m_search == Search::Verify) {
        if (id[1] != m_track) { searchNext(); return; }
        if (!crcOk) { m_status |= ST_CRC; searchNext(); return; }
        m_status &= ~ST_CRC;
        finish();
        return;
    }

    bool match = id[1] == m_track && id[3] == m_sector;
    if (match && hasSideCompare() && (m_command & 0x02))
        match = (id[2] & 1) == ((m_command >> 3) & 1);
    if (!match) { searchNext(); return; }
    if (!crcOk) { m_status |= ST_CRC; searchNext(); return; }
    m_status &= ~ST_CRC;
    m_left = sectorLength(id[4]);

    if (m_command & 0x20) {                 // Write Sector: DRQ two bytes into gap 2
        schedule(State::WriteDrq, timeOf(m_slot + 2 * (uint64_t)st));
        return;
    }
    // Read Sector: the data mark within 30 (FM) / 43 (MFM) bytes
    int window = dd ? 43 : 30;
    for (int j = 0; j < window; ++j) {
        uint64_t q = m_slot + (uint64_t)j * st;
        uint8_t b = readByte(q);
        if (b < 0xF8 || b > 0xFB) continue;
        if (dd && (j == 0 || readByte(q - 1) != 0xA1)) continue;
        if (is1771()) m_status = (uint8_t)((m_status & ~0x60) | (((0xFB - b) & 3) << 5));
        else if ((b & 0xFE) == 0xF8) m_status |= ST_RECTYPE;
        m_crc = dd ? floppyCrcAfterA1() : FLOPPY_CRC_INIT;
        crcByte(b);
        m_slot = q + st;
        schedule(State::ReadByte, timeOf(m_slot + st));
        return;
    }
    searchNext();
}

void Wd17xx::run() {
    while (m_state != State::Idle && m_eventAt <= m_now) {
        int st = step();
        bool dd = doubleDensity();
        auto put = [&](uint8_t b) {
            writeByte(m_slot, b);
            crcByte(b);
            m_slot += st;
        };
        switch (m_state) {
        case State::Idle:
            break;

        // Type I ------------------------------------------------------------
        case State::StepLoop: {
            int rate = (is1771() ? STEP_1771 : STEP_179X)[m_command & 3];
            if (!m_isStep) {
                if (m_track == m_data) { schedule(State::StepDone, m_now); break; }
                m_dir = m_data > m_track ? 1 : -1;
                m_track = (uint8_t)(m_track + m_dir);
            } else if (m_command & 0x10) {
                m_track = (uint8_t)(m_track + m_dir);
            }
            if (m_dir < 0 && trackZero()) {
                if (!m_isStep) m_track = 0;
                schedule(State::StepDone, m_now + fdcMs(rate));
                break;
            }
            stepDrive(m_dir);
            schedule(m_isStep ? State::StepDone : State::StepLoop, m_now + fdcMs(rate));
            break;
        }
        case State::StepDone:
            if ((m_command & 0xF0) == 0x00 && !trackZero()) m_status |= ST_SEEK;   // 255 steps, no track 0
            if (m_command & 0x04) {         // verify after the head settles
                m_headLoaded = true;
                schedule(State::VerifyStart, m_now + fdcMs(is1771() ? 20 : 30));
            } else {
                finish();
            }
            break;
        case State::VerifyStart:
            m_state = State::Idle;
            startSearch(Search::Verify);
            break;
        case State::Search:                 // five index pulses without a match
            finish(m_search == Search::Verify ? ST_SEEK : ST_RNF);
            break;
        case State::IdFound:
            m_state = State::Idle;
            idFound();
            break;

        // Type II / III start -------------------------------------------------
        case State::T2Start: {
            uint8_t op = m_command & 0xF0;
            bool writes = (op & 0xE0) == 0xA0 || op == 0xF0;
            if (writes && driveWriteProtected()) { finish(ST_WRITEPROTECT); break; }
            if (op == 0xC0) { m_state = State::Idle; startSearch(Search::Address); break; }
            if (op == 0xE0) { schedule(State::ReadTrackStart, m_now); break; }
            if (op == 0xF0) {
                setDrq(true);
                schedule(State::WriteTrackCheck, timeOf(slotAt(m_now) + 3 * (uint64_t)st));
                break;
            }
            m_state = State::Idle;
            startSearch(Search::Sector);
            break;
        }

        // Read Sector ------------------------------------------------------------
        case State::ReadByte: {
            uint8_t b = readByte(m_slot);
            crcByte(b);
            m_slot += st;
            if (m_drq) m_status |= ST_LOST;
            m_data = b;
            setDrq(true);
            if (--m_left > 0) schedule(State::ReadByte, timeOf(m_slot + st));
            else schedule(State::ReadCrc, timeOf(m_slot + 2 * (uint64_t)st));
            break;
        }
        case State::ReadCrc:
            crcByte(readByte(m_slot));
            crcByte(readByte(m_slot + st));
            m_slot += 2 * (uint64_t)st;
            if (m_crc != 0) { finish(ST_CRC); break; }
            if (m_command & 0x10) {         // multiple sectors
                ++m_sector;
                m_state = State::Idle;
                startSearch(Search::Sector);
                break;
            }
            finish();
            break;

        // Write Sector -----------------------------------------------------------
        case State::WriteDrq:
            setDrq(true);
            schedule(State::WriteStart, timeOf(m_slot + (uint64_t)(dd ? 22 : 11) * st));
            break;
        case State::WriteStart: {
            if (m_drq) { setDrq(false); finish(ST_LOST); break; }
            m_slot += (uint64_t)(dd ? 22 : 11) * st;
            for (int i = 0; i < (dd ? 12 : 6); ++i) put(0x00);
            m_crc = FLOPPY_CRC_INIT;
            if (dd) for (int i = 0; i < 3; ++i) put(0xA1);
            uint8_t dam;
            if (is1771()) dam = (uint8_t)(0xFB - (m_command & 3));
            else dam = (m_command & 1) ? 0xF8 : 0xFB;
            put(dam);
            schedule(State::WriteByte, timeOf(m_slot));
            break;
        }
        case State::WriteByte: {
            uint8_t b = m_data;
            if (m_drq) { m_status |= ST_LOST; b = 0; }
            put(b);
            if (--m_left > 0) {
                setDrq(true);
                schedule(State::WriteByte, timeOf(m_slot));
            } else {
                uint16_t crc = m_crc;
                put((uint8_t)(crc >> 8));
                put((uint8_t)crc);
                put(0xFF);
                schedule(State::WriteEnd, timeOf(m_slot));
            }
            break;
        }
        case State::WriteEnd:
            if (m_command & 0x10) {
                ++m_sector;
                m_state = State::Idle;
                startSearch(Search::Sector);
                break;
            }
            finish();
            break;

        // Read Address -----------------------------------------------------------
        case State::ReadAddrByte: {
            uint8_t b = readByte(m_slot);
            crcByte(b);
            m_slot += st;
            if (m_left == 6) m_idTrack = b;
            if (m_drq) m_status |= ST_LOST;
            m_data = b;
            setDrq(true);
            if (--m_left > 0) { schedule(State::ReadAddrByte, timeOf(m_slot + st)); break; }
            m_sector = m_idTrack;           // the ID's track goes to the sector register
            finish(m_crc ? ST_CRC : 0);
            break;
        }

        // Read Track ---------------------------------------------------------------
        case State::ReadTrackStart: {
            uint64_t L = (uint64_t)trackLen();
            uint64_t s = slotAt(m_now);
            m_slot = (s / L + 1) * L;       // from the next index pulse
            m_endSlot = m_slot + L;
            schedule(State::ReadTrackByte, timeOf(m_slot + st));
            break;
        }
        case State::ReadTrackByte: {
            uint8_t b = readByte(m_slot);
            m_slot += st;
            if (m_drq) m_status |= ST_LOST;
            m_data = b;
            setDrq(true);
            if (m_slot + st > m_endSlot) finish();
            else schedule(State::ReadTrackByte, timeOf(m_slot + st));
            break;
        }

        // Write Track ---------------------------------------------------------------
        case State::WriteTrackCheck: {
            if (m_drq) { setDrq(false); finish(ST_LOST); break; }   // no first byte in 3 byte times
            uint64_t L = (uint64_t)trackLen();
            m_slot = (slotAt(m_now) / L + 1) * L;
            m_endSlot = m_slot + L;
            schedule(State::WriteTrackStart, timeOf(m_slot));
            break;
        }
        case State::WriteTrackStart: {
            if (m_drive >= 0 && m_motor && m_drives[m_drive].disk && head() < m_drives[m_drive].sides) {
                Drive& d = m_drives[m_drive];
                if (FloppyTrack* t = d.disk->extend(d.cyl, head())) {
                    t->idams.clear();
                    d.disk->markDirty();
                }
            }
            m_crc = FLOPPY_CRC_INIT;
            schedule(State::WriteTrackByte, m_now);
            break;
        }
        case State::WriteTrackByte: {
            if (m_slot >= m_endSlot) { setDrq(false); finish(); break; }
            uint8_t b = m_data;
            if (m_drq) { m_status |= ST_LOST; b = 0; }
            setDrq(true);
            FloppyTrack* t = curTrack();
            int L = trackLen();
            if (b == 0xF7) {                // two CRC bytes
                uint16_t crc = m_crc;
                put((uint8_t)(crc >> 8));
                put((uint8_t)crc);
            } else if (b == 0xFE) {         // an ID address mark
                uint64_t at = m_slot;
                if (!dd) m_crc = FLOPPY_CRC_INIT;
                put(0xFE);
                if (t) t->addIdam((int)(at % L), dd);
            } else if (dd && b == 0xF5) {   // $A1 sync, CRC preset
                m_crc = FLOPPY_CRC_INIT;
                crcByte(0xA1);
                crcByte(0xA1);
                put(0xA1);
            } else if (dd && b == 0xF6) {
                put(0xC2);
            } else if (!dd && b >= 0xF8 && b <= 0xFB) {   // FM data marks preset the CRC
                m_crc = FLOPPY_CRC_INIT;
                put(b);
            } else {
                put(b);
            }
            schedule(State::WriteTrackByte, timeOf(m_slot));
            break;
        }
        }
    }
}

void Wd17xx::tick(uint64_t cycles) {
    uint64_t start = m_now, target = m_now + cycles;
    while (m_state != State::Idle && m_eventAt <= target) {
        m_now = m_eventAt;
        run();
    }
    m_now = target;
    if (m_intIndex && m_drive >= 0 && m_motor && m_drives[m_drive].disk && target / m_rev != start / m_rev)
        setIntrq(true);                     // I2: an interrupt every index pulse
    if (m_headLoaded && m_state == State::Idle && m_now - m_lastActive > 15 * m_rev)
        m_headLoaded = false;               // 15 idle turns unload the head
    if (m_flushAt && m_now >= m_flushAt && m_state == State::Idle) flush();
}

// ---------------------------------------------------------------------------
// Debugger and options
// ---------------------------------------------------------------------------

void Wd17xx::getDeviceInfo(DeviceInfo& out) const {
    out.name = name();
    out.baseAddr = m_base;
    out.addrMask = addrMask();
    out.registers.push_back({"STATUS", 0, composeStatus(), "status (read) / command (write)",
        {{"BUSY", 0, 1}, {"IDX/DRQ", 1, 1}, {"TR0/LOST", 2, 1}, {"CRC", 3, 1},
         {"SEEK/RNF", 4, 1}, {"HLD/RTYPE", 5, 1}, {"WPROT", 6, 1}, {"NREADY", 7, 1}}});
    out.registers.push_back({"TRACK", 1, m_track, "track register", {}});
    out.registers.push_back({"SECTOR", 2, m_sector, "sector register", {}});
    out.registers.push_back({"DATA", 3, m_data, "data register", {}});
    out.state.push_back({"variant", variantName(m_variant)});
    out.state.push_back({"command", hex2(m_command)});
    out.state.push_back({"busy", busy() ? "yes" : "no"});
    out.state.push_back({"intrq", m_intrq ? "1" : "0"});
    out.state.push_back({"drq", m_drq ? "1" : "0"});
    out.state.push_back({"drive", std::to_string(m_drive)});
    out.state.push_back({"side", std::to_string(head())});
    out.state.push_back({"density", doubleDensity() ? "double (MFM)" : "single (FM)"});
    out.state.push_back({"motor", m_motor ? "on" : "off"});
    out.state.push_back({"ready", ready() ? "yes" : "no"});
    for (int i = 0; i < MAX_DRIVES; ++i) {
        const Drive& d = m_drives[i];
        std::string s = "cyl " + std::to_string(d.cyl);
        if (d.disk) {
            s += ", " + std::string(FloppyDisk::formatName(d.disk->format())) + " " +
                 std::to_string(d.disk->cylinders()) + "x" + std::to_string(d.disk->heads());
            if (!d.disk->path().empty()) s += " " + d.disk->path();
            if (d.writeProtect || d.disk->writeProtected()) s += " (protected)";
        } else {
            s += ", empty";
        }
        out.state.push_back({"drive" + std::to_string(i), s});
    }
}

bool Wd17xx::setOption(const std::string& key, const std::string& value, std::string& err) {
    try {
        if (key == "variant") {
            Variant v;
            if (!parseVariant(value, v)) { err = "unknown variant " + value; return false; }
            setVariant(v);
            return true;
        }
        if (key == "base") { m_base = (uint32_t)std::stoul(value, nullptr, 0); return true; }
        if (key == "rpm") { setRpm(std::stoi(value)); return true; }
        if (key == "fdcclock") { setFdcClockHz((uint32_t)std::stoul(value, nullptr, 0)); return true; }
        if (key == "ready") {
            if (value == "motor") setReadyMode(ReadyMode::DiskAndMotor);
            else if (value == "disk") setReadyMode(ReadyMode::DiskPresent);
            else if (value == "always") setReadyMode(ReadyMode::Always);
            else { err = "ready: motor, disk or always"; return false; }
            return true;
        }
        if (key.size() == 6 && key.compare(0, 5, "drive") == 0 && key[5] >= '0' && key[5] <= '3') {
            auto d = FloppyDisk::load(value, &err);
            if (!d) return false;
            insertDisk(key[5] - '0', std::move(d));
            return true;
        }
    } catch (...) {
        err = "bad value " + value;
        return false;
    }
    err = "no such option";
    return false;
}
