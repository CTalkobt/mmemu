#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * RAMPAC / Node Datapac (and the later EXTRAM / MiniNDP work-alikes):
 * battery-backed RAM on the Model 100/102/200's system bus, reached a byte
 * at a time through I/O ports - a RAM disk for Node's ROM and Paul
 * Globman's RAMDSK. From the Datapac's schematic and theory of operation
 * in bkw777's NODE_DATAPAC (and RAMDSK's source there); VirtualT's
 * remem_in / remem_out first. mmsim's own code.
 *
 * The card decodes /Y0 (ports $80-$8F) and the port's bits 0 and 1 (the
 * bus's A8, A9) on any I/O cycle:
 *  - bit 0 = 0: nothing ($80, $82, ... are not the card's).
 *  - bits 1-0 = 01, SELECT BLOCK: the byte counter goes to 0 and the data
 *    bus is latched as the block number (0-255). A write sets it (OUT $81);
 *    on a read (IN $81) the card drives nothing (reads $FF) and the block
 *    latched is whatever floats on the bus - mmsim clears the counter and
 *    keeps the block (unverified).
 *  - bits 1-0 = 11, BYTE: the RAM byte at block * 1024 + counter is read
 *    (IN $83) or written (OUT $83), then the 10-bit counter steps; after
 *    byte 1023 it wraps to 0 in the same block.
 *  - Bigger cards take the bank from the select's port bits 2-3 (bus A10,
 *    A11): bank n at $81 + 4n. A 256 KB card ignores them (every bank is
 *    bank 0), a 512 KB card uses bit 2 (banks 0 and 1: $85 is bank 1,
 *    $89 bank 0 again, $8D bank 1), a 1 MB card both. This is how RAMDSK
 *    finds the banks.
 *  - A 128 KB Datapac has no RAM for blocks 128-255: writes there are lost
 *    and reads give $FF (what RAMDSK's 128 KB test looks for).
 *
 * The memory is the battery's: kept across power-off and reset, and
 * persisted to a host file (raw, bank 0 first: a 256 KB card's file is
 * VirtualT's .bin layout) when one is given: loaded on open (a missing file
 * starts as zeros, a short one is padded), written by flush() (the machine
 * flushes it a second after the last write and on detach / destruction).
 */
class Rampac {
public:
    static constexpr int SECTOR_SIZE = 1024;                       ///< a block
    static constexpr int SECTORS = 256;                            ///< blocks in a bank
    static constexpr size_t BANK_SIZE = (size_t)SECTOR_SIZE * SECTORS;
    static constexpr size_t SIZE = BANK_SIZE;                      ///< the standard (256 KB) card

    /** `kb`: 128, 256, 512 or 1024 (anything else: 256). */
    explicit Rampac(int kb = 256);
    ~Rampac();

    /** Back it with `path` (empty: memory only); false if the file can't be read or made. */
    bool open(const std::string& path);
    const std::string& path() const { return m_path; }
    /** Write the memory to the file, if any and changed; false on an error. */
    bool flush();
    /** Save the memory to `path` (an image). */
    bool saveTo(const std::string& path) const;
    bool dirty() const { return m_dirty; }

    /** True if the port is one of the card's (port & $FF): $80-$8F, odd. */
    static bool decodes(uint8_t port) { return (port & 0xF1) == 0x81; }
    uint8_t in(uint8_t port);
    void out(uint8_t port, uint8_t v);
    uint8_t peek(uint8_t port) const;

    int sizeKb() const { return m_kb; }
    int banks() const { return m_banks; }
    uint8_t sector() const { return m_sector; }
    int bank() const { return m_bank; }
    int counter() const { return m_counter; }
    /** All the banks, bank 0 first (a 128 KB card: blocks 128-255 unused). */
    std::vector<uint8_t>& ram() { return m_ram; }

    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    bool present() const { return m_kb != 128 || m_sector < 128; }
    size_t addr() const { return (size_t)m_bank * BANK_SIZE + (size_t)m_sector * SECTOR_SIZE + m_counter; }
    void select(uint8_t port);

    int m_kb = 256;
    int m_banks = 1;
    std::vector<uint8_t> m_ram;
    std::string m_path;
    uint8_t m_sector = 0;
    int m_bank = 0;
    int m_counter = 0;
    bool m_dirty = false;
};
