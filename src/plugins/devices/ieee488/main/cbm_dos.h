#pragma once

// A Commodore DOS unit at the bus command level (Commodore bus L2): what a
// drive does with LISTEN/TALK, secondary addresses and data bytes. A host
// folder (CbmHostDos) and a disk image (CbmImageDos) implement it.

#include <cstdint>
#include <deque>
#include <string>

class ICbmDos {
public:
    virtual ~ICbmDos() = default;

    /** The bus calls return >= 0 a STATUS value (0x40 EOI, 0x42 nothing to
     *  read), -1 handled without STATUS, -2 not for this unit. */
    virtual int listen(uint8_t a) = 0;
    virtual int second(uint8_t a) = 0;
    virtual int ciout(uint8_t a) = 0;
    virtual int unlsn() = 0;
    virtual int talk(uint8_t a) = 0;
    virtual int tksa(uint8_t a) = 0;
    virtual int acptr(uint8_t* a) = 0;
    virtual int untlk() = 0;

    virtual int unit() const = 0;
    /** Close every channel (drive reset). */
    virtual void reset() = 0;
    /** Ready to serve (e.g. a host folder resolved). */
    virtual bool ensureInit() { return true; }
    /** Busy: the drive's activity LED. */
    virtual bool active() const = 0;

    // 1571 burst commands ("U0" + binary bytes, Commodore's burst command
    // set): the bytes the drive sends on fast serial (status, sector data),
    // and a burst write: bytes from the host until a sector is complete,
    // then its status.
    std::deque<uint8_t>& burstOut() { return m_burstOut; }
    virtual bool burstWriting() const { return false; }
    /** A byte of burst write data; true when the sector's 256 are in. */
    virtual bool burstWriteByte(uint8_t) { return false; }
    /** Write the sector just received and queue its status. */
    virtual void burstFinishSector() {}
    virtual void burstCancel() { m_burstOut.clear(); m_fastloadPending = false; }
    /** A burst fastload ("U0" $1F + name) to serve: the drive reads the
     *  file through the DOS and sends it (IecDrive). */
    bool takeFastload(std::string& name) {
        if (!m_fastloadPending) return false;
        m_fastloadPending = false;
        name = m_fastload;
        return true;
    }

protected:
    std::deque<uint8_t> m_burstOut;
    std::string m_fastload;
    bool m_fastloadPending = false;
};
