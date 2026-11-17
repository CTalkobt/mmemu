#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * The REXCPM (Steve Adolph's): 4 MB of static RAM that takes the place of
 * the Model 100/102's RAM ($8000-$FFFF) and of its option ROM ($0000-
 * $7FFF when the option ROM is selected), through three window registers.
 * After VirtualT's rexC_read / rexC_set8 (memory.c, also by S. Adolph).
 * mmsim's own code.
 *
 *  - $8000-$BFFF: 16 KB block RL (the lower RAM register), $C000-$FFFF:
 *    16 KB block RU; the option ROM: 32 KB block O. Power-on/reset: O = 1,
 *    RL = 0, RU = 1, write protect on.
 *  - Write protect: 16 KB blocks 2 and 3 (32 KB block 1, where the
 *    firmware lives) and 8 and up can't be written; off, everything can.
 *  - The registers are set by reads. Every CPU memory read is counted
 *    (debugger peeks are not). Idle, a read in $8000-$FFFF whose high
 *    address byte is the key's first ($B8) starts the count at 1; the
 *    key's next bytes ($F2 $C4 $ED $A1 $98) must be the high bytes of the
 *    RAM reads that are counted 5, 9, 13, 17 and 21, else it starts over.
 *    Then the RAM read counted 23 or 24 sets the high bits of the
 *    register its address bits 14-12 pick (1 O: bits 11-8 -> bits 6-3; 2
 *    RL, 3 RU: bits 11-8 -> bits 7-4), and the read counted 26 completes
 *    it and returns: by bits 14-12, 0 write protect = bit 8 (returned); 1
 *    O += bits 11-9 (returns O x 2); 2 RL / 3 RU += bits 11-8 (returns
 *    it); 4 the model ($E8); 5-7 read O x 2, RL, RU back.
 *  - RAM reads while counting return the RAM as usual.
 */
class RexCpm {
public:
    static constexpr size_t SIZE = 4 * 1024 * 1024;
    static constexpr uint8_t MODEL = 0xE8;

    RexCpm() : m_ram(SIZE, 0) {}
    ~RexCpm() { flush(); }

    /** Back the RAM with `path` (read if there, made if not; "": memory only). */
    bool open(const std::string& path);
    void flush();
    bool saveTo(const std::string& path) const;
    const std::string& path() const { return m_path; }

    /** The registers to their power-on state (the computer's reset). */
    void reset();

    /** A CPU read: `optionRom` = the option ROM is selected; `rom` the main ROM (32 KB). */
    uint8_t read(uint16_t a, bool optionRom, const std::vector<uint8_t>& rom);
    /** The same without counting or stepping (the debugger). */
    uint8_t peek(uint16_t a, bool optionRom, const std::vector<uint8_t>& rom) const;
    void write(uint16_t a, uint8_t v, bool optionRom);

    uint8_t optionBlock() const { return m_o; }
    uint8_t lowerBlock() const { return m_rl; }
    uint8_t upperBlock() const { return m_ru; }
    bool writeProtect() const { return m_wp; }
    std::vector<uint8_t>& ram() { return m_ram; }

    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    uint8_t ramAt(uint16_t a) const;
    uint8_t command(uint16_t a);

    std::vector<uint8_t> m_ram;
    std::string m_path;
    bool m_dirty = false;

    uint8_t m_o = 1, m_rl = 0, m_ru = 1;
    bool m_wp = true;
    int m_state = 15;           // 15 idle, 14-10 the key, 0 and 1 the register cycle
    int m_keyState = 0;
    int m_count = 1;            // the read counter
    bool m_clear = true;        // the counter restarts at 1 on the next read
};
