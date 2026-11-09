#pragma once

#include "libcore/main/intercept.h"
#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <string>
#include <vector>

namespace apple2 {

/**
 * A SmartPort block device card in slot 7 for disk images of any size: hard
 * disks (ProDOS volumes up to 32 MB, GS/OS volumes larger) and 1.44 MB
 * disks, after KEGS (GPL-3.0; config.c g_rom_c700, smartport.c). The IIgs
 * would need a SCSI card or the SuperDrive card for these; KEGS serves them
 * from an emulator card instead, and so does this.
 *
 * The card's ROM ($C700) carries the SmartPort signature (Cn01 $20, Cn03
 * $00, Cn05 $03, Cn07 $00), the ProDOS entry at $C70A ($C7FF) and the
 * SmartPort entry three bytes on; each ends at $C70F, where a WDM traps to
 * the emulator (V set: boot; C clear: ProDOS; C set: SmartPort) and the
 * intercept serves the call:
 *   - boot: block 0 of unit 1 to $0800, then $0801 with X = $70 (else slot 5);
 *   - ProDOS ($42-$47): status (the block count in X/Y), read, write, format;
 *   - SmartPort (inline command and parameter list, standard and extended):
 *     status (driver, unit, DIB), read block, write block, format, control.
 *
 * Images: raw ProDOS-order files (.po, .hdv, .img, .iso), .2mg (its data
 * offset and length); writes go straight back to the file.
 */
class GsSmartPort : public IOHandler, public IPcIntercept {
public:
    static constexpr int UNITS = 4;
    static constexpr uint32_t TRAP_PC = 0xC70F;

    GsSmartPort();
    /** The card's 256-byte ROM ($C700-$C7FF). */
    const uint8_t* rom() const { return m_rom; }
    /** Whether a unit holds a disk (the card is fitted only then). */
    bool anyMounted() const;
    /** Called by the bus: whether the card's ROM answers at $C700 now. */
    std::function<bool()> romVisibleNow;

    // IOHandler
    const char* name() const override { return "SmartPort"; }
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

    /** A disk from bytes (tests): `blocks` x 512 bytes. */
    bool insertBytes(int unit, std::vector<uint8_t> data);
    const std::vector<uint8_t>& image(int unit) const { return m_unit[(unit - 1) & 3].file; }

    // IPcIntercept
    std::vector<uint32_t> addresses() const override { return {TRAP_PC}; }
    bool onExecute(MachineDescriptor& m, ICore& cpu, IBus& bus, uint32_t pc) override;

private:
    struct Unit {
        std::vector<uint8_t> file;      // the whole file
        size_t offset = 0, size = 0;    // the blocks within it
        std::string path;
        bool protect = false, switched = false;
        int lastBlock = -1;
        bool busy = false;
    };
    int readBlock(int unit, IBus& bus, uint32_t buf, uint32_t block);
    int writeBlock(int unit, IBus& bus, uint32_t buf, uint32_t block);
    int format(int unit);
    void boot(ICore& cpu, IBus& bus);
    void prodos(ICore& cpu, IBus& bus);
    void smartport(ICore& cpu, IBus& bus);

    uint8_t m_rom[256] = {};
    Unit m_unit[UNITS];
};

}  // namespace apple2
