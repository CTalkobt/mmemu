#include "test_harness.h"
#include "plugins/devices/vic2/main/vic2.h"
#include "libmem/main/memory_bus.h"
#include "libdevices/main/isignal_line.h"
#include <cstring>
#include <vector>

namespace {

class MockIrqLine : public ISignalLine {
public:
    bool get() const override { return m_level; }
    void set(bool v) override { m_level = v; m_setCount++; }
    void pulse() override { set(!m_level); set(!m_level); }
    bool m_level = false;
    int  m_setCount = 0;
};

struct VicFixture {
    FlatMemoryBus bus{"system", 16};
    std::vector<uint8_t> charRom = std::vector<uint8_t>(4096, 0);
    std::vector<uint8_t> colorRam = std::vector<uint8_t>(1024, 0);
    MockIrqLine irq;
    VIC2 vic;

    VicFixture() {
        vic.setDmaBus(&bus);
        vic.setCharRom(charRom.data(), charRom.size());
        vic.setColorRam(colorRam.data());
        vic.setIrqLine(&irq);
        vic.reset();
    }
};

} // namespace

// --- Register read/write ---

TEST_CASE(vic2_default_colors) {
    VicFixture f;
    uint8_t val;
    f.vic.ioRead(nullptr, 0xD020, &val);
    ASSERT_EQ(val & 0x0F, (uint8_t)14); // light blue border
    f.vic.ioRead(nullptr, 0xD021, &val);
    ASSERT_EQ(val & 0x0F, (uint8_t)6);  // blue background
}

TEST_CASE(vic2_color_reg_high_bits) {
    VicFixture f;
    // Color registers: writing 0x01, reading should return 0xF1 (high nibble is 1)
    f.vic.ioWrite(nullptr, 0xD020, 0x01);
    uint8_t val;
    f.vic.ioRead(nullptr, 0xD020, &val);
    ASSERT_EQ(val, (uint8_t)0xF1);
}

TEST_CASE(vic2_unused_regs_read_ff) {
    VicFixture f;
    uint8_t val = 0;
    f.vic.ioRead(nullptr, 0xD02F, &val);
    ASSERT_EQ(val, (uint8_t)0xFF);
    f.vic.ioRead(nullptr, 0xD03F, &val);
    ASSERT_EQ(val, (uint8_t)0xFF);
}

TEST_CASE(vic2_sprite_position) {
    VicFixture f;
    // Set sprite 0 position
    f.vic.ioWrite(nullptr, 0xD000, 100);  // SP0X lo
    f.vic.ioWrite(nullptr, 0xD001, 50);   // SP0Y
    f.vic.ioWrite(nullptr, 0xD010, 0x01); // MSIGX bit 0 = sprite 0 X MSB

    uint8_t xlo, y, msb;
    f.vic.ioRead(nullptr, 0xD000, &xlo);
    f.vic.ioRead(nullptr, 0xD001, &y);
    f.vic.ioRead(nullptr, 0xD010, &msb);
    ASSERT_EQ(xlo, (uint8_t)100);
    ASSERT_EQ(y, (uint8_t)50);
    ASSERT_EQ(msb & 0x01, (uint8_t)1);
}

TEST_CASE(vic2_irq_write_clears) {
    VicFixture f;
    // Enable raster IRQ
    f.vic.ioWrite(nullptr, 0xD01A, 0x01); // IRQEN = raster

    // Simulate raster match by ticking
    f.vic.ioWrite(nullptr, 0xD012, 1);     // RASTER compare = line 1
    f.vic.tick(VIC2::PAL_CYCLES_PER_LINE);     // Advance to line 1

    uint8_t irqStatus;
    f.vic.ioRead(nullptr, 0xD019, &irqStatus);
    ASSERT(irqStatus & 0x01); // RST bit set
    ASSERT(irqStatus & 0x80); // ANY bit set
    ASSERT(f.irq.m_level);    // IRQ line asserted

    // Clear by writing 1 to bit 0
    f.vic.ioWrite(nullptr, 0xD019, 0x01);
    f.vic.ioRead(nullptr, 0xD019, &irqStatus);
    ASSERT(!(irqStatus & 0x01));
    ASSERT(!f.irq.m_level);
}

TEST_CASE(vic2_collision_regs_clear_on_read) {
    VicFixture f;
    uint8_t val;

    // Read SSCOL — should be 0 and cleared
    f.vic.ioRead(nullptr, 0xD01E, &val);
    ASSERT_EQ(val, (uint8_t)0);

    // Read SBCOL
    f.vic.ioRead(nullptr, 0xD01F, &val);
    ASSERT_EQ(val, (uint8_t)0);

    // Writes to collision regs should be ignored
    f.vic.ioWrite(nullptr, 0xD01E, 0xFF);
    f.vic.ioWrite(nullptr, 0xD01F, 0xFF);
    f.vic.ioRead(nullptr, 0xD01E, &val);
    ASSERT_EQ(val, (uint8_t)0);
}

// --- Raster and timing ---

TEST_CASE(vic2_raster_counter) {
    VicFixture f;

    uint8_t lo;
    f.vic.ioRead(nullptr, 0xD012, &lo);
    ASSERT_EQ(lo, (uint8_t)0); // starts at 0

    // Tick one scanline
    f.vic.tick(VIC2::PAL_CYCLES_PER_LINE);
    f.vic.ioRead(nullptr, 0xD012, &lo);
    ASSERT_EQ(lo, (uint8_t)1);

    // Tick to line 255
    for (int i = 1; i < 255; i++) f.vic.tick(VIC2::PAL_CYCLES_PER_LINE);
    f.vic.ioRead(nullptr, 0xD012, &lo);
    ASSERT_EQ(lo, (uint8_t)255);

    // Tick to line 256 — raster bit 8 in SCR1
    f.vic.tick(VIC2::PAL_CYCLES_PER_LINE);
    f.vic.ioRead(nullptr, 0xD012, &lo);
    ASSERT_EQ(lo, (uint8_t)0); // low 8 bits wrap to 0
    uint8_t scr1;
    f.vic.ioRead(nullptr, 0xD011, &scr1);
    ASSERT(scr1 & 0x80); // bit 8 of raster in SCR1 bit 7
}

TEST_CASE(vic2_raster_wraps) {
    VicFixture f;
    // Tick through a full frame
    for (int i = 0; i < VIC2::PAL_LINES_PER_FRAME; i++)
        f.vic.tick(VIC2::PAL_CYCLES_PER_LINE);

    uint8_t lo;
    f.vic.ioRead(nullptr, 0xD012, &lo);
    ASSERT_EQ(lo, (uint8_t)0); // wrapped back to 0
}

TEST_CASE(vic2_raster_irq) {
    VicFixture f;
    f.vic.ioWrite(nullptr, 0xD01A, 0x01); // enable raster IRQ
    f.vic.ioWrite(nullptr, 0xD012, 10);    // compare at line 10

    // Tick to line 9 — no IRQ
    for (int i = 0; i < 10; i++) f.vic.tick(VIC2::PAL_CYCLES_PER_LINE);
    ASSERT(f.irq.m_level); // line 10 reached, IRQ fires
}

TEST_CASE(vic2_raster_irq_disabled) {
    VicFixture f;
    f.vic.ioWrite(nullptr, 0xD01A, 0x00); // IRQ disabled
    f.vic.ioWrite(nullptr, 0xD012, 1);

    f.vic.tick(VIC2::PAL_CYCLES_PER_LINE);
    ASSERT(!f.irq.m_level); // no IRQ even though raster matches
}

// --- Rendering ---

TEST_CASE(vic2_render_border_only) {
    VicFixture f;
    f.vic.ioWrite(nullptr, 0xD020, 0x01); // white border

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    // Top-left corner is border
    ASSERT_EQ(buf[0], (uint32_t)0xFFFFFFFF); // white
}

TEST_CASE(vic2_render_standard_text) {
    VicFixture f;
    // Set char ROM: character 'A' (code 1) has bit pattern 0xFF at row 0
    f.charRom[1 * 8 + 0] = 0xFF;
    // Write screen code 1 at position (0,0)
    uint32_t scrBase = 0x0400; // default screen base
    f.bus.write8(scrBase, 1);
    // Set color RAM: foreground = white (1)
    f.colorRam[0] = 0x01;

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    // First pixel of display area should be white (foreground)
    int px = VIC2::DISPLAY_X;
    int py = VIC2::DISPLAY_Y;
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px], (uint32_t)0xFFFFFFFF);
}

TEST_CASE(vic2_render_sprite) {
    VicFixture f;
    // Enable sprite 0
    f.vic.ioWrite(nullptr, 0xD015, 0x01);
    // Position at (50, 50)
    f.vic.ioWrite(nullptr, 0xD000, 50);
    f.vic.ioWrite(nullptr, 0xD001, 50);
    // Color = white (1)
    f.vic.ioWrite(nullptr, 0xD027, 0x01);

    // Sprite pointer: screen base $0400, pointer at $07F8
    // Default VMEM=$14 → screen at $0400, sprite pointers at $07F8
    f.bus.write8(0x07F8, 0x0D); // pointer = $0D → data at $0D * 64 = $0340

    // Write solid first row of sprite data
    f.bus.write8(0x0340, 0xFF);
    f.bus.write8(0x0341, 0xFF);
    f.bus.write8(0x0342, 0xFF);

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    // Pixel at (50, 50) should be white
    ASSERT_EQ(buf[50 * VIC2::FRAME_W + 50], (uint32_t)0xFFFFFFFF);
}

TEST_CASE(vic2_bank_select) {
    VicFixture f;
    f.vic.setBankBase(0x4000);

    // DMA read should come from bank 1
    f.bus.write8(0x4400, 0xAA);
    ASSERT_EQ(f.vic.dmaPeek(0x0400), (uint8_t)0xAA);
}

TEST_CASE(vic2_char_rom_shadow) {
    VicFixture f;
    // In bank 0, offsets $1000-$1FFF should read from char ROM
    f.charRom[0] = 0x42;
    ASSERT_EQ(f.vic.dmaPeek(0x1000), (uint8_t)0x42);

    // In bank 1, char ROM is NOT visible
    f.vic.setBankBase(0x4000);
    f.bus.write8(0x5000, 0xBB);
    ASSERT_EQ(f.vic.dmaPeek(0x1000), (uint8_t)0xBB);
}

TEST_CASE(vic2_memory_pointers) {
    VicFixture f;
    // Default VMEM=$14 → screen=$0400, char=$1000
    f.vic.ioWrite(nullptr, 0xD018, 0x14);
    // Screen: (0x14 >> 4) * $400 = 1 * $400 = $0400 ✓
    // Char:   (0x14 >> 1 & 7) * $800 = 2 * $800 = $1000 ✓

    // Change to screen=$0800, char=$2000
    f.vic.ioWrite(nullptr, 0xD018, 0x24);
    // (0x24 >> 4) * $400 = 2 * $400 = $0800
    // (0x24 >> 1 & 7) * $800 = 2 * $800 = $1000
    // That's actually char=$1000. For char=$2000: (4 << 1) = 0x08 → VMEM = 0x28
    f.vic.ioWrite(nullptr, 0xD018, 0x28);

    uint8_t vmem;
    f.vic.ioRead(nullptr, 0xD018, &vmem);
    ASSERT_EQ(vmem, (uint8_t)0x28);
}

TEST_CASE(vic2_device_info) {
    VicFixture f;
    f.vic.ioWrite(nullptr, 0xD015, 0x01); // enable sprite 0

    DeviceInfo info;
    f.vic.getDeviceInfo(info);

    ASSERT(info.name == "VIC-II");
    ASSERT_EQ(info.baseAddr, (uint32_t)0xD000);
    ASSERT(!info.registers.empty());
    ASSERT(!info.state.empty());
    ASSERT(!info.bitmaps.empty()); // sprite bitmaps

    // Check sprite 0 is reported as enabled
    bool found = false;
    for (auto& kv : info.state) {
        if (kv.first == "Sprite 0 Enabled" && kv.second == "yes") found = true;
    }
    ASSERT(found);
}

TEST_CASE(vic2_reset_clears_irq) {
    VicFixture f;
    f.vic.ioWrite(nullptr, 0xD01A, 0x01);
    f.vic.ioWrite(nullptr, 0xD012, 1);
    f.vic.tick(VIC2::PAL_CYCLES_PER_LINE);
    ASSERT(f.irq.m_level);

    f.vic.reset();
    ASSERT(!f.irq.m_level);

    uint8_t val;
    f.vic.ioRead(nullptr, 0xD019, &val);
    ASSERT_EQ(val & 0x0F, (uint8_t)0);
}

TEST_CASE(vic2_dimensions) {
    VIC2 vic;
    auto dims = vic.getDimensions();
    ASSERT_EQ(dims.width, VIC2::FRAME_W);
    ASSERT_EQ(dims.height, VIC2::FRAME_H);
    ASSERT_EQ(dims.displayWidth, VIC2::DISPLAY_W);
    ASSERT_EQ(dims.displayHeight, VIC2::DISPLAY_H);
}

TEST_CASE(vic2_name_and_base) {
    VIC2 vic;
    vic.setName("CustomVIC");
    ASSERT(std::string(vic.name()) == "CustomVIC");
    vic.setBaseAddr(0xD400);
    ASSERT_EQ(vic.baseAddr(), (uint32_t)0xD400);
    ASSERT_EQ(vic.addrMask(), (uint32_t)0x003F);
}

// --- Bitmap modes ---

TEST_CASE(vic2_render_standard_bitmap) {
    VicFixture f;

    // Enable bitmap mode: SCR1 BMM=1, DEN=1, RSEL=1
    f.vic.ioWrite(nullptr, 0xD011, VIC2::SCR1_BMM | VIC2::SCR1_DEN | VIC2::SCR1_RSEL | 0x03);
    // MCM=0 (standard bitmap)
    f.vic.ioWrite(nullptr, 0xD016, VIC2::SCR2_CSEL);

    // VMEM: screen at $0400, bitmap at $2000
    // screen = (V >> 4) * $400; bitmap = (V >> 1 & 7) * $800
    // For screen=$0400: V>>4 = 1; For bitmap=$2000: (V>>1&7) = 4 → V bit1-3 = 0b100 → 0x08
    // V = 0x18
    f.vic.ioWrite(nullptr, 0xD018, 0x18);

    // Screen RAM at $0400 holds color info: high nibble=fg, low nibble=bg
    // Cell (0,0): fg=white(1), bg=black(0)
    f.bus.write8(0x0400, 0x10);

    // Bitmap data at $2000: first row of cell (0,0) = all fg pixels
    f.bus.write8(0x2000, 0xFF);

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    // First pixel of display area should be white (fg)
    int px = VIC2::DISPLAY_X;
    int py = VIC2::DISPLAY_Y;
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px], (uint32_t)0xFFFFFFFF);

    // Second row of cell (0,0) = all bg pixels (0x00)
    f.bus.write8(0x2001, 0x00);
    f.vic.renderFrame(buf);
    // Row 1 of cell should be black (bg)
    ASSERT_EQ(buf[(py + 1) * VIC2::FRAME_W + px], (uint32_t)0xFF000000);
}

TEST_CASE(vic2_render_multicolor_bitmap) {
    VicFixture f;

    // BMM=1, MCM=1
    f.vic.ioWrite(nullptr, 0xD011, VIC2::SCR1_BMM | VIC2::SCR1_DEN | VIC2::SCR1_RSEL | 0x03);
    f.vic.ioWrite(nullptr, 0xD016, VIC2::SCR2_CSEL | VIC2::SCR2_MCM);

    // VMEM: screen=$0400, bitmap=$2000
    f.vic.ioWrite(nullptr, 0xD018, 0x18);

    // BGCOL0 = black (0)
    f.vic.ioWrite(nullptr, 0xD021, 0x00);

    // Screen RAM cell (0,0): high=fg1 (1=white), low=fg2 (2=red)
    f.bus.write8(0x0400, 0x12);

    // Color RAM cell (0,0): fg3 = cyan (3)
    f.colorRam[0] = 0x03;

    // Bitmap row 0: bit pairs select colors: 00=bg, 01=hi nibble, 10=lo nibble, 11=colorRam
    // Byte: 0b01_10_11_00 = 0x6C → pixels: white, red, cyan, black
    f.bus.write8(0x2000, 0x6C);

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    int px = VIC2::DISPLAY_X;
    int py = VIC2::DISPLAY_Y;

    // MC bitmap: 2 bits per pixel, each pixel is 2 screen pixels wide
    // Pair 0 (bits 7-6 = 01): white at px+0, px+1
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px], (uint32_t)0xFFFFFFFF);     // white
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px + 1], (uint32_t)0xFFFFFFFF);
    // Pair 1 (bits 5-4 = 10): red at px+2, px+3
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px + 2], (uint32_t)0xFF68372B); // red
    // Pair 2 (bits 3-2 = 11): cyan at px+4, px+5
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px + 4], (uint32_t)0xFF70A4B2); // cyan
    // Pair 3 (bits 1-0 = 00): black at px+6, px+7
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px + 6], (uint32_t)0xFF000000); // black
}

// --- Extended background color text ---

TEST_CASE(vic2_render_ecm_text) {
    VicFixture f;

    // ECM=1, BMM=0
    f.vic.ioWrite(nullptr, 0xD011, VIC2::SCR1_ECM | VIC2::SCR1_DEN | VIC2::SCR1_RSEL | 0x03);
    f.vic.ioWrite(nullptr, 0xD016, VIC2::SCR2_CSEL);

    // Set 4 background colors
    f.vic.ioWrite(nullptr, 0xD021, 0x00); // BGCOL0 = black
    f.vic.ioWrite(nullptr, 0xD022, 0x01); // BGCOL1 = white
    f.vic.ioWrite(nullptr, 0xD023, 0x02); // BGCOL2 = red
    f.vic.ioWrite(nullptr, 0xD024, 0x03); // BGCOL3 = cyan

    // Default VMEM: screen=$0400, char=$1000 (char ROM)
    f.vic.ioWrite(nullptr, 0xD018, 0x14);

    // Screen code: top 2 bits select BG color, low 6 bits = char code
    // 0xC1 = bg_sel=3 (cyan), char_code=1
    f.bus.write8(0x0400, 0xC1);

    // Char ROM: char 1, row 0 = 0x00 (all background pixels)
    f.charRom[1 * 8 + 0] = 0x00;

    // Color RAM: fg = white (1)
    f.colorRam[0] = 0x01;

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    int px = VIC2::DISPLAY_X;
    int py = VIC2::DISPLAY_Y;
    // All bg pixels → should be BGCOL3 (cyan, index 3)
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px], (uint32_t)0xFF70A4B2); // cyan
}

// --- Multicolor text mode ---

TEST_CASE(vic2_render_multicolor_text) {
    VicFixture f;

    // MCM=1, BMM=0, ECM=0
    f.vic.ioWrite(nullptr, 0xD011, VIC2::SCR1_DEN | VIC2::SCR1_RSEL | 0x03);
    f.vic.ioWrite(nullptr, 0xD016, VIC2::SCR2_CSEL | VIC2::SCR2_MCM);

    // Default VMEM: screen=$0400, char=$1000 (char ROM)
    f.vic.ioWrite(nullptr, 0xD018, 0x14);

    f.vic.ioWrite(nullptr, 0xD021, 0x00); // BGCOL0 = black
    f.vic.ioWrite(nullptr, 0xD022, 0x01); // BGCOL1 = white
    f.vic.ioWrite(nullptr, 0xD023, 0x02); // BGCOL2 = red

    // Screen code = char 1 at cell (0,0)
    f.bus.write8(0x0400, 1);

    // Color RAM nibble with bit 3 set → enables MC for this cell
    // Low 3 bits = color index for pair 11
    f.colorRam[0] = 0x0B; // bit3=1, color=3 (cyan)

    // Char ROM: char 1, row 0 = 0b01_10_11_00 = 0x6C
    // Pair 0=01 → BGCOL1(white), Pair 1=10 → BGCOL2(red), Pair 2=11 → colorRam(cyan), Pair 3=00 → BGCOL0(black)
    f.charRom[1 * 8 + 0] = 0x6C;

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    int px = VIC2::DISPLAY_X;
    int py = VIC2::DISPLAY_Y;

    // MC text: 2 bits → double-wide pixels
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px], (uint32_t)0xFFFFFFFF);     // white (pair 01)
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px + 1], (uint32_t)0xFFFFFFFF);
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px + 2], (uint32_t)0xFF68372B); // red (pair 10)
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px + 4], (uint32_t)0xFF70A4B2); // cyan (pair 11)
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px + 6], (uint32_t)0xFF000000); // black (pair 00)
}

// --- DMA-based character fetch (non-char-ROM address) ---

TEST_CASE(vic2_render_text_dma_chars) {
    VicFixture f;

    // Standard text, but place char data in RAM instead of char ROM shadow
    // Bank 1 ($4000) — char ROM is NOT visible here
    f.vic.setBankBase(0x4000);

    // VMEM: screen at offset $0400 → $4400, char at offset $0000 → $4000
    // V = (1 << 4) | (0 << 1) = 0x10
    f.vic.ioWrite(nullptr, 0xD018, 0x10);
    f.vic.ioWrite(nullptr, 0xD011, VIC2::SCR1_DEN | VIC2::SCR1_RSEL | 0x03);
    f.vic.ioWrite(nullptr, 0xD016, VIC2::SCR2_CSEL);

    // Screen RAM at $4400: char code 2
    f.bus.write8(0x4400, 2);

    // Character data in RAM at $4000 + 2*8 = $4010
    f.bus.write8(0x4010, 0xFF); // row 0: all foreground

    // Color RAM: fg = white (1)
    f.colorRam[0] = 0x01;

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    int px = VIC2::DISPLAY_X;
    int py = VIC2::DISPLAY_Y;
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px], (uint32_t)0xFFFFFFFF); // white fg from RAM chars
}

// --- Sprite Y expansion row duplication ---

TEST_CASE(vic2_sprite_y_expansion) {
    VicFixture f;

    // Enable sprite 0, Y-expanded
    f.vic.ioWrite(nullptr, 0xD015, 0x01); // SPENA
    f.vic.ioWrite(nullptr, 0xD017, 0x01); // YXPAND sprite 0
    f.vic.ioWrite(nullptr, 0xD000, 50);   // SP0X
    f.vic.ioWrite(nullptr, 0xD001, 50);   // SP0Y
    f.vic.ioWrite(nullptr, 0xD027, 0x01); // SP0COL = white

    // Sprite pointer at default screen base + $3F8
    f.bus.write8(0x07F8, 0x0D); // data at $0340

    // First row of sprite data: all pixels set
    f.bus.write8(0x0340, 0xFF);
    f.bus.write8(0x0341, 0xFF);
    f.bus.write8(0x0342, 0xFF);

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    // Y-expanded: row 0 of sprite data appears at y=50 AND y=51
    ASSERT_EQ(buf[50 * VIC2::FRAME_W + 50], (uint32_t)0xFFFFFFFF);
    ASSERT_EQ(buf[51 * VIC2::FRAME_W + 50], (uint32_t)0xFFFFFFFF); // duplicated row

    // Row 2 (next sprite data row at y=52,53) should NOT be white (data is 0)
    ASSERT_NE(buf[52 * VIC2::FRAME_W + 50], (uint32_t)0xFFFFFFFF);
}

// --- Logger callback ---

TEST_CASE(vic2_log_callback) {
    VicFixture f;

    int logCount = 0;
    f.vic.setLogger(&logCount, [](void* ctx, int level, const char* msg) {
        (*(int*)ctx)++;
    });

    f.vic.log(0, "test message");
    ASSERT_EQ(logCount, 1);
}
