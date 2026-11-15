#pragma once

#include "hd44102.h"
#include <array>
#include <cstdint>
#include <vector>

/**
 * The Model 100 / 102 (and Kyocera KC-85, Olivetti M10, NEC PC-8201A)
 * 240x64 LCD: ten HD44102 column drivers plus the HD44103 common (row)
 * driver, which only scans rows and has no programmable state.
 *
 * Wiring (MAME kyocera_v.cpp `set_screen_offsets((i % 5) * 50, (i / 5) * 32)`,
 * VirtualT display.cpp SetByte, the Model 100 Technical Reference):
 *
 *   chip:   0     1     2     3     4         x 0-49, 50-99, 100-149, 150-199, 200-239
 *   rows 0-31  : chips 0-4
 *   rows 32-63 : chips 5-9
 *
 * Each chip drives 50 columns x 32 rows; chips 4 and 9 have only their
 * columns 0-39 connected (4 x 50 + 40 = 240).
 *
 * Chip selects: 10 lines, active high, from the 81C55: port A bits 0-7
 * select chips 0-7, port B bits 0-1 chips 8 and 9 (the same lines strobe the
 * keyboard columns). An access goes to every selected chip at once (the ROM
 * selects them all to send a command to the whole panel); reads OR the
 * selected chips' outputs, as the shared bus does in MAME. The M100 decodes
 * the chips at I/O $F0-$FF: A0 = 0 instruction/status, A0 = 1 data.
 */
class M100Lcd {
public:
    static constexpr int WIDTH = 240;
    static constexpr int HEIGHT = 64;
    static constexpr int CHIPS = 10;

    /** The ten chip-select lines (bit n selects chip n). */
    void setChipSelect(uint16_t mask) { m_select = mask & 0x3FF; }
    uint16_t chipSelect() const { return m_select; }
    /** Port A / port B of the 81C55 as wired on the M100. */
    void setSelectFromPorts(uint8_t portA, uint8_t portB) { setChipSelect((uint16_t)(portA | ((portB & 3) << 8))); }

    /** An I/O read at offset (A0 picks status or data). 0 when nothing selected. */
    uint8_t read(int offset);
    void write(int offset, uint8_t v);

    void reset();
    void tick(uint64_t units);
    void setBusyTime(uint32_t units);

    Hd44102& chip(int i) { return m_chips[i]; }
    const Hd44102& chip(int i) const { return m_chips[i]; }

    /** The panel dot at (x 0-239, y 0-63). */
    bool pixel(int x, int y) const;
    /** The 240x64 panel, one byte per dot (0/1), row-major. */
    void render(uint8_t* out) const;
    std::vector<uint8_t> render() const;

    /** Where chip i's column 0, row 0 lands on the panel. */
    static int chipX(int i) { return (i % 5) * 50; }
    static int chipY(int i) { return (i / 5) * 32; }
    /** Visible columns of chip i (40 for chips 4 and 9). */
    static int chipColumns(int i) { return (i % 5) == 4 ? 40 : 50; }

    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    std::array<Hd44102, CHIPS> m_chips;
    uint16_t m_select = 0;
};
