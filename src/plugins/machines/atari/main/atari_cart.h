#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace atari {

/**
 * A cartridge in the left slot: the ROM, its type (the .car header's
 * numbering, as atari800 and Altirra use it) and its bank-switching state.
 * It answers $8000-$9FFF (RD4) and $A000-$BFFF (RD5) while enabled, and
 * switches banks on reads and writes of $D500-$D5FF (CCTL).
 *
 * Supported: standard 8 and 16 KB; OSS (034M, 043M, M091, 8 KB); Williams
 * 16/32/64 KB; Express, Diamond and SpartaDOS X 64 KB; SpartaDOS X 128 KB;
 * DB 32 KB; XEGS and switchable XEGS 32 KB-1 MB; MegaCart 16 KB-2 MB;
 * Atarimax 128 KB and 1 MB; Phoenix 8 KB and Blizzard 16 KB.
 */
class Cartridge {
public:
    enum Type {
        STD_8 = 1, STD_16 = 2, OSS_034M_16 = 3, DB_32 = 5, WILL_64 = 8, EXP_64 = 9,
        DIAMOND_64 = 10, SDX_64 = 11, XEGS_32 = 12, XEGS_07_64 = 13, XEGS_128 = 14,
        OSS_M091_16 = 15, WILL_32 = 22, XEGS_256 = 23, XEGS_512 = 24, XEGS_1024 = 25,
        MEGA_16 = 26, MEGA_32 = 27, MEGA_64 = 28, MEGA_128 = 29, MEGA_256 = 30,
        MEGA_512 = 31, MEGA_1024 = 32, SWXEGS_32 = 33, SWXEGS_64 = 34, SWXEGS_128 = 35,
        SWXEGS_256 = 36, SWXEGS_512 = 37, SWXEGS_1024 = 38, PHOENIX_8 = 39,
        BLIZZARD_16 = 40, ATMAX_128 = 41, ATMAX_OLD_1024 = 42, SDX_128 = 43, OSS_8 = 44,
        OSS_043M_16 = 45, MEGA_2048 = 64, ATMAX_NEW_1024 = 75, WILL_16 = 76
    };

    /**
     * Load a .car file (a "CART" header with the type) or a raw 8 or 16 KB
     * ROM image (standard cartridge). False with `err` if neither or the
     * type is not supported.
     */
    bool load(const std::vector<uint8_t>& bytes, std::string& err);

    int type() const { return m_type; }
    /** The type's name ("XEGS 128 KB"...). */
    std::string typeName() const;

    /** Back to the power-on bank. */
    void reset();

    /** $8000-$9FFF and $A000-$BFFF are the cartridge's (else RAM or BASIC). */
    bool rd4() const;
    bool rd5() const;
    /** A byte of the cartridge window ($8000-$BFFF), while it is enabled. */
    uint8_t read(uint16_t addr) const;

    /** A CPU read of $D5xx (switches banks on some types); returns the bus value. */
    uint8_t readCctl(uint16_t addr);
    /** A CPU write to $D5xx. */
    void writeCctl(uint16_t addr, uint8_t value);

    /** The bank state (for snapshots and tests). */
    int state() const { return m_state; }
    void setState(int s) { m_state = s; }

private:
    void access(uint16_t addr);
    uint8_t at(size_t offset) const { return offset < m_rom.size() ? m_rom[offset] : 0xFF; }

    int m_type = 0;
    int m_state = 0;
    std::vector<uint8_t> m_rom;
};

}  // namespace atari
