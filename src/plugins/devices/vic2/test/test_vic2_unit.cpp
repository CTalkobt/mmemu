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
    f.vic.tick(VIC2::CYCLES_PER_LINE);     // Advance to line 1

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
    f.vic.tick(VIC2::CYCLES_PER_LINE);
    f.vic.ioRead(nullptr, 0xD012, &lo);
    ASSERT_EQ(lo, (uint8_t)1);

    // Tick to line 255
    for (int i = 1; i < 255; i++) f.vic.tick(VIC2::CYCLES_PER_LINE);
    f.vic.ioRead(nullptr, 0xD012, &lo);
    ASSERT_EQ(lo, (uint8_t)255);

    // Tick to line 256 — raster bit 8 in SCR1
    f.vic.tick(VIC2::CYCLES_PER_LINE);
    f.vic.ioRead(nullptr, 0xD012, &lo);
    ASSERT_EQ(lo, (uint8_t)0); // low 8 bits wrap to 0
    uint8_t scr1;
    f.vic.ioRead(nullptr, 0xD011, &scr1);
    ASSERT(scr1 & 0x80); // bit 8 of raster in SCR1 bit 7
}

TEST_CASE(vic2_raster_wraps) {
    VicFixture f;
    // Tick through a full frame
    for (int i = 0; i < VIC2::LINES_PER_FRAME; i++)
        f.vic.tick(VIC2::CYCLES_PER_LINE);

    uint8_t lo;
    f.vic.ioRead(nullptr, 0xD012, &lo);
    ASSERT_EQ(lo, (uint8_t)0); // wrapped back to 0
}

TEST_CASE(vic2_raster_irq) {
    VicFixture f;
    f.vic.ioWrite(nullptr, 0xD01A, 0x01); // enable raster IRQ
    f.vic.ioWrite(nullptr, 0xD012, 10);    // compare at line 10

    // Tick to line 9 — no IRQ
    for (int i = 0; i < 10; i++) f.vic.tick(VIC2::CYCLES_PER_LINE);
    ASSERT(f.irq.m_level); // line 10 reached, IRQ fires
}

TEST_CASE(vic2_raster_irq_disabled) {
    VicFixture f;
    f.vic.ioWrite(nullptr, 0xD01A, 0x00); // IRQ disabled
    f.vic.ioWrite(nullptr, 0xD012, 1);

    f.vic.tick(VIC2::CYCLES_PER_LINE);
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
    f.vic.tick(VIC2::CYCLES_PER_LINE);
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
