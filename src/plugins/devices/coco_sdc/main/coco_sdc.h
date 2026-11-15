#pragma once

#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class FloppyDisk;

/**
 * The CoCo SDC (Darren Atkinson's SD-card floppy emulator cartridge, #356):
 * its command interface, its SD card (a host folder) and its flash, after
 * the "CoCo SDC User Guide" (v4, 2020): chapter 1's register table,
 * chapter 6's image formats and chapter 7's command reference.
 *
 * Registers (CoCo scheme; the Dragon scheme is not modelled):
 *
 * | Address | Use |
 * |---------|-----|
 * | $FF40 | drive control latch ($43: command mode; anything else: FDC emulation) |
 * | $FF42 | flash data register |
 * | $FF43 | flash control register |
 * | $FF48 | command (write) / status (read) in command mode; else the FDC's |
 * | $FF49-$FF4B | parameters / response bytes; $FF4A/$FF4B the data port |
 *
 * In FDC emulation mode the latch and the FDC at $FF48-$FF4B are an FD-502's
 * (the machine's FD-500 code and Wd17xx: drives 0 and 1 are the mounted
 * images, given to it through the floppy hooks).
 *
 * Status: $01 BUSY, $02 READY (a 256-byte block moves through the data
 * port), $80 FAILED, and per command $04 path name invalid, $08 miscellaneous
 * hardware error / CRC error / listing not initiated, $10 not found
 * (file, directory, LSN or image), $20 target in use / directory not empty
 * / deleted data mark (read, not an error), $40 image write protected.
 *
 * Commands (drive in bit 0):
 *  - $80 read logical sector, $A0 write: the LSN in $FF49 (high) and
 *    $FF4A:$FF4B; bit 1 the single-sided LSN flag (on a double-sided image
 *    LSN 18 means track 1 side 0: 36); bit 2 (read) / bit 3 (write) for
 *    8-bit transfers, every byte through $FF4B; else pairs, $FF4A first.
 *  - $90 stream the image file from the 512-byte block in $FF49-$FF4B to
 *    its end (bit 2: 8-bit); $D0 aborts it.
 *  - $C0 + $FF49: 'I' the image's 32-byte directory record, 'Q' its size in
 *    256-byte sectors (file size / 256) in $FF49-$FF4B, 'C' the current
 *    directory's record (at the root: FAILED + $10), '>' a listing page (16
 *    records of 16 bytes), '+' mount the next disk in the set, 'V' the
 *    firmware version (BCD in $FF4A/$FF4B: 1.24 here).
 *  - $D0 + '#' mount disk in set ($FF4A 0 next, 1-9 that one).
 *  - $E0 + a 256-byte block "X:path": M (mount; "M:" ejects), m (mount as
 *    raw blocks), N (mount, creating it: $FF49/$FF4A 0 a 630-sector DSK,
 *    else cylinders/sides of an SDF), n (create an empty raw file), D (set
 *    the current directory), L (start a listing: the last component a
 *    pattern), K (create a directory), X (delete a file or empty directory).
 *
 * Records: 32 bytes ('I', 'C': name 0-7, extension 8-10, attributes 11, size
 * 28-31 LSB first); listing 16 bytes (name, extension, attributes, size
 * 12-15 MSB first). Attributes: $10 directory, $04 SDF, $02 hidden, $01
 * locked (read only). Names must be 8.3; they are matched without regard
 * to case, may use * and ? in the file part, and a name without an
 * extension that doesn't exist matches name.*. Paths never leave the card.
 *
 * Images: DSK (256-byte sectors, at least 324; geometry by size: up to 720
 * sectors 40 tracks single sided, 1440 double sided, 2880 80 tracks
 * double sided; more is a hard disk whose FDC view is its first 1440
 * sectors as 80 single-sided tracks (read only here), the rest by LSN),
 * JVC and VDK (by their headers) and SDF (the SDC's own track format,
 * read and written). "m:" and "n:" mount raw blocks: LSN and stream only.
 * A directory mounted is a disk set: its files whose names end in 1-9.
 * STARTUP.CFG in the card's root ("0=", "1=", "D=") is read at power-on.
 *
 * Flash: 8 banks of 16 KB at $C000-$FEFF; the DIP switches pick the bank at
 * reset. The guide gives no bit layout for $FF42/$FF43: here $FF43 bits
 * 0-2 select the bank (and read back) and flash programming through $FF42
 * is not modelled (unverified).
 *
 * Timing: commands complete at once (READY or done when first polled).
 */
class CocoSdc {
public:
    static constexpr uint8_t ST_BUSY = 0x01;
    static constexpr uint8_t ST_READY = 0x02;
    static constexpr uint8_t ST_INVALID = 0x04;        ///< path name invalid
    static constexpr uint8_t ST_HARDWARE = 0x08;       ///< misc hardware error, CRC error, no listing
    static constexpr uint8_t ST_NOT_FOUND = 0x10;      ///< not found / invalid LSN / no image
    static constexpr uint8_t ST_IN_USE = 0x20;         ///< in use / not empty / deleted data mark
    static constexpr uint8_t ST_WRITE_PROTECT = 0x40;
    static constexpr uint8_t ST_FAILED = 0x80;
    static constexpr uint8_t COMMAND_MODE = 0x43;      ///< the $FF40 value that enters command mode
    static constexpr int BANK_SIZE = 0x4000;
    static constexpr int BANKS = 8;
    static constexpr uint16_t VERSION = 0x0124;        ///< the firmware version reported ('V')

    CocoSdc();
    ~CocoSdc();

    /** Insert the SD card (a host folder; "" none): power-on, STARTUP.CFG read. */
    void setCardRoot(const std::string& dir);
    const std::string& cardRoot() const { return m_root; }
    /** The current directory, relative to the card ("" at its root). */
    std::string currentDir() const;

    /** One 16 KB bank, or up to all 128 KB from bank 0. */
    void loadFlash(const std::vector<uint8_t>& image);
    bool hasFlash() const { return m_hasFlash; }
    int bank() const { return m_bank; }
    void setBank(int b) { m_bank = b & 7; }
    /** The DIP switches' bank (selected at reset). */
    void setDipBank(int b) { m_dipBank = b & 7; }
    /** A byte of the selected bank ($C000-$FEFF). */
    uint8_t romByte(uint16_t addr) const;

    void reset();
    bool commandMode() const { return m_cmdMode; }
    /** $FF40: $43 enters command mode; false when the value is for the drive latch. */
    bool writeLatch(uint8_t v);

    /** Registers 0-15 ($FF40-$FF4F): true when this answers the read. */
    bool read(int reg, uint8_t& v);
    bool peek(int reg, uint8_t& v) const;
    /** True when the write was the SDC's (not the latch's or the FDC's). */
    bool write(int reg, uint8_t v);

    /** The FDC half's drives 0 and 1. */
    std::function<bool(int, std::unique_ptr<FloppyDisk>)> insertFloppy;
    std::function<std::unique_ptr<FloppyDisk>(int)> removeFloppy;
    std::function<FloppyDisk*(int)> floppy;

    /** Mount a host file in drive 0/1 (CLI `disk mount`): as 'M' would. */
    bool mount(int drive, const std::string& hostPath, bool raw = false);
    void eject(int drive);
    /** The host path of drive 0/1's image, "" for none. */
    std::string imagePath(int drive) const;
    /** True when drive 0/1 holds an image the FDC can't see whole (hard disk, raw). */
    bool isHardDisk(int drive) const;
    /** Sectors (file size / 256) of drive 0/1's image. */
    uint32_t imageSectors(int drive) const;

    uint8_t status() const { return composeStatus(); }
    uint8_t param(int n) const { return m_param[n & 3]; }

private:
    enum class Phase { Idle, DataIn, DataOut, Stream };
    enum class Kind { None, Floppy, Sdf, HardDisk, Raw };
    struct Image {
        std::string path;
        Kind kind = Kind::None;
        bool readOnly = false;
        bool plainDsk = false;                 // a headerless DSK: saved here, as sectors
        std::unique_ptr<std::fstream> file;    // HardDisk, Raw
    };

    uint8_t composeStatus() const;
    void command(uint8_t c);
    void extended(uint8_t c);
    void finishDataOut();
    void fail(uint8_t bits);
    void beginDataIn(bool eightBit = false);
    void streamBlock();
    uint32_t lsn() const { return (uint32_t)m_param[1] << 16 | (uint32_t)m_param[2] << 8 | m_param[3]; }
    bool readLsn(int drive, uint32_t lsn, bool singleSided);
    void writeLsn(int drive, uint32_t lsn, bool singleSided);
    uint8_t mountPath(int drive, const std::string& path, bool raw);
    uint8_t mountFile(int drive, const std::string& host, bool raw);
    uint8_t mountSet(int number, bool next);
    uint8_t newImage(int drive, const std::string& path, bool raw);
    std::string blockString() const;
    uint64_t fileSize(int drive) const;
    // The card's filesystem
    uint8_t resolve(const std::string& name, std::string& host, bool mustExist) const;
    std::string findCase(const std::string& dir, const std::string& comp) const;
    std::string findWild(const std::string& dir, const std::string& pattern) const;
    uint8_t changeDir(const std::string& path);
    uint8_t startListing(const std::string& spec);
    void record32(const std::string& hostPath, uint8_t* e, bool sdf) const;
    void record16(const std::string& hostPath, uint8_t* e) const;
    std::vector<std::string> setMembers() const;
    void readStartup();

    std::string m_root;
    std::vector<std::string> m_cwd;            // host names of the components
    std::vector<uint8_t> m_flash;
    bool m_hasFlash = false;
    int m_bank = 0, m_dipBank = 0;
    bool m_cmdMode = false;
    uint8_t m_param[4] = {};                   // [1..3]; [0] the command
    uint8_t m_error = 0;                       // status bits after the command
    Phase m_phase = Phase::Idle;
    bool m_eightBit = false;
    uint8_t m_buf[512] = {};
    int m_pos = 0, m_len = 0;
    // Stream
    int m_streamDrive = 0;
    uint64_t m_streamBlock = 0;
    Image m_images[2];
    bool m_listing = false;
    std::vector<std::string> m_list;           // host paths of the listing
    size_t m_listPos = 0;
    // The disk set
    std::string m_setDir;
    int m_setDrive = 0, m_setNumber = 0;
};
