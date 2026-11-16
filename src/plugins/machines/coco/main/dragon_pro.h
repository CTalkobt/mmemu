#pragma once

// The Dragon Professional (Alpha)'s own hardware (#355), after XRoar
// (dragon/dragonpro.c; GPL-3.0-or-later) and MAME (trs/dgnalpha.cpp;
// BSD-3-Clause); mmsim's own code.

#include "libdevices/main/io_handler.h"
#include "plugins/devices/mc6821/main/mc6821.h"
#include "plugins/machines/spectrum/main/spectrum_ay.h"
#include <cstdint>
#include <memory>
#include <string>

class Wd17xx;

namespace coco {

class DragonProDisk;

/**
 * What the Dragon Professional adds to a Dragon 64's board:
 *
 *  - PIA2 at $FF24-$FF27 (where A2 is set in $FF20-$FF3F; its /IRQs on
 *    FIRQ): PA0 the PSG's BDIR, PA1 its BC1, PA2 the ROM select (1, the
 *    pull-ups' level at reset: the 8 KB boot ROM at $8000 and $A000; 0:
 *    BASIC); PB0-PB7 the PSG's data bus; CA2 the disk's NMI enable; CB1
 *    the controller's DRQ.
 *  - An AY-3-8912 (spectrum::SpectrumAy) at 1 MHz (XRoar: from the FDC's
 *    clock; MAME 4 MHz / 4), on the sound mux's fourth input (PIA0 CA2 and
 *    CB2 both high). Its I/O port A drives the disk: bits 0-3 select drive
 *    0-3 (the lowest set bit; none: drive 0), 4 the motor, 5 single
 *    density, 6 write precompensation, 7 8" drives (2 MHz controller clock).
 *  - A WD2797 at $FF2C-$FF2F, its registers in reverse order ($FF2C data,
 *    $FF2F command / status); INTRQ to /NMI while CA2 is high, READY high.
 *  - The modem: not emulated. XRoar maps nothing for it; MAME stubs
 *    $FF28-$FF2B (reads $FF), which here are PIA1's mirror as on the 64.
 *
 * The 6551 at $FF04 is the Dragon 64's (System::acia()).
 */
struct DragonProHw {
    DragonProHw();
    ~DragonProHw();
    Mc6821 pia2;
    spectrum::SpectrumAy ay;
    std::unique_ptr<Wd17xx> fdc;
    std::unique_ptr<DragonProDisk> disk;    ///< the drives as an I/O handler
    std::string paths[4];
    uint8_t ioa = 0xFF;          ///< the PSG's port A as the disk sees it
    uint64_t ayAcc = 0;
    float ayLevel = 0.0f;

    /** The PSG's port A changed: the disk's selects. */
    void diskControl();
    /** PIA2 port A written: the PSG's bus cycle (BDIR, BC1) with port B. */
    void psgCycle();
    bool bootSelected() const { return pia2.portA() & 0x04; }
};

/** The Professional's built-in floppy drives for the CLI and GUI (`disk mount 1-4`). */
class DragonProDisk : public IOHandler {
public:
    explicit DragonProDisk(DragonProHw* hw) : m_hw(hw) {}
    const char* name() const override { return "Floppy"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    bool mountDisk(int unit, const std::string& path) override;
    void ejectDisk(int unit) override;
    bool getDiskStatus(int unit, int& track, int& sector, bool& led) override;
    std::string getMountedDiskPath(int unit) override;
    void getDeviceInfo(DeviceInfo& out) const override;

private:
    DragonProHw* m_hw;
};

}  // namespace coco
