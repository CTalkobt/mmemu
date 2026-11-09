#pragma once

#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <string>
#include <vector>

namespace apple2 {

/**
 * SmartPort devices on the disk port (the IIc's 32 KB ROMs, the IIgs): a
 * chain of block devices that talk packets over the IWM, as a UniDisk 3.5
 * does. High-level: the bytes the IWM writes and reads are packets, served
 * from disk images; the drive's own 65C02 is not emulated. After Clemens
 * (clem_smartport.c, S. Sinha, MIT) and the IIgs Firmware Reference ch. 7;
 * the end-of-chain INIT reply ($FF) as FujiNet's iwm.cpp.
 *
 * - The bus: phases 0 and 2 alone reset it (the units forget their IDs and
 *   each holds phase 3 low for those after it until it has one); phases 1
 *   and 3 enable it; phase 0 is REQ; the sense line (status bit 7) is ACK.
 * - A transaction: ACK high (ready); the host raises REQ and writes a
 *   packet (sync, $C3, the header, the 7-bit groups, the checksum, $C8);
 *   the unit drops ACK; REQ falls and the unit runs the command; ACK high,
 *   REQ up, the host reads the reply packet; ACK low, REQ down. WRITEBLOCK
 *   takes a second packet (the data) before its reply.
 * - Commands: INIT (assigns the next unit its ID; the last replies $FF),
 *   STATUS (codes 0 and 3, the DIB), READBLOCK, WRITEBLOCK, FORMAT, CONTROL;
 *   standard and extended forms.
 * - Units: block devices (DIB type $02: GS/OS would drive a UniDisk's own
 *   type, $01, through the drive's 65C02 firmware), 800 KB images named
 *   "UNIDISK 3.5" (removable), others "MMEMU HARD DISK" (fixed). Images as
 *   the slot 7 card's: .po, .hdv, .2mg; writes go back to the file.
 * - Bytes take 8 bit cells (4 us, 2 in the IWM's fast mode); a reply's next
 *   byte is ready a byte time after the last was read.
 *
 * Time is in the caller's units, `unitsPerUs` to the microsecond.
 */
class SmartPortBus : public IOHandler {
public:
    static constexpr int UNITS = 4;

    explicit SmartPortBus(double unitsPerUs);

    // IOHandler
    const char* name() const override { return "UniDisk"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override;
    void tick(uint64_t) override {}
    bool mountDisk(int unit, const std::string& path) override;
    void ejectDisk(int unit) override;
    bool getDiskStatus(int unit, int& track, int& sector, bool& led) override;
    std::string getMountedDiskPath(int unit) override;

    /** A disk from bytes (tests): `blocks` x 512 bytes. */
    bool insertBytes(int unit, std::vector<uint8_t> data);
    const std::vector<uint8_t>& image(int unit) const { return m_unit[(unit - 1) & 3].file; }
    /** Units on the chain: the highest holding a disk (or as set). */
    int units() const;
    void setUnits(int n) { m_fixedUnits = n; }
    /** The ID the host gave a unit (0: none yet). */
    int unitId(int unit) const { return m_unit[(unit - 1) & 3].id; }

    // The IWM's side
    /** The phase lines (bits 0-3) changed. */
    void phases(uint32_t ph, uint64_t now);
    /** Whether the bus is enabled with units on it (the IWM's data and
     *  sense go to it). */
    bool active() const { return m_enabled && units() > 0; }
    /** ACK, on the sense line. */
    bool ack(uint64_t now);
    /** The data register (read mode): a reply byte (bit 7 set) once, or 0. */
    uint8_t read(uint64_t now);
    /** A byte written (write mode). */
    void write(uint8_t v, uint64_t now);
    /** The handshake register while writing (bit 7 ready, bit 6 no underrun). */
    uint8_t handshake(uint64_t now) const;
    /** The IWM's mode register (bit 3: fast, 2 us cells). */
    void setMode(uint8_t mode) { m_byteTime = 8.0 * ((mode & 0x08) ? 2.0 : 4.0) * m_unitsPerUs; }

    /** Packets handled (tests). */
    uint64_t commands() const { return m_commands; }

private:
    enum class St { Ready, Receive, Executing, PendingData, Response, Complete };
    struct Unit {
        std::vector<uint8_t> file;
        size_t offset = 0, size = 0;
        std::string path;
        bool protect = false;
        int id = 0;
        int lastBlock = -1;
        bool busy = false;
    };
    struct Packet {
        uint8_t dest = 0, src = 0, type = 0, status = 0;
        bool ext = false;
        std::vector<uint8_t> data;
    };

    void reqFell();
    void reqRose(uint64_t now);
    bool decode(Packet& p) const;
    void reply(int unit, uint8_t type, uint8_t status, const uint8_t* data, size_t len);
    void command(const Packet& p);
    int target(const Packet& p) const;

    const double m_unitsPerUs;
    double m_byteTime;
    Unit m_unit[UNITS];
    int m_fixedUnits = 0;
    uint32_t m_ph = 0;
    bool m_enabled = false, m_req = false;
    St m_st = St::Ready;
    std::vector<uint8_t> m_in;              // the packet being received (from $C3)
    bool m_sawBegin = false;
    uint64_t m_lastWrite = 0;
    bool m_writing = false;
    double m_ackLowAt = 0;
    int m_active = -1;                      // the unit in the transaction
    uint8_t m_cmd = 0xFF;
    uint32_t m_writeBlock = 0;
    std::vector<uint8_t> m_out;             // the reply's bytes
    size_t m_outPos = 0;
    double m_nextAt = 0;
    uint64_t m_commands = 0;
};

/** A block image's blocks within its file: raw (.po, .hdv) or .2mg
 *  (ProDOS order); false if it is not one. */
bool blockImage(const std::vector<uint8_t>& file, size_t& offset, size_t& size);

}  // namespace apple2
