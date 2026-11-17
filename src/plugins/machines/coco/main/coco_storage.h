#pragma once

// Mass storage cartridges for the CoCo (#356): the CoCo SDC and the
// Burke & Burke CoCo XT hard disk interface. The hardware is in
// src/plugins/devices/coco_sdc and src/plugins/devices/xt_hdc; these put it
// on the cartridge port.

#include "coco_cart.h"
#include "plugins/devices/coco_sdc/main/coco_sdc.h"
#include "plugins/devices/xt_hdc/main/xt_hdc.h"
#include "plugins/machines/apple2/main/no_slot_clock.h"   // a DS1216/DS1215 (header only)

namespace coco {

/**
 * The CoCo SDC cartridge (CoCo address scheme, after the CoCo SDC User
 * Guide): its flash ($C000, 8 banks of 16 KB, the DIP switches' bank at
 * reset), FDC emulation (an FD-502's latch at $FF40 and a WD1793 at
 * $FF48-$FF4B, drives 0 and 1 the mounted images) and command mode ($43 to
 * $FF40; see CocoSdc). The SD card is a host folder. Its I/O is
 * $FF40-$FF4F (not mirrored at $FF50).
 */
class CocoSdcCart : public Cart {
public:
    CocoSdcCart(std::vector<uint8_t> flash, std::string cardDir, int dipBank = 0);
    std::string kind() const override { return "sdc"; }
    std::string describe() const override;
    void reset() override;
    bool read(uint16_t a, bool cts, bool scs, uint8_t& d) override;
    bool peek(uint16_t a, bool cts, bool scs, uint8_t& d) const override;
    void write(uint16_t a, bool cts, bool scs, uint8_t d) override;
    void clock(uint64_t ticks) override { m_fd.clock(ticks); }
    void setClockHz(double hz) override { m_fd.setClockHz(hz); }
    /** Drives 0 and 1: a host image (DSK, JVC, VDK, SDF; a DSK over 2880 sectors is a hard disk). */
    bool mountDisk(int drive, const std::string& path) override;
    bool ejectDisk(int drive) override;
    std::string diskPath(int drive) const override;
    bool diskStatus(int drive, int& track, bool& motor) const override;

    CocoSdc& sdc() { return m_sdc; }
    Fd500& floppyHalf() { return m_fd; }

private:
    Fd500 m_fd;
    CocoSdc m_sdc;
};

/**
 * The Burke & Burke CoCo XT-RTC interface (Rev D): a PC hard disk controller
 * (WD1002-WX1 or equivalent: XtHdc) on a small ISA slot, plus a DS1215
 * Phantom Time Chip. From the board's schematic (WormFood's KiCad
 * transcription, roms/coco/xt-rtc/) and B&B's OS-9 boot ROM:
 *  - The ISA address is the CoCo's with A4, A5, A6 and A14 inverted (U3),
 *    BA16/BA17/AEN tied low, BA18/BA19 high. /IOR and /IOW come from R/W
 *    alone (U2: the gate's other input is a 7400 output tied high), so the
 *    controller's own I/O decode of $320-$323 picks CoCo $FF50-$FF53: $FF50
 *    data, $FF51 status (read) / reset (write), $FF52 jumpers (read) /
 *    select (write), $FF53 DMA/interrupt mask (write).
 *  - /SMEMR is /CTS: the controller's BIOS ROM socket (ISA $C8000) answers
 *    at $C000 with A4-A6 inverted (an XT-ROM: the dump as burned).
 *  - U4, a DS1215 in ROM mode (ROM/RAM low), /CEI = /(SCS . E), /OE =
 *    /(R/W . E), /WE = A2, D = A0, Q = D0, RST = /RESET: any $FF40-$FF5F
 *    access with A2 low writes bit A0 to it (a read or a write cycle), a
 *    read with A2 high reads a bit on D0. 64 bits of $C5 $3A $A3 $5C $C5
 *    $3A $A3 $5C (low bit first) open its eight BCD registers for 64 bits.
 *    The clock is the host's (setting it is not kept).
 * "No halt": the controller's interrupt and DMA lines aren't connected. Two
 * drives of raw images.
 */
class BurkeBurke : public Cart {
public:
    static constexpr uint16_t BASE = 0xFF50;
    explicit BurkeBurke(std::vector<uint8_t> rom = {}) : m_rom(std::move(rom)) {}
    std::string kind() const override { return "burke"; }
    std::string describe() const override;
    void reset() override {
        m_hdc.reset();
        m_clock.reset();     // its RST pin is the CoCo's /RESET
    }
    bool read(uint16_t a, bool cts, bool scs, uint8_t& d) override;
    bool peek(uint16_t a, bool cts, bool scs, uint8_t& d) const override;
    void write(uint16_t a, bool cts, bool scs, uint8_t d) override;
    bool mountDisk(int drive, const std::string& path) override;
    bool ejectDisk(int drive) override;
    std::string diskPath(int drive) const override { return m_hdc.imagePath(drive); }
    bool diskStatus(int drive, int& track, bool& motor) const override;

    XtHdc& controller() { return m_hdc; }
    /** The XT-RTC's DS1215 (the same protocol as the DS1216 SmartWatch/ROM). */
    apple2::NoSlotClock& clock() { return m_clock; }

private:
    XtHdc m_hdc;
    apple2::NoSlotClock m_clock;
    std::vector<uint8_t> m_rom;
};

}  // namespace coco
