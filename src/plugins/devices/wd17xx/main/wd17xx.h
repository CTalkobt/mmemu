#pragma once

#include "libdevices/main/io_handler.h"
#include "floppy_disk.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class ISignalLine;

/**
 * Western Digital WD17xx / WD279x floppy disk controller (#330, #331), after
 * the FD179X-02 and FD1771 data sheets, XRoar's wd279x.c and MAME's
 * wd_fdc.cpp.
 *
 * Registers (address & 3): 0 status (read) / command (write), 1 track,
 * 2 sector, 3 data.
 *
 * Commands:
 *  - Type I: Restore ($0x), Seek ($1x), Step ($2x/$3x), Step In ($4x/$5x),
 *    Step Out ($6x/$7x). Bits: u (4, steps update the track register),
 *    h (3, load the head), V (2, verify: find an ID with the track register's
 *    track), r1 r0 (step rate). Rates 6/12/20/30 ms (179x, 1773), 12/12/20/40 ms
 *    (1771) at a 1 MHz controller clock, half that at 2 MHz (8" drives).
 *  - Type II: Read Sector ($8x/$9x), Write Sector ($Ax/$Bx). Bits: m (4,
 *    multiple sectors: the sector register counts up until Record Not Found),
 *    E (2, 30 ms head settle at 1 MHz), and per variant
 *      - 1791/1793/2791/2793/1773: S (3) side to compare, C (1) compare on;
 *      - 1795/1797/2795/2797: L (3, 1 = IBM lengths 128-1024, 0 = 256, 512,
 *        1024, 128), U (1, the side select output);
 *      - 1771: b (3, 1 = IBM lengths, 0 = 16 x the length byte), a1 a0 (write:
 *        the data mark $FB, $FA, $F9, $F8);
 *    a0 (0, write) on the 179x: $F8 (deleted) instead of $FB.
 *  - Type III: Read Address ($Cx: the six ID bytes; the track goes to the
 *    sector register), Read Track ($Ex: index to index, raw), Write Track
 *    ($Fx: index to index from the data register; MFM $F5 = $A1 sync with the
 *    CRC preset, $F6 = $C2, $F7 = two CRC bytes, $FE = an ID mark; FM $F7 =
 *    CRC, $F8-$FB data marks and $FE an ID mark with the CRC preset).
 *  - Type IV: Force Interrupt ($Dx): ends any command; I0 not-ready to ready,
 *    I1 ready to not-ready, I2 every index pulse, I3 at once (INTRQ held until
 *    the next Force Interrupt). $D0 ends without an interrupt.
 *
 * Status: busy (0) and, after a Type I command, index (1), track 0 (2), CRC
 * error (3), seek error (4), head loaded (5), write protect (6), not ready (7);
 * after Type II/III, DRQ (1), lost data (2), CRC error (3), record not found
 * (4), record type / write fault (5; on the 1771 bits 6-5 are the data mark:
 * 0 $FB, 1 $FA, 2 $F9, 3 $F8; on the 179x bit 5 is $F8/$F9, deleted), write
 * protect (6), not ready (7). Reading status clears INTRQ (not after $D8);
 * reading or writing data clears DRQ.
 *
 * Timing is in the machine's cycles (setClockHz): the disk turns at 300 RPM
 * (setRpm) with an index pulse each turn; a byte passes the head every
 * 32 us (MFM) or 64 us (FM) on a 5.25" track (scaled from the track's
 * length). A byte read before the CPU took the last one sets lost data (and
 * replaces it); a byte to write that the CPU hasn't supplied is written as 0
 * with lost data; the first byte of Write Sector / Write Track not supplied
 * in time ends the command with lost data. A search fails (record not found /
 * seek error) after 5 index pulses. The head unloads after 15 idle turns.
 *
 * Variants: the 1791/1795/2791/2795 have an inverted data bus (every
 * register reads and writes complemented); 1795/1797/2795/2797 drive the side
 * select output from the U bit (the selected side then comes from it, not
 * setSide); the 1771 is single density only.
 *
 * Drives: four, each double-sided unless set otherwise, with an image
 * (FloppyDisk), write protect and a head position. The machine selects one
 * (selectDrive), and sets the side (setSide), motor (setMotor) and density
 * (setDoubleDensity, the DDEN pin) from its own latch. Ready = the selected
 * drive has a disk and its motor is on (setReadyMode for other wiring).
 * Index pulses and data need a disk and the motor on.
 *
 * Writes mark the image dirty; it is saved back to its file after half a
 * second without writes, on eject, flush() and destruction (a read-only
 * file keeps its changes in memory).
 */
class Wd17xx : public IOHandler {
public:
    enum class Variant { FD1771, FD1791, FD1793, FD1795, FD1797, WD2791, WD2793, WD2795, WD2797, WD1773 };
    /** What the READY input follows. */
    enum class ReadyMode { DiskAndMotor, DiskPresent, Always };

    static constexpr int MAX_DRIVES = 4;

    explicit Wd17xx(Variant v = Variant::WD2793, uint32_t base = 0xFF48);
    ~Wd17xx() override;

    static const char* variantName(Variant v);
    /** A variant by name ("wd1793", "FD1771", "2797"...); false if unknown. */
    static bool parseVariant(const std::string& name, Variant& out);
    Variant variant() const { return m_variant; }
    void setVariant(Variant v);

    // IOHandler -------------------------------------------------------------
    const char* name() const override { return m_name.c_str(); }
    void setName(const std::string& n) override { m_name = n; }
    uint32_t baseAddr() const override { return m_base; }
    uint32_t addrMask() const override { return 0x03; }
    void setBaseAddr(uint32_t b) override { m_base = b; }
    /** The clock tick() counts (the machine's CPU clock). */
    void setClockHz(uint32_t hz) override;
    /** INTRQ (true = asserted). */
    void setIrqLine(ISignalLine* l) override { m_intrqLine = l; }
    /** "intrq" or "drq" lines (true = asserted). */
    void setSignalLine(const char* name, ISignalLine* line) override;
    bool ioRead(IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioPeek(IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override;
    /** Master reset: registers cleared, sector = 1, then a Restore ($03). */
    void reset() override;
    void tick(uint64_t cycles) override;
    void getDeviceInfo(DeviceInfo& out) const override;
    /** Options: "variant", "base", "rpm", "fdcclock" (1000000 / 2000000),
     *  "drive0".."drive3" (an image path), "ready" (motor, disk, always). */
    bool setOption(const std::string& key, const std::string& value, std::string& err) override;
    bool mountDisk(int unit, const std::string& path) override;
    void ejectDisk(int unit) override;
    bool getDiskStatus(int unit, int& track, int& sector, bool& led) override;
    std::string getMountedDiskPath(int unit) override;

    // Registers (as the bus sees them: inverted on the 1791/1795/2791/2795) --
    uint8_t readRegister(int reg);
    uint8_t peekRegister(int reg) const;
    void writeRegister(int reg, uint8_t v);

    // Outputs ---------------------------------------------------------------
    bool intrq() const { return m_intrq; }
    bool drq() const { return m_drq; }
    /** Called when INTRQ / DRQ change. */
    void onIntrq(std::function<void(bool)> fn) { m_intrqHook = std::move(fn); }
    void onDrq(std::function<void(bool)> fn) { m_drqHook = std::move(fn); }
    /** The side select output (1795/1797/2795/2797). */
    int sideSelectOutput() const { return m_sso; }
    bool busy() const { return m_status & ST_BUSY; }
    /** True values (not inverted) for tests and debuggers. */
    uint8_t statusValue() const { return composeStatus(); }
    uint8_t trackRegister() const { return m_track; }
    uint8_t sectorRegister() const { return m_sector; }
    uint8_t dataRegister() const { return m_data; }
    uint8_t commandRegister() const { return m_command; }

    // Drive interface -------------------------------------------------------
    /** The selected drive (0-3), or -1 for none. */
    void selectDrive(int drive);
    int selectedDrive() const { return m_drive; }
    /** The side from the machine's latch (ignored on SSO variants). */
    void setSide(int side) { m_extSide = side ? 1 : 0; }
    void setMotor(bool on);
    bool motor() const { return m_motor; }
    /** DDEN: true = double density (MFM). Ignored on the FD1771. */
    void setDoubleDensity(bool dd) { m_dden = dd; }
    bool doubleDensity() const;
    void setReadyMode(ReadyMode m);
    /** Controller clock: 1 MHz (5.25") or 2 MHz (8"), for step and settle times. */
    void setFdcClockHz(uint32_t hz) { m_fdcClock = hz ? hz : 1000000; }
    void setRpm(int rpm);
    /** Cylinders a drive's head can reach and its sides (default 80, 2). */
    void setDriveGeometry(int drive, int cylinders, int sides);
    /** Put a disk in a drive (replacing and saving any there). */
    void insertDisk(int drive, std::unique_ptr<FloppyDisk> disk);
    /** Take a disk out (saved first if changed); null if none. */
    std::unique_ptr<FloppyDisk> removeDisk(int drive);
    FloppyDisk* disk(int drive) const;
    /** Write protect a drive (on top of the image's own flag). */
    void setWriteProtect(int drive, bool wp);
    /** The drive's head cylinder. */
    int driveCylinder(int drive) const;
    /** The drive's head cylinder, directly (for tests and snapshots). */
    void setDriveCylinder(int drive, int cyl);
    /** Save every changed image now. */
    void flush();

    /** Whether READY is up for the selected drive. */
    bool ready() const;
    /** Whether the index hole is under the sensor. */
    bool indexPulse() const;

    // Status bits
    static constexpr uint8_t ST_BUSY = 0x01, ST_INDEX = 0x02, ST_DRQ = 0x02, ST_TRACK0 = 0x04,
        ST_LOST = 0x04, ST_CRC = 0x08, ST_SEEK = 0x10, ST_RNF = 0x10, ST_HEADLOADED = 0x20,
        ST_RECTYPE = 0x20, ST_WRITEFAULT = 0x20, ST_WRITEPROTECT = 0x40, ST_NOTREADY = 0x80;

private:
    enum class State {
        Idle,
        StepLoop, StepDone, VerifyStart, Search, IdFound,
        T2Start, ReadByte, ReadCrc, WriteDrq, WriteStart, WriteByte, WriteEnd,
        ReadAddrByte, ReadTrackStart, ReadTrackByte,
        WriteTrackCheck, WriteTrackStart, WriteTrackByte,
    };
    enum class Search { Verify, Sector, Address };

    // Variant traits
    bool is1771() const { return m_variant == Variant::FD1771; }
    bool hasSso() const;
    bool hasSideCompare() const;
    bool invertedBus() const;

    // Time
    uint64_t usToCycles(uint64_t us) const;
    /** A delay in ms at a 1 MHz controller clock, scaled to the clock in use. */
    uint64_t fdcMs(int ms) const;
    uint64_t revCycles() const { return m_rev; }
    int trackLen() const;
    uint64_t slotAt(uint64_t t) const;
    uint64_t timeOf(uint64_t slot) const;
    int step() const { return doubleDensity() ? 1 : 2; }

    // The head
    int head() const;
    FloppyTrack* curTrack() const;
    uint8_t readByte(uint64_t slot) const;
    void writeByte(uint64_t slot, uint8_t b);
    bool driveWriteProtected() const;
    bool trackZero() const;

    // Signals
    void setIntrq(bool on);
    void setDrq(bool on);
    uint8_t composeStatus() const;

    // Commands
    void command(uint8_t cmd);
    void forceInterrupt(uint8_t cmd);
    void finish(uint8_t setBits = 0);
    void schedule(State s, uint64_t at);
    void run();
    void stepDrive(int dir);
    void startSearch(Search mode);
    void searchNext();
    void idFound();
    int sectorLength(uint8_t n) const;
    void crcByte(uint8_t b) { m_crc = floppyCrc(m_crc, b); }
    void wrote();
    void checkReadyChange();

    struct Drive {
        std::unique_ptr<FloppyDisk> disk;
        int cyl = 0;
        int cylinders = 80;
        int sides = 2;
        bool writeProtect = false;
    };

    std::string m_name = "WD17xx";
    uint32_t m_base;
    Variant m_variant;
    uint32_t m_clockHz = 1000000;
    uint32_t m_fdcClock = 1000000;
    int m_rpm = 300;
    uint64_t m_rev = 200000;                // cycles a turn

    // Registers
    uint8_t m_status = 0, m_command = 0, m_track = 0, m_sector = 0, m_data = 0;
    bool m_type1Status = true;
    bool m_intrq = false, m_drq = false;
    ISignalLine* m_intrqLine = nullptr;
    ISignalLine* m_drqLine = nullptr;
    std::function<void(bool)> m_intrqHook, m_drqHook;

    // Force interrupt conditions
    bool m_intImmediate = false, m_intIndex = false, m_intNotReadyToReady = false, m_intReadyToNotReady = false;
    bool m_lastReady = false;

    // Drives
    Drive m_drives[MAX_DRIVES];
    int m_drive = 0;
    int m_extSide = 0, m_sso = 0;
    bool m_motor = true, m_dden = true;
    ReadyMode m_readyMode = ReadyMode::DiskAndMotor;
    bool m_headLoaded = false;
    uint64_t m_lastActive = 0;

    // The command in progress
    State m_state = State::Idle;
    uint64_t m_now = 0;                     // cycles since power on
    uint64_t m_eventAt = 0;
    int m_dir = 1;                          // step direction
    bool m_isStep = false;
    Search m_search = Search::Sector;
    uint64_t m_slot = 0;                    // the head's absolute byte slot
    uint64_t m_searchRev = 0;               // revolution the search started in
    uint64_t m_endSlot = 0;                 // Read/Write Track: the next index
    uint64_t m_idSlot = 0;
    uint16_t m_crc = 0;
    int m_left = 0;                         // bytes still to move
    uint8_t m_idTrack = 0;
    bool m_writing = false;                 // a write command changed the disk
    uint64_t m_flushAt = 0;
};
