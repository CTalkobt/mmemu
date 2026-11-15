#pragma once

// Mass storage cartridges for the CoCo (#356): the CoCo SDC and the
// Burke & Burke CoCo XT hard disk interface. The hardware is in
// src/plugins/devices/coco_sdc and src/plugins/devices/xt_hdc; these put it
// on the cartridge port.

#include "coco_cart.h"
#include "plugins/devices/coco_sdc/main/coco_sdc.h"
#include "plugins/devices/xt_hdc/main/xt_hdc.h"

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
 * The Burke & Burke CoCo XT (and XT-RTC) interface, after its User's Manual
 * v2.4 (appendix A.1) and Application Note 5: a PC hard disk controller
 * (WD1002-WX1 or equivalent: XtHdc) selected by /SCS with A4 high, at
 * $FF50 data, $FF51 status (read) / force reset (write), $FF52 read
 * jumpers / start command, $FF53 (write) select mode. "No halt": the
 * controller's interrupt is not used. The controller's BIOS ROM socket is
 * on /CTS ($C000-$FEFF) with address lines A4-A6 inverted (an XT-ROM; the
 * option's ",ROM"). Two drives of raw images. The XT-RTC's clock is not
 * modelled (its registers aren't documented).
 */
class BurkeBurke : public Cart {
public:
    static constexpr uint16_t BASE = 0xFF50;
    explicit BurkeBurke(std::vector<uint8_t> rom = {}) : m_rom(std::move(rom)) {}
    std::string kind() const override { return "burke"; }
    std::string describe() const override;
    void reset() override { m_hdc.reset(); }
    bool read(uint16_t a, bool cts, bool scs, uint8_t& d) override;
    bool peek(uint16_t a, bool cts, bool scs, uint8_t& d) const override;
    void write(uint16_t a, bool cts, bool scs, uint8_t d) override;
    bool mountDisk(int drive, const std::string& path) override;
    bool ejectDisk(int drive) override;
    std::string diskPath(int drive) const override { return m_hdc.imagePath(drive); }
    bool diskStatus(int drive, int& track, bool& motor) const override;

    XtHdc& controller() { return m_hdc; }

private:
    XtHdc m_hdc;
    std::vector<uint8_t> m_rom;
};

}  // namespace coco
