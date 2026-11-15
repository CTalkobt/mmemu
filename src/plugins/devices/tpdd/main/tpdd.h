#pragma once

#include "plugins/devices/serial_endpoint/main/serial_endpoint.h"
#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

/**
 * Tandy Portable Disk Drive (26-3808, "TPDD") and Portable Disk Drive 2
 * (26-3814, "TPDD2") on a serial line: a SerialEndpoint, so it plugs into
 * any mmsim UART (the Model 100's IM6402; the machine offers it as
 * `serial tpdd:PATH` / `serial tpdd2:PATH`, via registerSchemes()).
 *
 * After the Bitchin100 wiki ("Base Protocol", "TPDD-2 Sector Access
 * Protocol", Desk-Link/TS-DOS notes), Brian K. White's pdd.sh (the FDC-mode
 * and TPDD2 command reference) and VirtualT's tpddserver.cpp (the TS-DOS
 * directory extensions). mmsim's own code.
 *
 * Operation mode (both models): requests `5A 5A type len data... sum`,
 * sum = ~(type + len + data) & $FF; returns `type len data... sum`.
 *  - $00 directory reference: 24-byte name, attribute, form (0 reference
 *        a name, 1 first, 2 next, 3 previous, 4 end) -> $11 return: name
 *        (24, zeros when none), attribute, size (2, big-endian), free
 *        sectors (x 1280 bytes)
 *  - $01 open: mode 1 new file (write), 2 append, 3 read -> $12 return
 *  - $02 close, $03 read (-> $10 with 0-128 bytes, or $12 error 3F at
 *        the end), $04 write (1-128 bytes), $05 delete, $06 format,
 *        $07 drive status, $0C drive condition (-> $15), $0D rename
 *  - $08 TPDD1: switch to FDC mode (no answer). TPDD2: parameter error.
 *        After "M1\r" (TS-DOS's probe) it is the Desk-Link/TS-DOS
 *        directory-management (DME) request instead: the answer names the
 *        current directory ("ROOT  .<> "). "M1\r" holds across other
 *        requests (TS-DOS asks for the status in between) until the CRs
 *        TS-DOS sends after the answer, or any other byte, end it.
 *  - TPDD2 only: bank 1 by adding $40 to the type; $23 version (-> $14),
 *        $33 system info (-> $3A), $30 sector cache load/commit (-> $38),
 *        $31 cache memory write (-> $38), $32 cache memory read (-> $39).
 * Errors ($12): 00 ok, 10 no such file, 11 file exists, 30 no file name,
 * 31 directory search error, 35 bank error, 36 parameter error, 37 open
 * format mismatch, 3F end of file, 41 checksum (ID CRC), 50 write
 * protected, 60 directory full, 61 disk full, 6E file too long, 70 no disk.
 *
 * Storage, operation mode: a host directory (`setRootDirectory`). Files
 * are listed in the Model 100's 6.2 form ("NAME  .BA", padded to 24);
 * a host name that does not fit is listed as-is (up to 24 characters).
 * Names match case-insensitively. Sub-directories appear as TS-DOS
 * "NAME.<>" entries (open for read enters one, "PARENT.<>" leaves it,
 * open for write on a ".<>" name makes one, delete removes an empty one),
 * as VirtualT's server does. Both TPDD2 banks see the same directory.
 * Format is a no-op on a directory (it would delete the host's files).
 * Files longer than 65535 bytes are listed as 65535.
 *
 * Storage, sector level: a disk image (`loadImage`): the TPDD1's FDC mode
 * and the TPDD2's sector cache work on it. 80 (TPDD1) or 160 (TPDD2)
 * physical sectors of 1280 bytes, each with a 12-byte ID area and a
 * logical-sector-size code (0-6: 64, 80, 128, 256, 512, 1024, 1280 bytes).
 * Files: pdd.sh's .pdd1/.pdd2 (per sector: LSC, 12 ID bytes, 1280 data)
 * or a raw 102400 / 204800-byte dump (IDs zero, LSC 0). Writes go back to
 * the file. Without an image there is a blank in-memory disk.
 *
 * FDC mode (TPDD1): ASCII commands `C[ ]P[,P]\r`, answers 8 hex
 * characters "EEDDLLLL" (error, data, length):
 *  - M0 / M1 select FDC / operation mode (no answer)
 *  - D condition (data: $20 write protected, $40 changed, $80 no disk)
 *  - F[n] / G[n] format (with / without verify) with LSC n (default 3)
 *  - A n read ID: answer (data = sector, length = logical size); then
 *        on CR the 12 ID bytes
 *  - B n / C n write ID: answer; then 12 bytes; then an answer
 *  - R p,l read logical sector l (1-based) of physical sector p: answer
 *        (data = p, length = logical size); on CR the sector's bytes
 *  - W p,l / X p,l write logical sector: answer; the bytes; an answer
 *  - S search ID: answer; then 12 bytes; answer with data = the first
 *        physical sector whose ID matches (error 3C and $FF if none)
 * Errors: 11/12 logical sector below/above range, 13 physical sector
 * above range, 21 bad parameter, 3C ID not found, B0 write protected,
 * C0 invalid command, D1 no disk (pdd.sh's experimentally found codes).
 *
 * Timing: answers are queued at once; the UART paces them at its baud
 * rate. The drive's mechanical delays are not modelled.
 */
class Tpdd : public SerialEndpoint {
public:
    enum class Model { Tpdd1, Tpdd2 };

    static constexpr int SECTOR_SIZE = 1280;
    static constexpr int ID_SIZE = 12;

    struct Sector {
        uint8_t lsc = 0;
        std::array<uint8_t, ID_SIZE> id{};
        std::array<uint8_t, SECTOR_SIZE> data{};
    };

    explicit Tpdd(Model m = Model::Tpdd1);
    ~Tpdd() override;

    Model model() const { return m_model; }

    /** Operation-mode storage: a host directory (false if it isn't one). */
    bool setRootDirectory(const std::string& dir);
    const std::string& rootDirectory() const { return m_root; }
    /** The current TS-DOS sub-directory, relative to the root ("" = root). */
    const std::string& currentDirectory() const { return m_cwd; }

    /** Sector-level storage: a .pdd1/.pdd2 or raw image (false if unreadable). */
    bool loadImage(const std::string& path);
    /** Writes the sector image (.pdd1/.pdd2 layout, or raw if `raw`). */
    bool saveImage(const std::string& path, bool raw = false) const;
    const std::vector<Sector>& sectors() const { return m_sectors; }
    std::vector<Sector>& sectors() { return m_sectors; }

    void setWriteProtect(bool wp) { m_writeProtect = wp; }
    bool writeProtected() const { return m_writeProtect; }
    /** No disk inserted (both modes report it). */
    void setDiskPresent(bool present) { m_diskPresent = present; }

    /** True while a TPDD1 is in FDC mode. */
    bool fdcMode() const { return m_fdc; }

    // SerialEndpoint
    void send(uint8_t b) override;
    bool receive(uint8_t& b) override;
    std::string describe() const override;

    /** Everything queued for the computer, drained (tests). */
    std::vector<uint8_t> takeOutput();

    /** Builds a request packet (preamble, type, length, data, checksum). */
    static std::vector<uint8_t> request(uint8_t type, const std::vector<uint8_t>& data);
    static uint8_t checksum(const uint8_t* p, size_t n);
    /** A 6.2 name in the 24-byte TPDD field ("NAME  .BA" + spaces). */
    static std::string tpddName(const std::string& hostName);

    /** Registers the `tpdd:` and `tpdd2:` serial schemes (idempotent). */
    static void registerSchemes();

private:
    enum class RxState { Idle, Z, Type, Len, Data, Sum, M, M1 };
    struct Entry {
        std::string host;    // the host name
        std::string name;    // the 24-byte TPDD name
        bool dir = false;
        bool exists = true;
        uint32_t size = 0;
    };

    // Operation mode
    void opByte(uint8_t b);
    void execute(uint8_t type, const std::vector<uint8_t>& d);
    void reply(uint8_t type, const std::vector<uint8_t>& d);
    void normal(uint8_t err) { reply(0x12, {err}); }
    void replyEntry(const Entry* e);
    std::vector<Entry> listDir() const;
    std::string dirPath() const;
    bool findEntry(const std::string& tpddName, Entry& out) const;
    static std::string hostNameFor(const std::string& tpddName, bool& isDir);
    uint8_t freeSectors() const;
    void opDirectory(const std::vector<uint8_t>& d);
    void opOpen(const std::vector<uint8_t>& d);
    void opRead();
    void opWrite(const std::vector<uint8_t>& d);
    void opDelete();
    void opRename(const std::vector<uint8_t>& d);
    void opCache(const std::vector<uint8_t>& d);
    void opMemWrite(const std::vector<uint8_t>& d);
    void opMemRead(const std::vector<uint8_t>& d);
    void closeFile();

    // FDC mode
    void fdcByte(uint8_t b);
    void fdcCommand(const std::string& line);
    void fdcAnswer(uint8_t err, uint8_t dat, uint16_t len);
    int logicalSize(int sector) const;
    void persistImage();

    Model m_model;
    std::string m_root, m_cwd;
    bool m_writeProtect = false, m_diskPresent = true;
    std::deque<uint8_t> m_out;

    RxState m_rx = RxState::Idle;
    uint8_t m_type = 0, m_len = 0;
    std::vector<uint8_t> m_data;
    bool m_dmePending = false;
    int m_dmeCrs = 0;              // after a DME answer: CRs seen + 1 (0: none expected)

    // The referenced file and the open one
    bool m_haveRef = false;
    Entry m_ref;
    FILE* m_file = nullptr;
    int m_openMode = 0;
    bool m_lastOpenDir = false;
    std::vector<Entry> m_listing;
    int m_listPos = -1;

    // Sector storage
    std::vector<Sector> m_sectors;
    std::string m_imagePath;
    bool m_imageRaw = false;
    std::array<uint8_t, SECTOR_SIZE> m_cache{};
    std::array<uint8_t, ID_SIZE + 1> m_cacheId{};

    // FDC mode
    bool m_fdc = false;
    std::string m_line;
    enum class FdcWait { Line, Bytes, Cr } m_fdcWait = FdcWait::Line;
    char m_fdcCmd = 0;
    int m_fdcSector = 0, m_fdcLogical = 0;
    size_t m_fdcNeed = 0;
    std::vector<uint8_t> m_fdcBuf;
};
