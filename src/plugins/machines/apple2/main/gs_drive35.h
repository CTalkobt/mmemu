#pragma once

#include "libdevices/main/io_handler.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "disk2.h"

namespace apple2 {

/**
 * Apple 3.5" drive (800 KB, double-sided, GCR), as the IIgs's IWM sees it,
 * after KEGS (iwm.c: iwm_read_status35, iwm_do_action35,
 * iwm_nibblize_track_35, iwm_denib_track35).
 *
 * - Images: 819,200-byte ProDOS-order images (.po, .hdv), .2mg (the 2IMG
 *   header's data offset), DiskCopy 4.2 (.dc, .dc42, .image: 84-byte header),
 *   and 3.5" WOZ2 (INFO disk type 2: each track side's bit stream, TMAP
 *   indexed by track * 2 + side).
 * - Tracks: 80 a side, 12/11/10/9/8 sectors by zone of 16 tracks, 2:1
 *   interleave, nibblized as the ROM writes them (address field D5 AA 96,
 *   data field D5 AA AD with 12 tag bytes and 512 data bytes in the 3.5"
 *   GCR code, 10-bit sync bytes). A track is generated when the head
 *   reaches it; a written track is decoded back into the image's sectors.
 * - A track side is a bit stream (generated, or the WOZ's own): the IWM
 *   latches a nibble when its shift register's top bit is set (so 10-bit
 *   syncs self-synchronize); a bit every 2 us; reads see each nibble once.
 *   Writes put 8 bits a byte at the head, keeping the track's length; when
 *   write mode ends the track goes back into the image (decoded into its
 *   sectors, or as bits into the WOZ).
 * - The drive's registers (the phase lines CA0-CA2, SEL from $C031 bit 7):
 *   status reads and LSTRB commands (step direction, step, motor, eject,
 *   disk-switched reset).
 *
 * Time is passed in 14.31818 MHz ticks.
 */
class Gs35Drive {
public:
    static constexpr int TRACKS = 80;
    static constexpr uint32_t IMAGE_BYTES = 819200;
    static constexpr double TICKS_PER_BIT = 14.31818 * 2.0;

    /** Load an image; false (with `err`) if it is not an 800 KB disk. */
    bool insert(const std::string& path, std::string* err = nullptr);
    /** A disk from bytes (tests): ProDOS order, 819,200 bytes. */
    bool insertBytes(std::vector<uint8_t> blocks, bool writeProtected = false);
    void eject();
    bool hasDisk() const { return !m_blocks.empty() || m_woz; }
    bool isWoz() const { return (bool)m_woz; }
    const std::string& path() const { return m_path; }
    /** The disk's blocks (ProDOS order; empty for a WOZ). */
    const std::vector<uint8_t>& blocks() const { return m_blocks; }
    bool writeProtected() const { return m_protect; }

    /** The status register `reg` (CA1 CA0 SEL CA2) in bit 0. */
    int status(int reg, uint64_t now);
    /** The LSTRB command `reg`. */
    void command(int reg, uint64_t now);
    /** The next nibble the head has passed since the last one read (bit 7
     *  set), or 0. */
    uint8_t readNibble(uint64_t now);
    /** A nibble written (write mode): the first at the head, then in turn. */
    void writeNibble(uint8_t v, uint64_t now);
    /** The IWM's handshake register while writing (bit 7 ready, bit 6 no underrun). */
    uint8_t handshake(uint64_t now) const;
    /** Write mode ended: the track goes back into the image. */
    void endWrite();

    /** Whether the drive is connected (status register $F). */
    void setInstalled(bool on) { m_installed = on; }
    int track() const { return m_track; }
    int side() const { return m_side; }
    bool motorOn() const { return m_motor; }

    /** Sectors on a track of zone `track / 16`. */
    static int sectorsOn(int track) { return 12 - track / 16; }
    /** The first block of a track's side (ProDOS order). */
    static int firstBlock(int track, int side);
    /** A track side's nibbles (value and bit length), as the ROM writes them. */
    static void nibblize(const uint8_t* sectors, int track, int side, std::vector<uint8_t>& nib,
                         std::vector<uint8_t>& bits);
    /** Find and decode the track's sectors into `sectors`; the count decoded. */
    static int denibblize(const std::vector<uint8_t>& nib, int track, int side, uint8_t* sectors);

private:
    void loadTrack();
    void deriveNibbles();
    uint32_t bitAt(uint64_t now) const { return (uint32_t)((uint64_t)((double)now / TICKS_PER_BIT) % m_trackBits); }
    bool getBit(uint32_t i) const { return (m_stream[i >> 3] >> (7 - (i & 7))) & 1; }
    void setBit(uint32_t i, bool v) {
        if (v) m_stream[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
        else m_stream[i >> 3] &= (uint8_t)~(0x80 >> (i & 7));
    }

    std::vector<uint8_t> m_blocks;          // the 800 KB (not for a WOZ)
    std::vector<uint8_t> m_file;            // the image file as read (written back by patching)
    std::unique_ptr<Disk2Image> m_woz;      // a 3.5" WOZ
    std::string m_path;
    size_t m_fileOffset = 0;
    bool m_protect = false, m_switched = false;
    int m_track = 0, m_side = 0;
    bool m_stepOut = false, m_motor = false, m_installed = true;
    // The current track side: its bits, and the nibbles the IWM latches
    int m_loadedTrack = -1, m_loadedSide = -1;
    std::vector<uint8_t> m_stream;
    uint32_t m_trackBits = 1;
    std::vector<uint8_t> m_nib;
    std::vector<uint32_t> m_end;            // the bit after each nibble's last
    size_t m_lastRead = SIZE_MAX;
    bool m_hasWeakBits = false;
    uint64_t m_revolution = 0;
    uint32_t m_rand = 0x1234567;
    bool m_trackDirty = false, m_writing = false;
    uint32_t m_writeBit = UINT32_MAX;
    uint64_t m_lastWrite = 0;
};

/** The 3.5" drives as a device (units 1 and 2) for the frontends. */
class Gs35Drives : public IOHandler {
public:
    Gs35Drive drive[2];
    const char* name() const override { return "3.5\" Drive"; }
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
};

}  // namespace apple2
