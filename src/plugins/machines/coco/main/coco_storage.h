#pragma once

// Mass storage cartridges for the CoCo (#356): the CoCo SDC, the
// Burke & Burke CoCo XT hard disk interface and the Disto Super Controller
// II, and the Becker port to a built-in DriveWire server. The hardware is in
// src/plugins/devices/coco_sdc, xt_hdc, wd17xx and drivewire;
// these put it on the cartridge port.

#include "coco_cart.h"
#include "plugins/devices/coco_sdc/main/coco_sdc.h"
#include "plugins/devices/xt_hdc/main/xt_hdc.h"
#include "plugins/devices/drivewire/main/drivewire_server.h"
#include "plugins/machines/apple2/main/no_slot_clock.h"   // a DS1216/DS1215 (header only)
#include <array>
#include <functional>
#include <memory>

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

/**
 * A board on the Disto Super Controller II's Mini-Expansion Bus (J1, a
 * 17-pin header): RESET, E, A0-A2, D0-D7, /CE, R/W, +5V and GND. It sees
 * eight locations, $FF50-$FF57 (reg 0-7). A board that interrupts drives
 * the CART line (the CoCo's FIRQ through PIA1) through the SC-II's J2.
 */
class MebDevice {
public:
    virtual ~MebDevice() = default;
    virtual std::string describe() const = 0;
    virtual void reset() {}
    virtual uint8_t read(int reg) = 0;
    virtual uint8_t peek(int reg) const = 0;
    virtual void write(int reg, uint8_t v) = 0;
    /** Time, in the machine's crystal ticks as Cart::clock. */
    virtual void clock(uint64_t ticks) { (void)ticks; }
    /** Its interrupt to J2 (true = pulling CART low). */
    std::function<void(bool)> setFirq;
};

/**
 * The Disto Super Controller II (CRC Computers; Tony DiStefano's design,
 * after its Instruction Manual and the Disto schematic SCII13.SCH rev 1.3):
 * an FD-502-compatible controller (a WD1773, the latch at $FF40-$FF47, the
 * WD1773 at $FF48-$FF4B mirrored to $FF4F) with
 *  - the DOS socket on /CTS (an 8 KB ROM or a 2764 / 27128 / 27256 EPROM;
 *    16 KB at $C000);
 *  - a 256-byte "no halt" sector buffer (a 6116 addressed by an 8-bit
 *    counter, 74HC393) and its registers at $FF74-$FF77, A1 picking one:
 *      $FF74/$FF75 buffer data: read or write the byte under the counter,
 *                  which then counts up;
 *      $FF76/$FF77 control (write; any write clears the counter): bit 0
 *                  1 = the FDC reads into the buffer, 0 = the buffer is
 *                  written to the FDC (buffered mode only); bit 1 buffered
 *                  mode (the WD1773's DRQs are served from / into the
 *                  buffer, /HALT is never asserted); bit 2 masks the
 *                  WD1773's INTRQ from /NMI; bit 3 puts INTRQ on CART
 *                  (FIRQ). Read: bit 7 = INTRQ, inverted (low: interrupt).
 *    Jumper J3 (setAltRegisters, the option's ",alt") moves the four to the
 *    SCS area, $FF58-$FF5F (data at $FF58, control at $FF5A as NitrOS-9's
 *    rb1773 SCIIALT);
 *  - the Mini-Expansion Bus at $FF50-$FF57 (MebDevice).
 * At power-on the control register is clear (normal mode): the board is
 * then an FD-502, so Disk BASIC runs on it unchanged.
 */
class DistoSc2 : public Fd500 {
public:
    static constexpr uint8_t CTRL_READ = 0x01, CTRL_BUFFERED = 0x02, CTRL_MASK_NMI = 0x04, CTRL_FIRQ = 0x08;

    explicit DistoSc2(std::vector<uint8_t> dos, bool altRegisters = false);
    std::string kind() const override { return "sc2"; }
    std::string describe() const override;
    void reset() override;
    bool read(uint16_t a, bool cts, bool scs, uint8_t& d) override;
    bool peek(uint16_t a, bool cts, bool scs, uint8_t& d) const override;
    void write(uint16_t a, bool cts, bool scs, uint8_t d) override;
    void clock(uint64_t ticks) override;
    void setDrq(bool on) override;
    void setIntrq(bool on) override;

    /** Jumper J3: the registers at $FF58-$FF5F instead of $FF74-$FF77. */
    void setAltRegisters(bool alt) { m_alt = alt; }
    bool altRegisters() const { return m_alt; }
    /** A board on the Mini-Expansion Bus (null: none, $FF50-$FF57 not driven). */
    void plugMeb(std::unique_ptr<MebDevice> dev);
    MebDevice* meb() const { return m_meb.get(); }
    /** The control register as last written; the buffer counter and RAM. */
    uint8_t control() const { return m_ctrl; }
    uint8_t bufferCounter() const { return m_count; }
    const std::array<uint8_t, 256>& buffer() const { return m_buf; }

protected:
    bool nmiAllowed() const override { return !(m_ctrl & CTRL_MASK_NMI); }
    bool haltAllowed() const override { return !(m_ctrl & CTRL_BUFFERED); }

private:
    /** 0 = buffer data, 1 = control, -1 = neither, for a cycle at a. */
    int bufferRegister(uint16_t a, bool cts, bool scs) const;
    void controlWrite(uint8_t v);
    uint8_t controlRead() const { return (uint8_t)((m_ctrl & 0x7F) | (m_intrq ? 0x00 : 0x80)); }
    void updateCart();
    std::array<uint8_t, 256> m_buf{};
    std::unique_ptr<MebDevice> m_meb;
    uint8_t m_count = 0, m_ctrl = 0;
    bool m_alt = false, m_mebFirq = false, m_cartLow = false;
};

/**
 * The Becker port (Gary Becker's, from the CoCo3FPGA; XRoar's becker.c): a
 * DriveWire byte stream through two registers, here to the built-in
 * DriveWire 4 server (DriveWireServer) instead of a TCP link:
 *  - $FF41 status (read): bit 1 set while a byte from the server waits;
 *  - $FF42 data: read takes the server's next byte (0 if none), write
 *    sends one to it.
 * Decoded on /SCS (in a Multi-Pak, the slot $FF7F routes $FF40-$FF5F to).
 * Answers are ready at once (the server is in-process): NitrOS-9's
 * Becker dwread polls $FF41 with a timeout, HDB-DOS's without one.
 *
 * Its option ROM (for HDB-DOS built for the Becker port, hdbdw3bck.rom /
 * hdbdw3bc3.rom) answers /CTS at $C000 as a disk controller's DOS does (8 KB
 * repeated over the 16 KB). There is no floppy controller: $FF40 and
 * $FF48-$FF4B are open. mountDisk(n) puts an image in DriveWire drive n
 * (CLI `disk mount 1 FILE`: drive 0), written through to the file, or
 * kept in memory with copy-on-write (option ",cow").
 */
class BeckerCart : public Cart {
public:
    static constexpr uint16_t STATUS = 0xFF41, DATA = 0xFF42;
    explicit BeckerCart(std::vector<uint8_t> rom = {}, std::string romLabel = "", bool copyOnWrite = false)
        : m_rom(std::move(rom)), m_label(std::move(romLabel)), m_cow(copyOnWrite) {}
    std::string kind() const override { return "becker"; }
    std::string describe() const override;
    void reset() override { m_dw.reset(); }
    bool read(uint16_t a, bool cts, bool scs, uint8_t& d) override;
    bool peek(uint16_t a, bool cts, bool scs, uint8_t& d) const override;
    void write(uint16_t a, bool cts, bool scs, uint8_t d) override;
    void clock(uint64_t ticks) override;
    void setClockHz(double hz) override { m_hz = hz; }
    /** DriveWire drive 0-255 (the CLI's unit 1 is drive 0). */
    bool mountDisk(int drive, const std::string& path) override;
    bool ejectDisk(int drive) override { return m_dw.eject(drive); }
    std::string diskPath(int drive) const override { return m_dw.imagePath(drive); }
    /** "track": the last LSN's / 18; "motor": a transfer within the last half second. */
    bool diskStatus(int drive, int& track, bool& motor) const override;

    DriveWireServer& server() { return m_dw; }
    bool copyOnWrite() const { return m_cow; }

private:
    DriveWireServer m_dw;
    std::vector<uint8_t> m_rom;
    std::string m_label;
    bool m_cow;
    double m_hz = 14318180.0;
    uint64_t m_acc = 0;
    double m_sinceAccess = 1e9;     // seconds since the last disk transfer
    uint64_t m_lastCount = 0;
};

}  // namespace coco
