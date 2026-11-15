#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * An AMD-style (JEDEC command set) parallel NOR flash: the part on the REX
 * cards (Am29LV160DB-type, bottom boot: VirtualT's device ID $49), 1 MB of
 * it used. After VirtualT's amd_flash_write (memory.c) and the Am29LV160D
 * data sheet. mmsim's own code.
 *
 * Commands (byte mode; only the address's low byte is compared, as
 * VirtualT does, since REX hands the chip $AA / $55 for $AAA / $555):
 *  - AA@AA 55@55 A0@AA, then data@addr: program (bits only go 1 -> 0);
 *  - AA@AA 55@55 80@AA AA@AA 55@55 10@AA: chip erase (all $FF);
 *  - ... 80@AA AA@AA 55@55 30@sector: sector erase. Sectors: 16 KB at 0,
 *    8 KB at $4000 and $6000, 32 KB at $8000, then 64 KB each (VirtualT
 *    computes the second 8 KB sector from address & $FFF000, which splits
 *    $5000-$6FFF; the data sheet's $6000-$7FFF is used here);
 *  - AA@AA 55@55 20@AA: unlock bypass; then A0, data@addr programs;
 *    90, 00 leaves it;
 *  - AA@AA 55@55 90@AA autoselect, 98@AA CFI query: entered and left
 *    with F0 (their reads are not modelled: REX reads the array directly);
 *  - F0 (any time but mid-program): reset to reading.
 * Busy: a program takes PROGRAM_CLOCKS, a sector erase SECTOR_ERASE_CLOCKS,
 * a chip erase CHIP_ERASE_CLOCKS (VirtualT's counts, in 8085 clocks);
 * busy(now) reports it.
 */
class AmdFlash {
public:
    static constexpr uint64_t PROGRAM_CLOCKS = 40;
    static constexpr uint64_t SECTOR_ERASE_CLOCKS = 1680000;
    static constexpr uint64_t CHIP_ERASE_CLOCKS = 19000000;

    explicit AmdFlash(size_t size = 0x100000) : m_data(size, 0xFF) {}

    /** A bus write (the command interface); `now` in clocks. */
    void write(uint32_t addr, uint8_t v, uint64_t now);
    uint8_t read(uint32_t addr) const { return m_data[addr % m_data.size()]; }
    bool busy(uint64_t now) const { return now < m_busyUntil; }
    void reset() { m_state = RO; m_busyUntil = 0; }

    std::vector<uint8_t>& data() { return m_data; }
    const std::vector<uint8_t>& data() const { return m_data; }
    /** Changed since the last clearDirty(). */
    bool dirty() const { return m_dirty; }
    void clearDirty() { m_dirty = false; }
    int state() const { return m_state; }

    enum State { RO, UNLK1, CMD, UNLK2, UNLK3, CMD2, PROG, BYPASS, BYPASS_PROG, BYPASS_EXIT, AUTOSELECT, CFI };

private:
    void eraseSector(uint32_t addr);

    std::vector<uint8_t> m_data;
    int m_state = RO;
    uint64_t m_busyUntil = 0;
    bool m_dirty = false;
};

/**
 * REX and REX# (Rex# is "REXS" in VirtualT): a flash card in the Model
 * 100/102/200's option ROM socket. The socket has no write line, so the
 * card is commanded by which addresses are read. After VirtualT
 * (memory.c rex_read, by Ken Pettit and Steve Adolph). mmsim's own code.
 *
 * Reading (state 7, from power-on): a read at $0000-$7FFF returns the
 * flash at the selected 32 KB sector (sector * $8000 + address). Six
 * consecutive reads whose address low bytes are $B8 $F2 $34 $B0 $31 $BF
 * (any read in between that is not the next key byte starts over) put the
 * card in command mode (state 0), where a read's address bits 0-2 are the
 * command and its low byte a parameter:
 *  - 0: back to reading;
 *  - 1: set sector: the next read's address bits 0-4 are the sector
 *       (0-31), bit 6 clear selects the flash (set: deselected, reads $FF);
 *  - 2: AAA,AA: the next read's low byte is written to the flash at
 *       sector | the command read's low byte (REX# : see below);
 *  - 3: status: this read and the next return 7 /BUSY (1 ready), 6 the
 *       flash selected, 0-4 the sector;
 *  - 4: read: the next read returns the flash at sector | (address &
 *       $7FFF), and so does the one after it;
 *  - 5: 555,55: the next read writes $55 to $55;
 *  - 6: program address: the next read's address & $7FFF is the address;
 *       the one after that's low byte the data (sector | address);
 *  - 7: ID: this read and the next return $10 | model (REX $11, REX# $13).
 * Command reads (1, 2, 4, 5, 6 and those that just feed a parameter)
 * return $FF.
 * REX# (no address latch) takes 2, 5 and 6 the other way round: the next
 * read's low byte is the data, the read after it the address (2 and 5:
 * sector | low byte; 6: sector | address & $7FFF).
 * Not modelled: REX2's RAM and the REXCPM (VirtualT's REX2 / REXC),
 * flash autoselect / CFI data through the socket.
 *
 * The image (1 MB, VirtualT's .bin) persists to a host file: loaded on
 * open (a missing file starts erased, $FF; a short one is padded),
 * written by flush() (the machine flushes a second after the last change
 * and on removal / destruction).
 */
class Rex {
public:
    enum class Kind { Rex, RexSharp };
    static constexpr size_t SIZE = 0x100000;

    explicit Rex(Kind k = Kind::Rex) : m_kind(k), m_flash(SIZE) {}
    ~Rex();

    bool open(const std::string& path);
    const std::string& path() const { return m_path; }
    bool flush();
    bool saveTo(const std::string& path) const;
    Kind kind() const { return m_kind; }

    /** A CPU read of the socket ($0000-$7FFF); `now` in clocks. */
    uint8_t read(uint16_t addr, uint64_t now);
    /** What a debugger sees (no state change). */
    uint8_t peek(uint16_t addr) const;
    /** Power-on / reset: back to reading sector 0 (the flash keeps its data). */
    void reset();

    AmdFlash& flash() { return m_flash; }
    int state() const { return m_state; }
    int sector() const { return (int)(m_sector >> 15); }
    uint8_t model() const { return m_kind == Kind::Rex ? 0x01 : 0x03; }

    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    Kind m_kind;
    AmdFlash m_flash;
    std::string m_path;
    int m_state = 7;
    int m_key = 0;
    uint32_t m_sector = 0;           // sector << 15
    bool m_flashSel = true;
    uint32_t m_pa = 0;
    uint8_t m_ret = 0xFF;
    uint8_t m_sharpData = 0;
    int m_sharpCmd = 0;
};
