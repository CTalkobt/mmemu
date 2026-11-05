#pragma once

// An IEEE-488 (GPIB) bus as the Commodore PET wires it, with a disk drive on
// it that serves a host folder (CbmHostDos).
//
// PET wiring (VICE petpia1.c, petpia2.c, petvia.c; the lines are active low
// and wired-AND on the bus):
//   PIA2 port A ($E820)  data in        PIA2 port B ($E822)  data out
//   PIA2 CA2             NDAC out       PIA2 CB2             DAV out
//   PIA2 CA1             ATN in         PIA1 CA2             EOI out
//   PIA1 PA6             EOI in         VIA PB1 / PB2        NRFD / ATN out
//   VIA PB0 / PB6 / PB7  NDAC / NRFD / DAV in
//
// The drive answers the three-wire handshake (DAV/NRFD/NDAC) as listener and
// talker; under ATN it takes LISTEN/TALK/UNLISTEN/UNTALK and secondary
// addresses, and passes them to CbmHostDos as the bus calls.

#include "libdevices/main/io_handler.h"
#include "libdevices/main/iport_device.h"
#include "libdevices/main/isignal_line.h"
#include "cbm_host_dos.h"
#include <cstdint>
#include <string>

class Ieee488Bus : public IOHandler {
public:
    Ieee488Bus();
    ~Ieee488Bus() override = default;

    // IOHandler (no registers of its own)
    const char* name() const override { return m_name.c_str(); }
    void        setName(const std::string& n) override { m_name = n; }
    uint32_t    baseAddr() const override { return 0; }
    uint32_t    addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override;
    void tick(uint64_t) override {}
    void getDeviceInfo(DeviceInfo& out) const override;

    /**
     * Wiring endpoints (machine JSON "lines" / "portWiring"):
     *  signal lines: "ndacOut", "davOut", "eoiOut" (the computer drives them),
     *                "atn" (the bus ATN level, e.g. for PIA2 CA1);
     *  ports: "dataIn" (reads the bus), "dataOut" (writes the computer's
     *         data), "via" (PB0 NDAC, PB6 NRFD, PB7 DAV in; PB1 NRFD, PB2 ATN
     *         out), "eoiIn" (bit 6 = EOI).
     */
    ISignalLine* getSignalLine(const char* name) override;
    IPortDevice* namedPort(const std::string& name) override;

    // The drive: a host folder as unit 8 (CLI `disk mount 8 <folder>`)
    bool mountDisk(int unit, const std::string& path) override;
    void ejectDisk(int unit) override;
    bool getDiskStatus(int unit, int& track, int& sector, bool& led) override;
    std::string getMountedDiskPath(int unit) override;

    /** Bus levels (true = high, released). Data: the lines as they are (active low). */
    bool atn() const  { return m_ctl.atn  && m_drv.atn; }
    bool dav() const  { return m_ctl.dav  && m_drv.dav; }
    bool eoi() const  { return m_ctl.eoi  && m_drv.eoi; }
    bool nrfd() const { return m_ctl.nrfd && m_drv.nrfd; }
    bool ndac() const { return m_ctl.ndac && m_drv.ndac; }
    uint8_t data() const { return (uint8_t)(m_ctl.data & m_drv.data); }

    CbmHostDos& dos() { return m_dos; }

private:
    struct Lines {
        bool atn = true, dav = true, eoi = true, nrfd = true, ndac = true;
        uint8_t data = 0xFF;
    };

    // A line the computer drives (a PIA's CA2/CB2)
    class OutLine : public ISignalLine {
    public:
        OutLine(Ieee488Bus* bus, bool Lines::*line) : m_bus(bus), m_line(line) {}
        bool get() const override { return m_bus->m_ctl.*m_line; }
        void set(bool level) override { m_bus->setCtl(m_line, level); }
        void pulse() override { set(false); set(true); }
    private:
        Ieee488Bus* m_bus;
        bool Lines::*m_line;
    };
    // ATN as seen on the bus (PIA2 CA1)
    class AtnLine : public ISignalLine {
    public:
        explicit AtnLine(Ieee488Bus* bus) : m_bus(bus) {}
        bool get() const override { return m_bus->atn(); }
        void set(bool) override {}
        void pulse() override {}
    private:
        Ieee488Bus* m_bus;
    };

    class DataInPort : public IPortDevice {
    public:
        explicit DataInPort(Ieee488Bus* bus) : m_bus(bus) {}
        uint8_t readPort() override { return m_bus->data(); }
        void writePort(uint8_t) override {}
        void setDdr(uint8_t) override {}
    private:
        Ieee488Bus* m_bus;
    };
    class DataOutPort : public IPortDevice {
    public:
        explicit DataOutPort(Ieee488Bus* bus) : m_bus(bus) {}
        uint8_t readPort() override { return 0xFF; }
        void writePort(uint8_t v) override { m_val = v; update(); }
        void setDdr(uint8_t d) override { m_ddr = d; update(); }
    private:
        void update();
        Ieee488Bus* m_bus;
        uint8_t m_val = 0xFF, m_ddr = 0;
    };
    class ViaPort : public IPortDevice {
    public:
        explicit ViaPort(Ieee488Bus* bus) : m_bus(bus) {}
        uint8_t readPort() override;
        void writePort(uint8_t v) override { m_val = v; update(); }
        void setDdr(uint8_t d) override { m_ddr = d; update(); }
    private:
        void update();
        Ieee488Bus* m_bus;
        uint8_t m_val = 0xFF, m_ddr = 0;
    };
    class EoiInPort : public IPortDevice {
    public:
        explicit EoiInPort(Ieee488Bus* bus) : m_bus(bus) {}
        uint8_t readPort() override { return m_bus->eoi() ? 0xFF : 0xBF; }
        void writePort(uint8_t) override {}
        void setDdr(uint8_t) override {}
    private:
        Ieee488Bus* m_bus;
    };

    void setCtl(bool Lines::*line, bool level);
    void setCtlData(uint8_t v);
    /** The drive follows the bus until it settles. */
    void step();
    void listenerStep();
    void talkerStep();
    void command(uint8_t b);           // a byte under ATN
    void releaseAll();

    std::string m_name = "IEEE";
    Lines m_ctl, m_drv;                // the computer's outputs, the drive's
    OutLine m_ndacOut, m_davOut, m_eoiOut;
    AtnLine m_atnLine;
    DataInPort m_dataIn;
    DataOutPort m_dataOut;
    ViaPort m_via;
    EoiInPort m_eoiIn;

    // The drive
    CbmHostDos m_dos;
    bool m_listener = false;           // addressed to listen (data after ATN)
    bool m_talker = false;             // addressed to talk
    bool m_lastWasOurListen = false;   // a secondary address follows our LISTEN
    bool m_lastWasOurTalk = false;
    bool m_prevAtn = true;
    bool m_byteTaken = false;          // listener: holding the byte until DAV goes high
    bool m_byteOnBus = false;          // talker: a byte waiting to be accepted
    bool m_talkEnded = false;          // talker: sent EOI or found nothing more
    bool m_hasPending = false;         // talker: a byte read from the DOS, not yet accepted
    bool m_tentative = false;          // talker: taken away with the listener not busy
    uint8_t m_pendingByte = 0;
    int m_pendingStatus = 0;
    int m_pendingChannel = -1;         // its secondary address
    bool m_stepping = false;
};
