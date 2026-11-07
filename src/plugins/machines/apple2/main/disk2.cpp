// The Disk II interface card and 5.25" disk images, after AppleWin
// (GPL-2.0-or-later): Disk.cpp (Disk2InterfaceCard, the .dsk/.nib path)
// and DiskImageHelper.cpp (CImageBase Code62/Decode62, NibblizeTrack,
// DenibblizeTrack, SkewTrack; CDoImage, CPoImage, CNib1Image).

#include "disk2.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <unistd.h>

namespace apple2 {

namespace {

// Valid disk bytes for the 6-bit values (6-and-2 encoding)
const uint8_t DISK_BYTE[0x40] = {
    0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6, 0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
    0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
    0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF};

// The image's sector for each physical sector, ProDOS and DOS order
const uint8_t SECTOR_NUMBER[2][16] = {
    {0x00, 0x08, 0x01, 0x09, 0x02, 0x0A, 0x03, 0x0B, 0x04, 0x0C, 0x05, 0x0D, 0x06, 0x0E, 0x07, 0x0F},
    {0x00, 0x07, 0x0E, 0x06, 0x0D, 0x05, 0x0C, 0x04, 0x0B, 0x03, 0x0A, 0x02, 0x09, 0x01, 0x08, 0x0F}};

constexpr uint8_t VOLUME = 254;
constexpr int TRACKS_STANDARD = 35;
constexpr int TRACKS_MAX = 40;

std::string lowerExt(const std::string& path) {
    const auto dot = path.find_last_of('.');
    std::string e = dot == std::string::npos ? "" : path.substr(dot);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

// CImageBase::IsValidImageSize: 140 KB + n x 4 KB (up to 40 tracks), or
// the odd sizes seen in the wild
bool validDskSize(size_t n) {
    const size_t t = Disk2Image::TRACK_BYTES;
    if (n > (size_t)TRACKS_MAX * t) return false;
    if (n >= (size_t)(TRACKS_STANDARD + 1) * t) return (n - TRACKS_STANDARD * t) % t == 0;
    return (n >= 143105 && n <= 143364) || n == 143403 || n == 143488;
}

uint16_t word(const std::vector<uint8_t>& d, size_t at) { return (uint16_t)(d[at] | (d[at + 1] << 8)); }

// CDoImage::Detect: a DOS 3.3 catalog chain on track 17 in DOS order, or a
// ProDOS volume directory's block links
bool looksDosOrder(const std::vector<uint8_t>& d) {
    bool ok = true;
    for (int loop = 1; loop < 16 && ok; ++loop)
        if (d[0x11002 + (loop << 8)] != loop - 1) ok = false;
    if (ok) return true;
    ok = true;
    for (int loop = 2; loop <= 5 && ok; ++loop)
        if (word(d, (loop << 9) + 0x100) != (loop == 5 ? 0 : 6 - loop) ||
            word(d, (loop << 9) + 0x102) != (loop == 2 ? 0 : 8 - loop)) ok = false;
    return ok;
}
// CPoImage::Detect
bool looksProDosOrder(const std::vector<uint8_t>& d) {
    bool ok = true;
    for (int loop = 5; loop < 14 && ok; ++loop)
        if (d[0x11002 + (loop << 8)] != 14 - loop) ok = false;
    if (ok) return true;
    ok = true;
    for (int loop = 2; loop <= 5 && ok; ++loop)
        if (word(d, loop << 9) != (loop == 2 ? 0 : loop - 1) ||
            word(d, (loop << 9) + 2) != (loop == 5 ? 0 : loop + 1)) ok = false;
    return ok;
}

}  // namespace

// ---------------------------------------------------------------------------
// Disk2Image
// ---------------------------------------------------------------------------

std::unique_ptr<Disk2Image> Disk2Image::fromBytes(std::vector<uint8_t> data, const std::string& ext,
                                                  std::string* err) {
    auto img = std::unique_ptr<Disk2Image>(new Disk2Image());
    const std::string e = lowerExt("x" + ext);
    if (e == ".nib") {
        if (data.empty() || data.size() % NIB_TRACK || data.size() / NIB_TRACK > TRACKS_MAX) {
            if (err) *err = "not a .nib image (35 tracks of 6656 nibbles)";
            return nullptr;
        }
        img->m_order = Order::Nib;
        img->m_tracks = (int)(data.size() / NIB_TRACK);
    } else {
        if (!validDskSize(data.size())) {
            if (err) *err = "not a 140 KB 5.25\" disk image";
            return nullptr;
        }
        if (e == ".po") img->m_order = Order::ProDOS;
        else if (e == ".do") img->m_order = Order::DOS;
        else img->m_order = looksDosOrder(data) ? Order::DOS : looksProDosOrder(data) ? Order::ProDOS : Order::DOS;
        img->m_tracks = (int)(data.size() / TRACK_BYTES);
    }
    img->m_data = std::move(data);
    return img;
}

std::unique_ptr<Disk2Image> Disk2Image::open(const std::string& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "can't open " + path;
        return nullptr;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto img = fromBytes(std::move(data), lowerExt(path), err);
    if (!img) return nullptr;
    img->m_path = path;
    img->m_protect = access(path.c_str(), W_OK) != 0;
    return img;
}

// Code62: 256 bytes -> 342 6-bit values (86 of the low two bits, 256 of
// the high six) -> XOR-chained with a checksum (343) -> disk bytes
void Disk2Image::nibblizeSector(const uint8_t* sector, uint8_t* out) {
    uint8_t six[343];
    int n = 0;
    uint8_t offset = 0xAC;
    auto add = [](uint8_t value, uint8_t a) {
        return (uint8_t)((value << 2) | ((a & 0x01) << 1) | ((a & 0x02) >> 1));
    };
    while (offset != 0x02) {
        uint8_t value = 0;
        value = add(value, sector[offset]); offset = (uint8_t)(offset - 0x56);
        value = add(value, sector[offset]); offset = (uint8_t)(offset - 0x56);
        value = add(value, sector[offset]); offset = (uint8_t)(offset - 0x53);
        six[n++] = (uint8_t)(value << 2);
    }
    six[n - 2] &= 0x3F;
    six[n - 1] &= 0x3F;
    for (int i = 0; i < 0x100; ++i) six[n++] = sector[i];
    uint8_t chained[343];
    uint8_t saved = 0;
    for (int i = 0; i < 342; ++i) { chained[i] = saved ^ six[i]; saved = six[i]; }
    chained[342] = saved;
    for (int i = 0; i < 343; ++i) out[i] = DISK_BYTE[chained[i] >> 2];
}

// Decode62
void Disk2Image::denibblizeSector(const uint8_t* in, uint8_t* sector) {
    static uint8_t sixBit[0x80];
    static bool made = false;
    if (!made) {
        std::memset(sixBit, 0, sizeof sixBit);
        for (int i = 0; i < 0x40; ++i) sixBit[DISK_BYTE[i] - 0x80] = (uint8_t)(i << 2);
        made = true;
    }
    uint8_t six[343], buf[342];
    for (int i = 0; i < 343; ++i) six[i] = sixBit[in[i] & 0x7F];
    uint8_t saved = 0;
    for (int i = 0; i < 342; ++i) { buf[i] = saved ^ six[i]; saved = buf[i]; }
    const uint8_t* low = buf;
    const uint8_t* high = buf + 0x56;
    uint8_t offset = 0xAC;
    while (offset != 0x02) {
        if (offset >= 0xAC)
            sector[offset] = (uint8_t)((high[offset] & 0xFC) | ((*low & 0x80) >> 7) | ((*low & 0x40) >> 5));
        offset = (uint8_t)(offset - 0x56);
        sector[offset] = (uint8_t)((high[offset] & 0xFC) | ((*low & 0x20) >> 5) | ((*low & 0x10) >> 3));
        offset = (uint8_t)(offset - 0x56);
        sector[offset] = (uint8_t)((high[offset] & 0xFC) | ((*low & 0x08) >> 3) | ((*low & 0x04) >> 1));
        offset = (uint8_t)(offset - 0x53);
        ++low;
    }
}

// NibblizeTrack: gap 1 (48 sync bytes), then 16 sectors of an address
// field (D5 AA 96, volume, track, sector, checksum in 4-and-4, DE AA EB),
// gap 2 (6), the data field (D5 AA AD, 343 bytes, DE AA EB), gap 3 (27)
int Disk2Image::nibblize(int track, uint8_t* out) const {
    const uint8_t* data = m_data.data() + (size_t)track * TRACK_BYTES;
    const int ord = m_order == Order::ProDOS ? 0 : 1;
    uint8_t* p = out;
    auto a44 = [](uint8_t v) { return (uint8_t)(((v >> 1) & 0x55) | 0xAA); };
    auto b44 = [](uint8_t v) { return (uint8_t)((v & 0x55) | 0xAA); };
    for (int i = 0; i < 48; ++i) *p++ = 0xFF;
    for (int s = 0; s < 16; ++s) {
        const uint8_t t = (uint8_t)track, sec = (uint8_t)s, chk = (uint8_t)(VOLUME ^ t ^ sec);
        *p++ = 0xD5; *p++ = 0xAA; *p++ = 0x96;
        *p++ = a44(VOLUME); *p++ = b44(VOLUME);
        *p++ = a44(t); *p++ = b44(t);
        *p++ = a44(sec); *p++ = b44(sec);
        *p++ = a44(chk); *p++ = b44(chk);
        *p++ = 0xDE; *p++ = 0xAA; *p++ = 0xEB;
        for (int i = 0; i < 6; ++i) *p++ = 0xFF;
        *p++ = 0xD5; *p++ = 0xAA; *p++ = 0xAD;
        nibblizeSector(data + (SECTOR_NUMBER[ord][s] << 8), p);
        p += 343;
        *p++ = 0xDE; *p++ = 0xAA; *p++ = 0xEB;
        for (int i = 0; i < 27; ++i) *p++ = 0xFF;
    }
    return (int)(p - out);
}

// DenibblizeTrack: find each address field's sector, then decode the data
// field after it into the image's sector (sectors not found read as zeroes)
void Disk2Image::denibblize(const uint8_t* track, int nibbles, uint8_t* out) const {
    std::memset(out, 0, TRACK_BYTES);
    const int ord = m_order == Order::ProDOS ? 0 : 1;
    uint8_t work[384];
    int offset = 0;
    int partsLeft = 16 * 2 + 1;
    int sector = -1;
    while (partsLeft--) {
        uint8_t v[3] = {0, 0, 0};
        int num = 0;
        int loop = nibbles;
        while (loop-- && num < 3) {
            if (num) v[num++] = track[offset++];
            else if (track[offset++] == 0xD5) num = 1;
            if (offset >= nibbles) offset = 0;
        }
        if (num == 3 && v[1] == 0xAA) {
            int temp = offset;
            for (int i = 0; i < 384; ++i) {
                work[i] = track[temp++];
                if (temp >= nibbles) temp = 0;
            }
            if (v[2] == 0x96) {
                sector = ((work[4] & 0x55) << 1) | (work[5] & 0x55);
            } else if (v[2] == 0xAD) {
                if (sector >= 0 && sector < 16) denibblizeSector(work, out + (SECTOR_NUMBER[ord][sector] << 8));
                sector = 0;
            }
        }
    }
}

int Disk2Image::readTrack(int track, uint8_t* out, bool skew) {
    if (track < 0 || track >= m_tracks) return 0;
    if (m_order == Order::Nib) {
        std::memcpy(out, m_data.data() + (size_t)track * NIB_TRACK, NIB_TRACK);
        return NIB_TRACK;
    }
    const int n = nibblize(track, out);
    if (skew) {     // SkewTrack: each track starts 768 nibbles on from the last
        const int k = (track * 768) % n;
        std::rotate(out, out + k, out + n);
    }
    return n;
}

void Disk2Image::writeTrack(int track, const uint8_t* nibbles, int count) {
    if (track < 0 || track >= m_tracks || m_protect) return;
    size_t at, len;
    if (m_order == Order::Nib) {
        at = (size_t)track * NIB_TRACK;
        len = NIB_TRACK;
        std::memcpy(m_data.data() + at, nibbles, std::min(count, NIB_TRACK));
    } else {
        at = (size_t)track * TRACK_BYTES;
        len = TRACK_BYTES;
        denibblize(nibbles, count, m_data.data() + at);
    }
    if (!m_path.empty()) {
        std::fstream f(m_path, std::ios::binary | std::ios::in | std::ios::out);
        if (f) {
            f.seekp((std::streamoff)at);
            f.write((const char*)m_data.data() + at, (std::streamsize)len);
        }
    }
}

// ---------------------------------------------------------------------------
// Disk2Card
// ---------------------------------------------------------------------------

Disk2Card::Disk2Card(std::vector<uint8_t> rom) : m_rom(std::move(rom)) {
    m_rom.resize(256, 0);
    for (auto& d : m_drive) d.disk.track.assign(Disk2Image::MAX_NIBBLES, 0);
}

Disk2Card::~Disk2Card() {
    flush(0);
    flush(1);
}

void Disk2Card::reset(bool powerCycle) {
    // RESET forces the switches off (UTAIIe table 9.1); the heads stay put
    m_curr = 0;
    m_motorOn = false;
    m_magnets = 0;
    m_writeMode = m_loadMode = false;
    m_deferred = false;
    if (powerCycle) {
        for (int i = 0; i < 2; ++i) {
            flush(i);
            m_drive[i].spinning = 0;
            m_drive[i].writeLight = 0;
            m_drive[i].motorOnCycle = 0;
        }
        m_lastCycle = m_lastReadLatch = m_now = 0;
    }
}

void Disk2Card::syncStepper(uint64_t now) {
    if (now > m_now) m_now = now;
    if (m_deferred && m_now >= m_deferredAt + 10) stepperDeferred();
}

// Disk2InterfaceCard::Update: the motor runs on for a second; a stopped
// drive's dirty track goes back to the image
void Disk2Card::update(uint64_t now, uint64_t cycles) {
    syncStepper(now);
    for (int i = 0; i < 2; ++i) {
        Drive& d = m_drive[i];
        if (d.spinning && !m_motorOn) d.spinning -= (int)std::min<uint64_t>((uint64_t)d.spinning, cycles);
        if (m_writeMode && m_curr == i && d.spinning) d.writeLight = SPINNING_CYCLES;
        else if (d.writeLight) d.writeLight -= (int)std::min<uint64_t>((uint64_t)d.writeLight, cycles);
        if (!m_motorOn && !d.spinning) flush(i);
    }
}

int Disk2Card::io(uint16_t addr, const uint8_t* wr, uint64_t now) {
    syncStepper(now);
    setSequencer(addr);
    switch (addr & 0xF) {
        case 0x0: case 0x1: case 0x2: case 0x3:
        case 0x4: case 0x5: case 0x6: case 0x7: controlStepper(addr, m_now); break;
        case 0x8: case 0x9: controlMotor(addr, m_now); break;
        case 0xA: case 0xB: enable(addr, m_now); break;
        case 0xC: readWrite(m_now); break;
        case 0xD: loadWriteProtect(); break;
        case 0xE: break;                        // read mode
        case 0xF:                               // write mode: the light
            m_drive[m_curr].writeLight = SPINNING_CYCLES;
            break;
    }
    if (wr) {
        // Any address loads the latch in the sequencer's LD function
        // (write mode and load mode, 74LS323)
        if (m_writeMode && m_loadMode) m_latch = *wr;
        return 0;
    }
    // Only even addresses return the latch (UTAIIe table 9.1)
    return (addr & 1) ? -1 : m_latch;
}

// SetSequencerFunction: $C08C/$C08D load mode off/on, $C08E/$C08F write mode
void Disk2Card::setSequencer(uint16_t addr) {
    if ((addr & 0xF) < 0xC) return;
    switch ((addr & 3) ^ 2) {
        case 0: m_writeMode = false; break;
        case 1: m_writeMode = true; break;
        case 2: m_loadMode = false; break;
        case 3: m_loadMode = true; break;
    }
}

void Disk2Card::checkSpinning(bool stateChanged, uint64_t now) {
    Drive& d = m_drive[m_curr];
    const bool modeChanged = m_motorOn && !d.spinning;
    if (m_motorOn) d.spinning = SPINNING_CYCLES;
    if (modeChanged) m_lastCycle = now;
    if (m_motorOn && stateChanged) d.motorOnCycle = now;
}

void Disk2Card::controlMotor(uint16_t addr, uint64_t now) {
    const bool on = addr & 1;
    const bool changed = on != m_motorOn;
    // "DRIVES OFF forces the control flip-flops to clear" (UTAIIe 9-12)
    if (!on) m_magnets = 0;
    if (changed) m_motorOn = on;
    checkSpinning(changed, now);
}

void Disk2Card::controlStepper(uint16_t addr, uint64_t now) {
    Drive& d = m_drive[m_curr];
    if (!m_motorOn && !d.spinning) return;
    const int bit = 1 << ((addr >> 1) & 3);
    if (addr & 1) m_magnets |= bit;
    else m_magnets &= ~bit;
    if (m_deferred) {
        m_deferred = false;
        int delta = (m_deferredAddr & 7) - (addr & 7);
        if (delta < 0) delta = -delta;
        // Two adjacent magnets off in quick succession don't move the cog
        if ((delta == 2 || delta == 6) && !(addr & 1)) return;
        stepperDeferred();          // the earlier change first
    }
    m_deferredAddr = addr;
    m_deferredAt = now;
    m_deferred = true;
}

// The head moves toward an adjacent magnet that is on, unless the magnets
// on both sides are on; one half track at a time
void Disk2Card::stepperDeferred() {
    m_deferred = false;
    Drive& d = m_drive[m_curr];
    int dir = 0;
    if (m_magnets & (1 << ((d.phase + 1) & 3))) dir += 1;
    if (m_magnets & (1 << ((d.phase + 3) & 3))) dir -= 1;
    const int np = std::clamp(d.phase + dir, 0, 79);
    if (np != d.phase) {
        flush(m_curr);
        d.phase = np;
        d.disk.trackData = false;
    }
}

void Disk2Card::enable(uint16_t addr, uint64_t now) {
    const int nd = addr & 1;
    const bool changed = nd != m_curr;
    m_curr = nd;
    m_drive[!m_curr].spinning = 0;
    m_drive[!m_curr].writeLight = 0;
    checkSpinning(changed, now);
}

void Disk2Card::readTrack(int drive) {
    Floppy& f = m_drive[drive].disk;
    const int track = m_drive[drive].phase >> 1;
    if (!f.image || track >= f.image->tracks()) { f.trackData = false; return; }
    f.nibbles = f.image->readTrack(track, f.track.data());
    f.byte = 0;
    f.trackData = f.nibbles != 0;
}

void Disk2Card::flush(int drive) {
    Floppy& f = m_drive[drive].disk;
    if (f.image && f.dirty) f.image->writeTrack(m_drive[drive].phase >> 1, f.track.data(), f.nibbles);
    f.dirty = false;
}

// ReadWrite ($C08C): the disk has turned a nibble every 32 cycles since the
// last access; a read takes the nibble under the head (part of it if within
// 6 cycles of the last read), a write puts the latch there
void Disk2Card::readWrite(uint64_t now) {
    Drive& d = m_drive[m_curr];
    Floppy& f = d.disk;
    if (!f.trackData && f.image) readTrack(m_curr);
    if (!f.trackData) {
        // An empty drive: noise once the sequencer settles (GH#748, GH#864)
        if (now - d.motorOnCycle < (uint64_t)MOTOR_ON_UNTIL_LSS_STABLE_CYCLES) m_latch = 0x80;
        else { m_rand = m_rand * 1103515245u + 12345u; m_latch = (uint8_t)(m_rand >> 16); }
        return;
    }
    if (d.spinning) {
        const uint64_t diff = now - m_lastCycle;
        m_lastCycle = now;
        if (diff > 40) {
            f.byte += (int)((diff >> 5) % (uint64_t)f.nibbles);
            if (f.byte >= f.nibbles) f.byte -= f.nibbles;
        }
    }
    if (!m_writeMode) {
        // "DRIVES OFF forces the data register to hold its present state"
        if (!d.spinning) return;
        const uint64_t rd = now - m_lastReadLatch;
        if (rd <= 6) {
            m_latch = (uint8_t)(f.track[f.byte] >> (8 - rd / 4));
            return;
        }
        m_latch = f.track[f.byte];
        m_lastReadLatch = now;
    } else if (!f.image->writeProtected()) {
        if (!d.spinning) return;
        f.track[f.byte] = m_latch;
        f.dirty = true;
    }
    if (++f.byte >= f.nibbles) f.byte = 0;
}

// $C08D: the write-protect switch shifted into the latch (saturated: $FF
// protected, $00 not)
void Disk2Card::loadWriteProtect() {
    if (!m_drive[m_curr].spinning) return;
    const Disk2Image* img = m_drive[m_curr].disk.image.get();
    m_latch = (img && img->writeProtected()) ? 0xFF : 0x00;
}

bool Disk2Card::insert(int drive, std::unique_ptr<Disk2Image> image) {
    drive &= 1;
    ejectDisk(drive + 1);
    Floppy& f = m_drive[drive].disk;
    f.image = std::move(image);
    f.trackData = false;
    f.dirty = false;
    f.byte = 0;
    return f.image != nullptr;
}

bool Disk2Card::mountDisk(int unit, const std::string& path) {
    if (unit != 1 && unit != 2) return false;
    auto img = Disk2Image::open(path);
    if (!img) return false;
    return insert(unit - 1, std::move(img));
}

void Disk2Card::ejectDisk(int unit) {
    if (unit != 1 && unit != 2) return;
    flush(unit - 1);
    Floppy& f = m_drive[unit - 1].disk;
    f.image.reset();
    f.trackData = false;
}

bool Disk2Card::getDiskStatus(int unit, int& track, int& sector, bool& led) {
    if (unit != 1 && unit != 2) return false;
    const Drive& d = m_drive[unit - 1];
    track = d.phase >> 1;
    sector = 0;
    led = d.spinning > 0 && m_curr == unit - 1;
    return true;
}

std::string Disk2Card::getMountedDiskPath(int unit) {
    if (unit != 1 && unit != 2) return "";
    const Floppy& f = m_drive[unit - 1].disk;
    if (!f.image) return "";
    return f.image->path().empty() ? "(memory)" : f.image->path();
}

}  // namespace apple2
