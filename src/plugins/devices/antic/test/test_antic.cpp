#include "test_harness.h"
#include "plugins/devices/antic/main/antic.h"
#include "plugins/devices/gtia/main/gtia.h"
#include "libmem/main/ibus.h"
#include <vector>

// ANTIC (#10): timing, interrupts, WSYNC, display lists, modes, scrolling,
// player/missile DMA; rendered through GTIA.

namespace {
class MockSignal : public ISignalLine {
public:
    bool get() const override { return m_level; }
    void set(bool level) override { if (level && !m_level) ++m_rises; m_level = level; }
    void pulse() override { ++m_rises; }
    bool m_level = false;
    int m_rises = 0;
};

class MockBus : public IBus {
public:
    const BusConfig& config() const override { return m_config; }
    const char* name() const override { return "mock"; }
    uint8_t read8(uint32_t addr) override { return m_mem[addr & 0xFFFF]; }
    void write8(uint32_t addr, uint8_t val) override { m_mem[addr & 0xFFFF] = val; }
    uint8_t peek8(uint32_t addr) override { return m_mem[addr & 0xFFFF]; }
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}
    BusConfig m_config = {16, 8, BusRole::DMA, true, 0xFFFF};
    uint8_t m_mem[65536] = {0};
};

/** ANTIC + GTIA on a flat 64 KB, a display list at $1000, screen at $2000, font at $E000. */
struct Video {
    MockBus bus;
    GTIA gtia;
    Antic antic;
    MockSignal nmi;
    std::vector<uint32_t> frame = std::vector<uint32_t>(Antic::FRAME_W * Antic::FRAME_H);
    Video() {
        antic.setDmaBus(&bus);
        antic.setGtia(&gtia);
        antic.setNmiLine(&nmi);
        antic.reset();
        gtia.reset();
        w(0xD402, 0x00); w(0xD403, 0x10);     // DLIST $1000
        w(0xD409, 0xE0);                      // CHBASE $E000
        g(GTIA::COLBK, 0x00); g(GTIA::COLPF0, 0x46); g(GTIA::COLPF1, 0x0E);
        g(GTIA::COLPF2, 0x94); g(GTIA::COLPF3, 0xC8);
    }
    void w(uint16_t a, uint8_t v) { antic.ioWrite(nullptr, a, v); }
    void g(int reg, uint8_t v) { gtia.ioWrite(nullptr, 0xD000 + reg, v); }
    void dl(std::initializer_list<int> bytes) {
        uint16_t a = 0x1000;
        for (int b : bytes) bus.m_mem[a++] = (uint8_t)b;
    }
    void frameRun() { antic.tick((uint64_t)Antic::CYCLES_PER_LINE * Antic::LINES_NTSC); antic.renderFrame(frame.data()); }
    uint32_t px(int x, int line) const { return frame[(size_t)(line - Antic::FIRST_LINE) * Antic::FRAME_W + x]; }
    static uint32_t col(uint8_t c) { return GTIA::getARGB(c & 0xFE); }
};
}  // namespace

TEST_CASE(antic_vcount_counts_line_pairs) {
    Antic antic;
    uint8_t v = 0;
    antic.tick(114 * 5);
    antic.ioRead(nullptr, 0xD40B, &v);
    ASSERT_EQ((int)v, 2);                 // line 5
    antic.tick(114 * 257);                // to line 262 = line 0 of the next frame
    antic.ioRead(nullptr, 0xD40B, &v);
    ASSERT_EQ((int)v, 0);
    ASSERT_EQ((int)antic.frameCount(), 1);
}

TEST_CASE(antic_vbi_and_dli_nmi) {
    Video v;
    v.w(0xD40E, 0xC0);                                        // NMIEN: DLI + VBI
    v.w(0xD400, 0x22);                                        // DL DMA, normal width
    // 8 blank lines, the last with DLI; then JVB
    v.dl({0xF0, 0x41, 0x00, 0x10});
    v.antic.tick((uint64_t)114 * 8 + 9);                      // line 8, past cycle 8
    ASSERT_EQ(v.nmi.m_rises, 0);
    v.antic.tick((uint64_t)114 * 7);                          // line 15: the 8th blank line
    ASSERT_EQ(v.nmi.m_rises, 1);
    uint8_t st = 0;
    v.antic.ioRead(nullptr, 0xD40F, &st);
    ASSERT_EQ((int)(st & 0xE0), 0x80);                        // DLI
    v.antic.tick((uint64_t)114 * (248 - 15));                 // the vertical blank
    v.antic.ioRead(nullptr, 0xD40F, &st);
    ASSERT_EQ((int)(st & 0xE0), 0x40);                        // VBI set, DLI cleared
    ASSERT_EQ(v.nmi.m_rises, 2);
    v.w(0xD40F, 0);                                           // NMIRES
    v.antic.ioRead(nullptr, 0xD40F, &st);
    ASSERT_EQ((int)(st & 0xE0), 0);
}

TEST_CASE(antic_wsync_blocks_to_line_end) {
    Antic antic;
    antic.tick(10);
    ASSERT(!antic.cpuBlocked());
    antic.ioWrite(nullptr, 0xD40A, 0);
    ASSERT(antic.cpuBlocked());
    antic.tick(Antic::RENDER_CYCLE - 10);          // cycles 10-103
    ASSERT(antic.cpuBlocked());
    antic.tick(1);
    ASSERT(!antic.cpuBlocked());
}

TEST_CASE(antic_mode2_text_through_gtia) {
    Video v;
    v.w(0xD400, 0x22);
    v.w(0xD401, 0x02);                                        // CHACTL: inverse video
    // 24 blank lines, mode 2 with LMS $2000, then JVB
    v.dl({0x70, 0x70, 0x70, 0x42, 0x00, 0x20, 0x41, 0x00, 0x10});
    v.bus.m_mem[0x2000] = 0x21;                               // 'A'
    v.bus.m_mem[0x2001] = 0xA1;                               // inverse 'A'
    for (int r = 0; r < 8; ++r) v.bus.m_mem[0xE000 + 0x21 * 8 + r] = 0xF0;   // left half lit
    v.frameRun();
    int line = 8 + 24;                                        // the mode line's first scan line
    uint32_t lit = Video::col((0x94 & 0xF0) | 0x0E), bg = Video::col(0x94);
    ASSERT_EQ(v.px(32, line), lit);                           // normal playfield starts at pixel 32
    ASSERT_EQ(v.px(35, line), lit);
    ASSERT_EQ(v.px(36, line), bg);
    ASSERT_EQ(v.px(40, line), bg);                            // inverse: the left half dark
    ASSERT_EQ(v.px(44, line), lit);
    ASSERT_EQ(v.px(10, line), Video::col(0x00));              // border: COLBK
    ASSERT_EQ(v.px(32, line - 1), Video::col(0x00));          // blank line above
    ASSERT_EQ(v.px(32, line + 8), Video::col(0x00));          // after JVB: blank
}

TEST_CASE(antic_map_modes_and_widths) {
    Video v;
    v.w(0xD400, 0x21);                                        // narrow playfield
    v.dl({0x4E, 0x00, 0x20, 0x41, 0x00, 0x10});               // mode E, LMS $2000
    v.bus.m_mem[0x2000] = 0x1B;                               // pixels 0,1,2,3 = 00 01 10 11
    v.frameRun();
    // Narrow: starts at colour clock 64 = pixel 64; mode E pixel = 2 pixels
    ASSERT_EQ(v.px(64, 8), Video::col(0x00));
    ASSERT_EQ(v.px(66, 8), Video::col(0x46));                 // PF0
    ASSERT_EQ(v.px(68, 8), Video::col(0x0E));                 // PF1
    ASSERT_EQ(v.px(70, 8), Video::col(0x94));                 // PF2
    ASSERT_EQ(v.px(63, 8), Video::col(0x00));
}

TEST_CASE(antic_hscrol_shifts_and_vscrol_skips_rows) {
    Video v;
    v.w(0xD400, 0x22);
    v.w(0xD404, 2);                                           // HSCROL 2 colour clocks
    v.w(0xD405, 3);                                           // VSCROL 3
    // mode F with HSCROL (fetches a wide line), mode 2 with VSCROL then without
    v.dl({0x5F, 0x00, 0x20, 0x62, 0x00, 0x30, 0x42, 0x00, 0x30, 0x41, 0x00, 0x10});
    v.bus.m_mem[0x2000 + 6] = 0x80;
    v.frameRun();
    uint32_t lit = Video::col((0x94 & 0xF0) | 0x0E);
    // A wide fetch starts at pixel 0, so byte 6 at pixel 48; HSCROL 2 colour
    // clocks moves it 4 pixels on. (Shown in the normal window, 32-351.)
    ASSERT_EQ(v.px(48 + 4, 8), lit);
    ASSERT_EQ(v.px(48 + 3, 8), Video::col(0x94));
    // The VSCROL line starts at row 3: 5 scan lines (9-13); the next mode 2
    // line ends at row 3: 4 scan lines (14-17); then blank
    v.bus.m_mem[0x3000] = 0x01;
    for (int r = 0; r < 8; ++r) v.bus.m_mem[0xE008 + r] = (uint8_t)(0x80 >> r);   // a diagonal
    v.frameRun();
    ASSERT_EQ(v.px(32 + 3, 9), lit);                          // row 3 on scan line 9
    ASSERT_EQ(v.px(32 + 7, 13), lit);                         // row 7 on scan line 13
    ASSERT_EQ(v.px(32 + 0, 14), lit);                         // next line: row 0
    ASSERT_EQ(v.px(32 + 3, 17), lit);                         // ...to row 3
    ASSERT_EQ(v.px(32 + 4, 18), Video::col(0x00));            // then JVB
}

TEST_CASE(antic_player_dma_and_priority) {
    Video v;
    v.w(0xD400, 0x2E);                                        // DL, normal, players + missiles, double line
    v.w(0xD407, 0x40);                                        // PMBASE $4000
    v.g(GTIA::GRACTL, 0x03);
    v.g(GTIA::HPOSP0, 48 + 4);                                // 4 colour clocks into the playfield
    v.g(GTIA::COLPM0, 0x36);
    // A full mode E line of PF0 at scan lines 8-9
    v.dl({0x4E, 0x00, 0x20, 0x0E, 0x41, 0x00, 0x10});
    for (int i = 0; i < 80; ++i) v.bus.m_mem[0x2000 + i] = 0x55;
    for (int line = 0; line < 262; ++line) v.bus.m_mem[0x4200 + line / 2] = 0x80;   // player 0, 1 bit
    v.frameRun();
    // Colour clock 52 = pixel (52 - 32) * 2 = 40
    ASSERT_EQ(v.px(40, 8), Video::col(0x36));                 // player over PF0 (PRIOR 0 = players first)
    ASSERT_EQ(v.px(41, 8), Video::col(0x36));
    ASSERT_EQ(v.px(42, 8), Video::col(0x46));
    uint8_t p0pf = 0;
    v.gtia.ioRead(nullptr, 0xD004, &p0pf);
    ASSERT_EQ((int)p0pf, 0x01);                               // collided with PF0
    v.g(GTIA::PRIOR, 0x04);                                   // playfield over players
    v.frameRun();
    ASSERT_EQ(v.px(40, 8), Video::col(0x46));
    ASSERT_EQ(v.px(40, 20), Video::col(0x36));                // player alone below the playfield
}

TEST_CASE(antic_dma_takes_cpu_cycles) {
    Video v;
    v.w(0xD400, 0x22);
    v.dl({0x42, 0x00, 0x20, 0x41, 0x00, 0x10});
    v.antic.tick((uint64_t)114 * 8 + 1);                      // into line 8 (the mode line)
    ASSERT(v.antic.cpuBlocked());
    int stolen = 0;
    while (v.antic.cpuBlocked() && stolen < 200) { v.antic.consumeBlockedCycle(); v.antic.tick(1); ++stolen; }
    // refresh 9 + instruction 3 + 40 names + 40 glyph bytes
    ASSERT_EQ(stolen, 9 + 3 + 40 + 40);
}
