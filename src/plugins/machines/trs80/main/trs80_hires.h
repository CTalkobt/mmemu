#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace trs80 {

/** Which hi-res graphics board is fitted (Model III/4). */
enum class HiresBoard { None, Tandy, MicroLabs };

/**
 * The Model III/4 hi-res graphics boards: Radio Shack's (26-1126 for the
 * Model 4) and Micro-Labs' Grafyx Solution. After xtrs (Tim Mann;
 * trs_xinterface.c, trs_io.c; MIT-style licence); mmsim's own code.
 *
 * 32 KB of graphics memory, 128 bytes (each 8 dots, bit 7 leftmost) by
 * 256 lines, of which 80 x 240 (640 x 240) are shown. Ports: $80 x (the
 * byte), $81 y (the line), $82 data (read/write at x, y), $83 mode (bit 0
 * graphics on; 1 Micro-Labs: text off / Radio Shack: wait states; 2/3 x/y
 * count down; 4/5 x/y don't move after a read; 6/7 after a write), and on
 * the Model 4 $8C the x offset, $8D the y offset, $8E bit 0 the overlay
 * (Radio Shack). With the overlay the graphics are XORed with the text,
 * dot for dot (byte x under text column x, line y under scan line y);
 * without it the screen is the graphics alone, 640 x 240.
 *
 * The Model III's Grafyx Solution also takes over port $FF writes with
 * bit 5 set (bit 6 on, 7 the video RAM addresses its memory, 4-1 the line
 * within a character row): then $3C00 + n is the byte at column n % 64 of
 * line (n / 64) * 12 + that line.
 */
class Hires {
public:
    static constexpr int XSIZE = 128, YSIZE = 256;

    void setBoard(HiresBoard b) { m_board = b; reset(); }
    HiresBoard board() const { return m_board; }
    bool fitted() const { return m_board != HiresBoard::None; }
    void reset() {
        m_x = m_y = m_mode = 0;
        m_enable = m_overlay = false;
        m_xoff = m_yoff = 0;
        m_m3Mode = 0;
    }
    void clear() { std::memset(m_mem, 0, sizeof m_mem); }

    // Ports $80-$83, $8C-$8E
    void writeX(uint8_t v) { m_x = v; }
    void writeY(uint8_t v) { m_y = v; }
    void writeData(uint8_t v) {
        m_mem[m_y][m_x % XSIZE] = v;
        step(0x40, 0x80);
    }
    uint8_t readData() {
        const uint8_t v = m_mem[m_y][m_x % XSIZE];
        step(0x10, 0x20);
        return v;
    }
    uint8_t peekData() const { return m_mem[m_y][m_x % XSIZE]; }
    void writeMode(uint8_t v) {
        m_enable = v & 1;
        if (m_board == HiresBoard::MicroLabs) m_overlay = !(v & 2);
        m_mode = v;
    }
    void writeXOffset(uint8_t v) { m_xoff = (uint8_t)(v % XSIZE); }
    void writeYOffset(uint8_t v) { m_yoff = v; }
    void writeOverlay(uint8_t v) { m_overlay = v & 1; }

    // The Model III's Grafyx Solution through port $FF and video RAM
    bool m3Mode(uint8_t v) {
        if (m_board != HiresBoard::MicroLabs || !(v & 0x20)) return false;
        m_enable = m_overlay = v & 0x40;
        m_m3Mode = v;
        m_mode = v;
        m_y = (uint8_t)((v & 0x1E) >> 1);
        return true;
    }
    bool m3Active() const { return m_board == HiresBoard::MicroLabs && (m_m3Mode & 0x80); }
    uint8_t& m3Byte(int position) { return m_mem[(position / 64) * 12 + m_y][position % 64]; }

    // The display
    bool enabled() const { return m_board != HiresBoard::None && m_enable; }
    bool overlay() const { return m_overlay; }
    /** Graphics alone (640 x 240). */
    bool alone() const { return enabled() && !m_overlay; }
    /** The byte shown at screen column `col`, scan line `line`. */
    uint8_t shown(int col, int line) const {
        return m_mem[(line + m_yoff) % YSIZE][(col + m_xoff) % XSIZE];
    }
    uint8_t byteAt(int x, int y) const { return m_mem[y % YSIZE][x % XSIZE]; }
    uint8_t x() const { return m_x; }
    uint8_t y() const { return m_y; }
    uint8_t mode() const { return m_mode; }

    /** For snapshots. */
    std::vector<uint8_t> state() const {
        std::vector<uint8_t> v(&m_mem[0][0], &m_mem[0][0] + sizeof m_mem);
        v.insert(v.end(), {m_x, m_y, m_mode, (uint8_t)m_enable, (uint8_t)m_overlay, m_xoff, m_yoff, m_m3Mode});
        return v;
    }
    static size_t stateSize() { return sizeof(uint8_t) * XSIZE * YSIZE + 8; }
    void loadState(const uint8_t* p) {
        std::memcpy(m_mem, p, sizeof m_mem);
        p += sizeof m_mem;
        m_x = p[0]; m_y = p[1]; m_mode = p[2]; m_enable = p[3]; m_overlay = p[4];
        m_xoff = p[5]; m_yoff = p[6]; m_m3Mode = p[7];
    }

private:
    void step(uint8_t xHold, uint8_t yHold) {
        if (!(m_mode & xHold)) m_x = (uint8_t)(m_x + ((m_mode & 0x04) ? -1 : 1));
        if (!(m_mode & yHold)) m_y = (uint8_t)(m_y + ((m_mode & 0x08) ? -1 : 1));
    }

    HiresBoard m_board = HiresBoard::None;
    uint8_t m_mem[YSIZE][XSIZE] = {};
    uint8_t m_x = 0, m_y = 0, m_mode = 0, m_xoff = 0, m_yoff = 0, m_m3Mode = 0;
    bool m_enable = false, m_overlay = false;
};

}  // namespace trs80
