#include "ieee488_bus.h"
#include "libdevices/main/device_info.h"
#include <sys/stat.h>
#include <cstdio>

Ieee488Bus::Ieee488Bus()
    : m_ndacOut(this, &Lines::ndac), m_davOut(this, &Lines::dav), m_eoiOut(this, &Lines::eoi),
      m_atnLine(this), m_dataIn(this), m_dataOut(this), m_via(this), m_eoiIn(this) {
    m_host.setDirectoryLoadAddress(0x0401);     // a Commodore drive's directory (PET BASIC start)
}

void Ieee488Bus::reset() {
    m_ctl = Lines{};
    m_drv = Lines{};
    m_listener = m_talker = false;
    m_lastWasOurListen = m_lastWasOurTalk = false;
    m_prevAtn = true;
    m_byteTaken = m_byteOnBus = m_talkEnded = m_hasPending = m_tentative = false;
    m_dos->reset();
}

ISignalLine* Ieee488Bus::getSignalLine(const char* name) {
    std::string n(name);
    if (n == "ndacOut") return &m_ndacOut;
    if (n == "davOut")  return &m_davOut;
    if (n == "eoiOut")  return &m_eoiOut;
    if (n == "atn")     return &m_atnLine;
    return nullptr;
}

IPortDevice* Ieee488Bus::namedPort(const std::string& n) {
    if (n == "dataIn")  return &m_dataIn;
    if (n == "dataOut") return &m_dataOut;
    if (n == "via")     return &m_via;
    if (n == "eoiIn")   return &m_eoiIn;
    return nullptr;
}

// ---------------------------------------------------------------------------
// The computer's side
// ---------------------------------------------------------------------------

void Ieee488Bus::DataOutPort::update() {
    // Input bits do not drive the bus (VICE: all lines high in input mode)
    m_bus->setCtlData((uint8_t)(m_val | ~m_ddr));
}

uint8_t Ieee488Bus::ViaPort::readPort() {
    uint8_t v = 0xFF;
    if (!m_bus->ndac()) v &= (uint8_t)~0x01;
    if (!m_bus->nrfd()) v &= (uint8_t)~0x40;
    if (!m_bus->dav())  v &= (uint8_t)~0x80;
    return v;
}

void Ieee488Bus::ViaPort::update() {
    // PB1 NRFD out, PB2 ATN out (an input bit leaves the line released)
    m_bus->setCtl(&Lines::nrfd, !(m_ddr & 0x02) || (m_val & 0x02));
    m_bus->setCtl(&Lines::atn,  !(m_ddr & 0x04) || (m_val & 0x04));
}

void Ieee488Bus::setCtl(bool Lines::*line, bool level) {
    if (m_ctl.*line == level) return;
    m_ctl.*line = level;
    step();
}

void Ieee488Bus::setCtlData(uint8_t v) {
    if (m_ctl.data == v) return;
    m_ctl.data = v;
    step();
}

// ---------------------------------------------------------------------------
// The drive
// ---------------------------------------------------------------------------

void Ieee488Bus::releaseAll() {
    // A byte on the bus or only tentatively taken is sent again (m_hasPending)
    if (m_byteOnBus || m_tentative) m_talkEnded = false;
    m_tentative = false;
    m_drv = Lines{};
    m_byteTaken = m_byteOnBus = false;
}

void Ieee488Bus::step() {
    if (m_stepping) return;                     // a change made while stepping
    m_stepping = true;
    for (int i = 0; i < 16; ++i) {
        Lines before = m_drv;
        bool atnNow = atn();
        if (atnNow != m_prevAtn) {
            m_prevAtn = atnNow;
            if (!atnNow) {
                // ATN asserted: every device listens to the command bytes
                releaseAll();
                m_drv.ndac = false;
            } else {
                // ATN released: listen for data, talk, or stay off the bus
                releaseAll();
                if (m_listener) m_drv.ndac = false;
            }
        }
        if (!atnNow || m_listener) listenerStep();
        else if (m_talker) talkerStep();
        if (before.atn == m_drv.atn && before.dav == m_drv.dav && before.eoi == m_drv.eoi &&
            before.nrfd == m_drv.nrfd && before.ndac == m_drv.ndac && before.data == m_drv.data)
            break;
    }
    m_stepping = false;
}

void Ieee488Bus::listenerStep() {
    if (!m_byteTaken) {
        m_drv.nrfd = true;                      // ready for a byte
        m_drv.ndac = false;
        if (!dav()) {
            // The talker presents a byte (the lines are inverted)
            uint8_t b = (uint8_t)~data();
            m_drv.nrfd = false;                 // busy
            if (!atn()) command(b);
            else        (void)m_dos->ciout(b);
            m_byteTaken = true;
            m_drv.ndac = true;                  // accepted
        }
    } else if (dav()) {
        // The talker took DAV away: get ready for the next byte
        m_byteTaken = false;
        m_drv.ndac = false;
        m_drv.nrfd = true;
    }
}

void Ieee488Bus::talkerStep() {
    m_drv.nrfd = m_drv.ndac = true;             // the computer listens
    if (m_byteOnBus) {
        if (!ndac()) return;
        // NDAC released: take the byte away. A busy listener (NRFD low) took
        // it. NDAC released while NRFD is high is how the PET lets go of the
        // bus before UNTALK: the byte only counts if the listener goes on to
        // the next one; if ATN comes first it is sent again at the next TALK,
        // as Commodore's drives do.
        m_drv.dav = true;
        m_drv.eoi = true;
        m_drv.data = 0xFF;
        m_byteOnBus = false;
        if (!nrfd()) m_hasPending = false;
        else         m_tentative = true;
        return;
    }
    if (m_talkEnded || !nrfd() || ndac()) return;   // the listener is not ready
    if (m_tentative) {                          // it went on: the last byte arrived
        m_tentative = false;
        m_hasPending = false;
    }
    uint8_t b = 0;
    int s;
    if (m_hasPending) { b = m_pendingByte; s = m_pendingStatus; }   // not taken last time
    else              s = m_dos->acptr(&b);
    if (s == -2 || s == 0x42) {
        // Nothing to send (no file): stay silent, as Commodore's drives do;
        // the computer's read times out and it reports FILE NOT FOUND
        m_talkEnded = true;
        return;
    }
    m_drv.data = (uint8_t)~b;
    m_drv.eoi = !(s & 0x40);                    // EOI with the last byte
    m_drv.dav = false;
    m_byteOnBus = true;
    m_hasPending = true;                        // until accepted
    m_pendingByte = b;
    m_pendingStatus = s;
    if (s & 0x40) m_talkEnded = true;
}

void Ieee488Bus::command(uint8_t b) {
    int unit = m_dos->unit();
    if (((b >= 0x20 && b <= 0x3E) || (b >= 0x40 && b <= 0x5E)) && (b & 0x1F) == unit)
        m_dos->ensureInit();                     // resolve the folder at first use
    if (b >= 0x20 && b <= 0x3E) {               // LISTEN
        m_lastWasOurListen = (b & 0x1F) == unit;
        m_lastWasOurTalk = false;
        if (m_lastWasOurListen && m_dos->listen(b) != -2) m_listener = true;
    } else if (b == 0x3F) {                     // UNLISTEN
        if (m_listener) { m_dos->unlsn(); m_listener = false; }
        m_lastWasOurListen = m_lastWasOurTalk = false;
    } else if (b >= 0x40 && b <= 0x5E) {        // TALK (another talker untalks us)
        bool ours = (b & 0x1F) == unit;
        if (ours) {
            if (m_dos->talk(b) != -2) { m_talker = true; m_talkEnded = false; }
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

// ---------------------------------------------------------------------------
// Drive management
// ---------------------------------------------------------------------------

bool Ieee488Bus::mountDisk(int unit, const std::string& path) {
    if (unit != m_dos->unit()) return false;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    if (S_ISDIR(st.st_mode)) {                  // a host folder
        m_host.setRoot(path);
        m_dos = &m_host;
        m_imageMounted = false;
    } else {                                    // a disk image
        m_image.setUnit(unit);
        if (!m_image.open(path)) return false;
        m_dos = &m_image;
        m_imageMounted = true;
    }
    m_listener = m_talker = false;
    m_hasPending = m_tentative = m_talkEnded = false;
    return true;
}

void Ieee488Bus::ejectDisk(int unit) {
    if (unit != m_dos->unit()) return;
    // No image: back to the host folder (the directory mmemu runs in)
    m_dos = &m_host;
    m_imageMounted = false;
    m_host.setRoot(".");
}

bool Ieee488Bus::getDiskStatus(int unit, int& track, int& sector, bool& led) {
    if (unit != m_dos->unit()) return false;
    track = sector = 0;
    led = m_dos->active();
    return true;
}

std::string Ieee488Bus::getMountedDiskPath(int unit) {
    if (unit != m_dos->unit()) return std::string();
    return m_imageMounted ? m_image.path() : m_host.rootPath();
}

void Ieee488Bus::getDeviceInfo(DeviceInfo& out) const {
    out.name = m_name;
    out.baseAddr = 0;
    out.addrMask = 0;
    auto lvl = [](bool high) { return std::string(high ? "high" : "LOW"); };
    out.state.push_back({"ATN", lvl(atn())});
    out.state.push_back({"DAV", lvl(dav())});
    out.state.push_back({"NRFD", lvl(nrfd())});
    out.state.push_back({"NDAC", lvl(ndac())});
    out.state.push_back({"EOI", lvl(eoi())});
    char buf[8];
    std::snprintf(buf, sizeof buf, "$%02X", data());
    out.state.push_back({"Data lines", buf});
    out.state.push_back({"Drive", "unit " + std::to_string(m_dos->unit()) +
                                  (m_imageMounted ? ": disk image " + m_image.path()
                                                  : ": host folder " + m_host.rootPath())});
    out.state.push_back({"Drive role", m_talker ? "talker" : m_listener ? "listener" : "idle"});
}
