#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/iport_device.h"
#include "libdevices/main/isignal_line.h"
#include "libmem/main/ibus.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class MOS6502;
class VIA6522;
class SharedIrqManager;

/**
 * A true-drive Commodore 1541 (#341): its own 6502 at 1 MHz running the DOS
 * ROM, 2K of RAM, VIA1 at $1800 (the serial bus) and VIA2 at $1C00 (the disk
 * head), and a disk as GCR tracks under a rotating head.
 *
 *  - The serial bus: VIA1 PB1/PB3 pull DATA/CLK low through 7406s, PB0/PB2/
 *    PB7 read DATA, CLK and ATN (1 = low), PB5/PB6 the device number
 *    jumpers; ATN also goes to CA1. DATA is pulled low too while ATN and
 *    the ATN acknowledge (PB4) differ, the XOR gate that answers an ATN
 *    before the DOS does. The computer's side is an IPortDevice for a CIA
 *    port, as IecDrive's: bits 3-5 out (ATN, CLK, DATA), 6-7 in (CLK, DATA).
 *  - The disk: VIA2 PB0/PB1 the stepper's phase (half-tracks 0-83), PB2 the
 *    motor, PB3 the LED, PB4 write protect (0 = protected), PB5/PB6 the
 *    speed zone, PB7 SYNC (0 = found); PA the byte under the head, CA2 the
 *    byte-ready enable to the CPU's SO pin, CB2 read (1) or write (0). The
 *    head moves on a byte every 26-32 cycles by zone; ten 1 bits are a SYNC,
 *    where no byte is ready. A byte is ready a byte after a SYNC's end, and
 *    sets V when SO is enabled.
 *
 * Images: D64 (35-42 tracks, with or without error bytes) encoded as the
 * DOS formats it; G64 as stored. Writes go to the tracks in memory, and
 * from them back to a D64 file on eject.
 *
 * Time: tick() is in the host's cycles (setClockHz); the drive runs its
 * instructions up to the same time, its VIAs and head a cycle at a time.
 */
class Drive1541 : public IOHandler, public IPortDevice {
public:
    explicit Drive1541(int unit = 8);
    ~Drive1541() override;

    const char* name() const override { return m_name.c_str(); }
    void        setName(const std::string& n) override { m_name = n; }
    uint32_t    baseAddr() const override { return 0; }
    uint32_t    addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override;
    void tick(uint64_t cycles) override;
    void setClockHz(uint32_t hz) override { m_hostHz = hz ? hz : 985248; }
    void getDeviceInfo(DeviceInfo& out) const override;

    bool mountDisk(int unit, const std::string& path) override;
    void ejectDisk(int unit) override;
    bool getDiskStatus(int unit, int& track, int& sector, bool& led) override;
    std::string getMountedDiskPath(int unit) override { return unit == m_unit ? m_path : ""; }

    /** The DOS ROM (16K, $C000-$FFFF); false if it can't be read. */
    bool loadRom(const std::string& path);
    bool romLoaded() const { return m_romLoaded; }

    // IPortDevice: the computer's CIA port
    uint8_t readPort() override;
    void writePort(uint8_t v) override { m_hostPra = v; hostLines(); }
    void setDdr(uint8_t d) override { m_hostDdr = d; hostLines(); }

    /** The drive's CPU, bus and VIAs (inspection, tests). */
    MOS6502* cpu() const { return m_cpu.get(); }
    IBus* bus() const;
    int halfTrack() const { return m_halfTrack; }
    uint64_t driveCycles() const { return m_clock; }

private:
    class Bus;
    class Via1Port;
    class Via2PortA;
    class Via2PortB;
    struct Line : public ISignalLine {
        bool level = true;
        bool get() const override { return level; }
        void set(bool l) override { level = l; }
        void pulse() override {}
    };

    void hostLines();
    void busChanged();
    void runDevicesTo(uint64_t t);
    void diskCycle();
    void stepper(uint8_t phase);
    void loadTrack();
    void flushTrack();
    uint8_t via1Pins() const;

    std::string m_name = "Drive8";
    int m_unit = 8;
    uint32_t m_hostHz = 985248;
    uint64_t m_frac = 0;           // host cycles * 1e6, mod m_hostHz
    uint64_t m_target = 0;         // drive cycles the host has reached
    uint64_t m_clock = 0;          // drive cycles run (instructions)
    uint64_t m_devClock = 0;       // drive cycles the VIAs and head have run
    uint64_t m_instrStart = 0;

    std::unique_ptr<MOS6502> m_cpu;
    std::unique_ptr<Bus> m_bus;
    std::unique_ptr<VIA6522> m_via1, m_via2;
    std::unique_ptr<SharedIrqManager> m_irq;
    std::unique_ptr<Via1Port> m_via1Port;
    std::unique_ptr<Via2PortA> m_via2A;
    std::unique_ptr<Via2PortB> m_via2B;
    Line m_soe, m_mode;            // VIA2 CA2 (SO enable), CB2 (1 = read)
    uint8_t m_ram[0x800] = {};
    uint8_t m_rom[0x4000] = {};
    bool m_romLoaded = false;

    // The serial bus (true = pulled low)
    uint8_t m_hostPra = 0, m_hostDdr = 0;
    bool m_hostAtn = false, m_hostClk = false, m_hostData = false;
    bool m_driveClk = false, m_driveData = false, m_atna = false;
    bool m_atn = false, m_clk = false, m_data = false;

    // The disk
    std::string m_path;
    bool m_isD64 = false, m_d64Errors = false;
    int m_d64Tracks = 0;
    std::vector<uint8_t> m_image;              // the D64's bytes
    std::vector<std::vector<uint8_t>> m_tracks; // GCR by half-track (empty: none)
    std::vector<bool> m_dirty;
    bool m_writeProtect = false;
    int m_halfTrack = 34;                      // track 18
    uint8_t m_phase = 0;
    size_t m_pos = 0;                          // byte under the head
    int m_byteCycles = 0;                      // to the next byte
    uint8_t m_readByte = 0;                    // VIA2 PA's input
    bool m_sync = false;
    uint8_t m_lastByte = 0;
};
