#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * RAMPAC / Node Datapac: 256 KB of battery-backed RAM on the Model 100's
 * system bus, reached a byte at a time through two I/O ports (a RAM disk
 * for software such as Node's and RAMPAC's own DOS). After VirtualT
 * (memory.c remem_in / remem_out, memory.h: RAMPAC_SECTOR_PORT $81,
 * RAMPAC_DATA_PORT $83, 256 sectors of 1024 bytes). mmsim's own code.
 *
 *  - OUT $81 (and, as VirtualT also decodes it, OUT $85): the sector
 *    (0-255); the byte counter goes to 0.
 *  - IN $81: the sector.
 *  - IN / OUT $83: the byte at sector * 1024 + counter, then the counter
 *    steps; after byte 1023 the sector steps (255 wraps to 0).
 * Other ports in $80-$8F are not decoded (read $FF); whether the real
 * cards decode fewer address lines (mirrors) is unverified, as is any
 * difference between the RAMPAC and the Node Datapac (treated as the same).
 *
 * The memory is the battery's: kept across power-off and reset, and
 * persisted to a host file (raw 256 KB, VirtualT's .bin layout) when one
 * is given: loaded on open (a missing file starts as zeros, a short one is
 * padded), written by flush() (the machine flushes it a second after the
 * last write and on detach / destruction).
 */
class Rampac {
public:
    static constexpr int SECTOR_SIZE = 1024;
    static constexpr int SECTORS = 256;
    static constexpr size_t SIZE = (size_t)SECTOR_SIZE * SECTORS;

    Rampac() : m_ram(SIZE, 0) {}
    ~Rampac();

    /** Back it with `path` (empty: memory only); false if the file can't be read or made. */
    bool open(const std::string& path);
    const std::string& path() const { return m_path; }
    /** Write the memory to the file, if any and changed; false on an error. */
    bool flush();
    /** Save the memory to `path` (an image). */
    bool saveTo(const std::string& path) const;
    bool dirty() const { return m_dirty; }

    /** True if the port is one of the RAMPAC's (port & $FF). */
    static bool decodes(uint8_t port) { return port == 0x81 || port == 0x83 || port == 0x85; }
    uint8_t in(uint8_t port);
    void out(uint8_t port, uint8_t v);
    uint8_t peek(uint8_t port) const;

    uint8_t sector() const { return m_sector; }
    int counter() const { return m_counter; }
    std::vector<uint8_t>& ram() { return m_ram; }

    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    void step();

    std::vector<uint8_t> m_ram;
    std::string m_path;
    uint8_t m_sector = 0;
    int m_counter = 0;
    bool m_dirty = false;
};
