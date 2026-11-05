#pragma once

// A Commodore DOS unit at the bus command level (Commodore bus L2): what a
// drive does with LISTEN/TALK, secondary addresses and data bytes. A host
// folder (CbmHostDos) and a disk image (CbmImageDos) implement it.

#include <cstdint>

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
};
