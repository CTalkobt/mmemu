#include "iec_drive.h"
#include "libdevices/main/device_info.h"
#include <sys/stat.h>
#include <algorithm>
#include <vector>

// Bit phases the drive holds as talker. The Plus/4 KERNAL polls CLK every
// 10-15 us and a TED bad line can stall it ~45 us more, so each phase is
// held longer than that (a 1541 holds its bits for 60-80 us).
static constexpr double BIT_SETUP_US = 80.0;   // CLK low, the bit on DATA
static constexpr double BIT_VALID_US = 80.0;   // CLK high, the bit valid
static constexpr double TBB_US = 100.0;        // between bytes / after turnaround
static constexpr double EOI_US = 200.0;        // talker ready but silent: EOI
static constexpr double EOI_ACK_US = 60.0;     // the listener's EOI acknowledge pulse
static constexpr double FRAME_US = 1000.0;     // a byte must be acknowledged within 1 ms

IecDrive::IecDrive(int unit) {
    m_host.setUnit(unit);
    m_image.setUnit(unit);
    m_host.setDirectoryLoadAddress(0x0401);     // as Commodore's drives send it
    m_name = "Drive" + std::to_string(unit);
}

void IecDrive::reset() {
    m_st = St::Idle;
    m_dClk = m_dData = false;
    m_cAtn = m_cClk = m_cData = false;
    m_prevAtn = m_prevClk = false;
    m_listener = m_talker = false;
    m_lastWasOurListen = m_lastWasOurTalk = false;
    m_hasPending = false;
    m_pendingChannel = -1;
    m_dos->reset();
}

void IecDrive::setComputerLines(bool atn, bool clk, bool data) {
    m_cAtn = atn;
    m_cClk = clk;
    m_cData = data;
    step();                                     // follow edges as they happen
}

void IecDrive::tick(uint64_t cycles) {
    m_now += cycles;
    step();
}

// ---------------------------------------------------------------------------
// The protocol
// ---------------------------------------------------------------------------

void IecDrive::atnStart() {
    // A fast computer shifted a byte out just before ATN; a burst stops
    m_hostFast = m_fastToHost && m_fastAnnounce;
    m_fastAnnounce = false;
    m_dos->burstCancel();
    // Every device listens under ATN: release CLK, pull DATA (present)
    m_dClk = false;
    m_dData = true;
    m_underAtn = true;
    m_prevClk = busClk();
    enter(St::RxWaitReady);
}

void IecDrive::atnEnd() {
    m_underAtn = false;
    // A burst command (run at UNLISTEN): sectors and status on fast serial
    std::string name;
    if (m_fastToHost && m_dos->takeFastload(name)) fastload(name);
    if (m_fastToHost && (!m_dos->burstOut().empty() || m_dos->burstWriting())) {
        m_dClk = m_dData = false;
        enter(St::BurstArm);
        return;
    }
    if (m_talker) {
        enter(St::Turnaround);                  // still holding DATA (the last ack)
    } else if (m_listener) {
        m_dData = true;
        enter(St::RxWaitReady);
    } else {
        m_dClk = m_dData = false;
        enter(St::Idle);
    }
}

void IecDrive::step() {
    if (m_cAtn != m_prevAtn) {
        m_prevAtn = m_cAtn;
        if (m_cAtn) atnStart(); else atnEnd();
    }
    bool clk = busClk();
    bool clkRose = m_prevClk && !clk;           // released: a bit is valid
    m_prevClk = clk;

    switch (m_st) {
    case St::Idle:
    case St::Silent:
        m_dClk = m_dData = false;
        break;

    // --- Listener -----------------------------------------------------------
    case St::RxWaitReady:
        // Hold DATA until the talker, having pulled CLK, lets it go (an
        // edge: right after ATN the computer may not have pulled CLK yet)
        m_dData = true;
        if (clkRose) {
            m_dData = false;                    // ready for data
            m_eoi = false;
            enter(St::RxWaitStart);
            if (fast()) m_fastToHost(0x00);     // 1571: tell the C128 we are fast
        }
        break;
    case St::RxWaitStart:
        if (clk) {
            m_bits = 0;
            m_byte = 0;
            enter(St::RxBits);
        } else if (!m_eoi && us() > EOI_US) {
            m_eoi = true;                       // the talker signals the last byte
            m_dData = true;
            enter(St::RxEoiAck);
        }
        break;
    case St::RxEoiAck:
        if (us() >= EOI_ACK_US) {
            m_dData = false;
            enter(St::RxWaitStart);
        }
        break;
    case St::RxBits:
        if (clkRose) {                          // LSB first; DATA high = 1
            m_byte = (uint8_t)((m_byte >> 1) | (busData() ? 0x00 : 0x80));
            if (++m_bits == 8) enter(St::RxFrameEnd);
        }
        break;
    case St::RxFrameEnd:
        if (clk) {
            m_dData = true;                     // acknowledge the byte
            uint8_t b = m_byte;
            enter(St::RxWaitReady);
            if (m_underAtn) command(b); else received(b);
        }
        break;

    // --- Burst (1571) --------------------------------------------------------
    case St::BurstArm:
        m_dClk = m_dData = false;
        if (!m_cClk) {                          // the computer has let CLK go
            m_burstClk = false;
            if (m_dos->burstWriting()) startBurstSector();
            else enter(St::BurstOut);
        }
        break;
    case St::BurstOut:
        if (m_cClk != m_burstClk) {             // a toggle: the next byte
            m_burstClk = m_cClk;
            auto& q = m_dos->burstOut();
            if (!q.empty()) { m_fastToHost(q.front()); q.pop_front(); }
        }
        break;
    case St::BurstIn:
        break;                                  // bytes arrive in fastByteFromHost
    case St::BurstStatus:
        if (m_cClk) {                           // the computer wants the status
            m_dos->burstFinishSector();
            auto& q = m_dos->burstOut();
            if (!q.empty()) { m_fastToHost(q.front()); q.pop_front(); }
            enter(St::BurstDone);
        }
        break;
    case St::BurstDone:
        m_dClk = false;
        if (!m_cClk) {
            if (m_dos->burstWriting()) startBurstSector();   // the next sector
            else enter(St::Silent);
        }
        break;

    // --- Talker -------------------------------------------------------------
    case St::Turnaround:
        // The computer pulls DATA and lets CLK go; the drive takes CLK
        if (!m_cClk) {
            m_dClk = true;
            m_dData = false;
            enter(St::TxStart);
        }
        break;
    case St::TxStart:
        m_dClk = true;
        m_dData = false;
        if (busData() && us() >= TBB_US) {      // the listener is there
            uint8_t b = 0;
            int s;
            if (m_hasPending) { b = m_pendingByte; s = m_pendingStatus; }
            else              s = m_dos->acptr(&b);
            if (s == -2 || s == 0x42) {
                // Nothing to send (no file): ready, then never a byte; the
                // computer's read times out (FILE NOT FOUND)
                m_hasPending = false;
                enter(St::Silent);
                break;
            }
            m_hasPending = true;                // until acknowledged
            m_pendingByte = b;
            m_pendingStatus = s;
            m_txByte = b;
            m_txLast = (s & 0x40) != 0;
            m_dClk = false;                     // ready to send
            enter(St::TxWaitListener);
        }
        break;
    case St::TxWaitListener:
        if (!busData()) {                       // the listener is ready
            m_bits = 0;
            if (m_txLast) enter(St::TxEoiWait);
            else if (fast()) { m_dClk = true; m_fastToHost(m_txByte); enter(St::TxFrame); }
            else enter(St::TxBitSetup);
        }
        break;
    case St::TxEoiWait:
        if (busData()) enter(St::TxEoiRelease); // EOI acknowledged
        break;
    case St::TxEoiRelease:
        if (!busData()) {
            m_bits = 0;
            if (fast()) { m_dClk = true; m_dData = false; m_fastToHost(m_txByte); enter(St::TxFrame); }
            else enter(St::TxBitSetup);
        }
        break;
    case St::TxBitSetup:
        m_dClk = true;
        m_dData = !((m_txByte >> m_bits) & 1);  // pull DATA for a 0
        if (us() >= BIT_SETUP_US) {
            m_dClk = false;
            enter(St::TxBitValid);
        }
        break;
    case St::TxBitValid:
        if (us() >= BIT_VALID_US) {
            if (++m_bits == 8) {
                m_dClk = true;
                m_dData = false;
                enter(St::TxFrame);
            } else {
                enter(St::TxBitSetup);
                m_dClk = true;
                m_dData = !((m_txByte >> m_bits) & 1);
            }
        }
        break;
    case St::TxFrame:
        if (busData()) {                        // acknowledged
            m_hasPending = false;
            if (m_txLast) {
                m_dClk = m_dData = false;
                enter(St::Silent);
            } else {
                enter(St::TxStart);
            }
        } else if (us() > FRAME_US) {
            m_dClk = m_dData = false;           // not taken: keep it for next time
            enter(St::Silent);
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Fast serial (1571)
// ---------------------------------------------------------------------------

void IecDrive::fastByteFromHost(uint8_t b) {
    if (!m_fastToHost) return;
    if (m_st == St::BurstIn) {                  // burst write data
        if (m_dos->burstWriteByte(b)) enter(St::BurstStatus);
        else m_dClk = !m_dClk;                  // ask for the next byte
        return;
    }
    if (m_hostFast && !m_cAtn && m_listener && (m_st == St::RxWaitStart || m_st == St::RxBits)) {
        m_dData = true;                         // a data byte: acknowledge it
        enter(St::RxWaitReady);
        received(b);
        return;
    }
    m_fastAnnounce = true;                      // the C128 says it is fast (before ATN)
}

// Burst fastload (the C128 KERNAL's LOAD from a 1571, $F3A1): the file read
// through the DOS, then for each 254-byte block status 0 and the bytes; the
// last block status $1F, a count and the bytes (in a one-block file the
// count leaves out the load address, which comes first); no file: status 2
void IecDrive::fastload(const std::string& name) {
    const int u = m_dos->unit();
    std::vector<uint8_t> data;
    m_dos->listen((uint8_t)(0x20 | u));
    m_dos->second(0xF0);                        // OPEN on channel 0
    for (char c : name) m_dos->ciout((uint8_t)c);
    m_dos->unlsn();
    m_dos->talk((uint8_t)(0x40 | u));
    m_dos->tksa(0x60);
    for (size_t guard = 0; guard < (1u << 24); ++guard) {
        uint8_t b = 0;
        int st = m_dos->acptr(&b);
        if (st < 0 || (st & 0x02)) break;       // nothing there
        data.push_back(b);
        if (st & 0x40) break;                   // EOI: the last byte
    }
    m_dos->untlk();
    m_dos->listen((uint8_t)(0x20 | u));
    m_dos->second(0xE0);                        // CLOSE
    m_dos->unlsn();
    auto& q = m_dos->burstOut();
    q.clear();
    if (data.size() < 2) { q.push_back(0x02); return; }   // file not found
    for (size_t at = 0; at < data.size(); at += 254) {
        size_t n = std::min<size_t>(254, data.size() - at);
        if (at + n >= data.size()) {            // the last block
            q.push_back(0x1F);
            q.push_back((uint8_t)(at == 0 ? n - 2 : n));
        } else {
            q.push_back(0x00);
        }
        q.insert(q.end(), data.begin() + (long)at, data.begin() + (long)(at + n));
    }
}

void IecDrive::startBurstSector() {
    m_dClk = true;                              // ask for the first byte
    enter(St::BurstIn);
}

void IecDrive::command(uint8_t b) {
    int unit = m_dos->unit();
    if (((b >= 0x20 && b <= 0x3E) || (b >= 0x40 && b <= 0x5E)) && (b & 0x1F) == unit)
        m_dos->ensureInit();                    // resolve the folder at first use
    if (b >= 0x20 && b <= 0x3E) {               // LISTEN
        m_lastWasOurListen = (b & 0x1F) == unit;
        m_lastWasOurTalk = false;
        if (m_lastWasOurListen && m_dos->listen(b) != -2) m_listener = true;
        if (m_talker && m_lastWasOurListen) { m_dos->untlk(); m_talker = false; }
    } else if (b == 0x3F) {                     // UNLISTEN
        if (m_listener) { m_dos->unlsn(); m_listener = false; }
        m_lastWasOurListen = m_lastWasOurTalk = false;
    } else if (b >= 0x40 && b <= 0x5E) {        // TALK (another talker untalks us)
        bool ours = (b & 0x1F) == unit;
        if (ours) {
            if (m_dos->talk(b) != -2) m_talker = true;
        } else if (m_talker) {
            m_dos->untlk();
            m_talker = false;
        }
        m_lastWasOurTalk = ours;
        m_lastWasOurListen = false;
    } else if (b == 0x5F) {                     // UNTALK
        if (m_talker) { m_dos->untlk(); m_talker = false; }
        m_lastWasOurListen = m_lastWasOurTalk = false;
    } else if (b >= 0x60) {                     // secondary: $6x data, $Ex close, $Fx open
        if (m_lastWasOurListen) {
            m_dos->second(b);
            m_hasPending = false;               // a new open, close or write: the old byte is gone
        } else if (m_lastWasOurTalk) {
            if ((b & 0x0F) != m_pendingChannel) m_hasPending = false;
            m_pendingChannel = b & 0x0F;
            m_dos->tksa(b);
        }
    }
}

void IecDrive::received(uint8_t b) {
    if (m_listener) m_dos->ciout(b);
}

// ---------------------------------------------------------------------------
// Drive management
// ---------------------------------------------------------------------------

bool IecDrive::mountDisk(int unit, const std::string& path) {
    if (unit != m_dos->unit()) return false;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    if (S_ISDIR(st.st_mode)) {                  // a host folder
        m_host.setRoot(path);
        m_dos = &m_host;
        m_imageMounted = false;
    } else {                                    // a disk image
        if (!m_image.open(path)) return false;
        m_dos = &m_image;
        m_imageMounted = true;
    }
    m_listener = m_talker = false;
    m_hasPending = false;
    return true;
}

void IecDrive::ejectDisk(int unit) {
    if (unit != m_dos->unit()) return;
    m_dos = &m_host;                            // back to the folder mmemu runs in
    m_imageMounted = false;
    m_host.setRoot(".");
}

bool IecDrive::getDiskStatus(int unit, int& track, int& sector, bool& led) {
    if (unit != m_dos->unit()) return false;
    track = sector = 0;
    led = m_dos->active();
    return true;
}

std::string IecDrive::getMountedDiskPath(int unit) {
    if (unit != m_dos->unit()) return std::string();
    return m_imageMounted ? m_image.path() : m_host.rootPath();
}

void IecDrive::getDeviceInfo(DeviceInfo& out) const {
    out.name = m_name;
    out.baseAddr = 0;
    out.addrMask = 0;
    auto lvl = [](bool high) { return std::string(high ? "high" : "LOW"); };
    out.state.push_back({"ATN", lvl(atnLine())});
    out.state.push_back({"CLK", lvl(clkLine())});
    out.state.push_back({"DATA", lvl(dataLine())});
    out.state.push_back({"Drive", "unit " + std::to_string(m_dos->unit()) +
                                  (m_imageMounted ? ": disk image " + m_image.path()
                                                  : ": host folder " + m_host.rootPath())});
    out.state.push_back({"Drive role", m_talker ? "talker" : m_listener ? "listener" : "idle"});
}
