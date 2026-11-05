#include "test_harness.h"
#include "plugins/machines/x16/main/machine_x16.h"
#include "plugins/devices/vera/main/vera.h"
#include "plugins/6502/main/cpu65c02.h"
#include "libcore/main/machine_desc.h"
#include "libdebug/main/gfx_view.h"
#include "libdevices/main/ivideo_inspect.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libmem/main/memory_bus.h"
#include <cstdio>
#include <string>
#include <vector>

namespace {

Vera* findVera(MachineDescriptor* m) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if (auto* v = dynamic_cast<Vera*>(h)) return v;
    return nullptr;
}

// The text screen: layer 1's map at $1B000, two bytes (screen code, colour) per cell
bool screenHas(Vera* vera, const std::vector<uint8_t>& codes) {
    for (uint32_t a = 0x1B000; a < 0x1F000; a += 2) {
        bool m = true;
        for (size_t k = 0; k < codes.size() && m; k++) m = vera->vramPeek(a + 2 * (uint32_t)k) == codes[k];
        if (m) return true;
    }
    return false;
}

const std::vector<uint8_t> READY = { 0x12, 0x05, 0x01, 0x04, 0x19, 0x2E };   // "READY."

} // namespace

// R49 boots to BASIC's READY. and runs a typed line. Skipped without
// roms/x16/rom.bin (not in the repository).
TEST_CASE(x16_boots_to_ready_and_runs_basic) {
    std::string err;
    MachineDescriptor* m = x16::create(&err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    Vera* vera = findVera(m);
    ASSERT(vera != nullptr);
    auto run = [&](long steps) { for (long s = 0; s < steps; s++) m->schedulerStep(*m); };
    run(16000000);
    ASSERT(screenHas(vera, READY));

    IKeyboardMatrix* kbd = nullptr;
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if ((kbd = dynamic_cast<IKeyboardMatrix*>(h))) break;
    ASSERT(kbd != nullptr);
    kbd->enqueueText("PRINT 6*7\n");
    run(8000000);
    ASSERT(screenHas(vera, { 0x20, 0x34, 0x32 }));                              // " 42"
    delete m;
}

// SNES pad 1 through the KERNAL (JOY) and the mouse (MOUSE, MX/MY/MB)
TEST_CASE(x16_basic_reads_gamepad_and_mouse) {
    std::string err;
    MachineDescriptor* m = x16::create(&err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    Vera* vera = findVera(m);
    auto run = [&](long steps) { for (long s = 0; s < steps; s++) m->schedulerStep(*m); };
    IKeyboardMatrix* kbd = nullptr;
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if ((kbd = dynamic_cast<IKeyboardMatrix*>(h))) break;
    run(16000000);

    ASSERT(m->onGamepad(0, MachineDescriptor::PAD_B | MachineDescriptor::PAD_UP | MachineDescriptor::PAD_START));
    ASSERT(!m->onGamepad(4, 0));
    kbd->enqueueText("PRINT JOY(1);JOY(2)\n");
    run(16000000);
    ASSERT(screenHas(vera, { 0x20, 0x31, 0x35, 0x32, 0x20, 0x2D, 0x31 }));      // " 152 -1"

    kbd->enqueueText("MOUSE 1\n");
    run(8000000);
    m->onMouse(100, 50, 2, 0);                                                  // right button
    run(1000000);
    kbd->enqueueText("PRINT MX;MY;MB\n");
    run(16000000);
    ASSERT(screenHas(vera, { 0x20, 0x34, 0x31, 0x39, 0x20, 0x20, 0x32, 0x38, 0x39, 0x20, 0x20, 0x32 }));   // " 419  289  2"
    delete m;
}

// The 65C02's VPB pin: the hook runs before an IRQ fetches its vector
TEST_CASE(cpu65c02_vector_pull_hook_before_irq_vector) {
    FlatMemoryBus bus("system", 16);
    MOS65C02 cpu;
    cpu.setDataBus(&bus);
    cpu.setCodeBus(&bus);
    bus.write8(0xFFFE, 0x00);
    bus.write8(0xFFFF, 0x30);
    bus.write8(0x0200, 0xEA);
    int pulls = 0;
    cpu.setVectorPullHook([&]() { pulls++; bus.write8(0xFFFF, 0x40); });   // e.g. a bank switch
    cpu.reset();
    cpu.setPc(0x0200);
    cpu.regWrite(5, 0x20);                                                      // I clear
    cpu.setIrqLine(true);
    cpu.step();
    ASSERT_EQ(pulls, 1);
    ASSERT_EQ((int)cpu.pc(), 0x4000);                                           // vector read after the hook
}

// The X16's hardware panes: drives and the VERA graphics viewer (no VIC
// graphics, tape, cartridge or SID); only VERA formats
TEST_CASE(x16_ui_panes) {
    std::string err;
    MachineDescriptor* m = x16::create(&err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    ASSERT(m->uiPanesSet);
    ASSERT(m->hasPane("drive") && m->hasPane("graphics-vera"));
    ASSERT(!m->hasPane("graphics-vic") && !m->hasPane("tape") && !m->hasPane("cartridge") && !m->hasPane("sidscope"));
    auto fmts = gfx::machineFormats(m);
    ASSERT_EQ((int)fmts.size(), 16);
    for (const auto* f : fmts) ASSERT_EQ(std::string(f->family), std::string("vera"));
    delete m;
}

// VERA follows its 128 sprites and both tile layers (Graphics (VERA) pane,
// CLI gfx chip-sprite / chip-layer)
TEST_CASE(x16_vera_video_inspect) {
    std::string err;
    MachineDescriptor* m = x16::create(&err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    Vera* vera = findVera(m);
    for (long s = 0; s < 16000000; s++) m->schedulerStep(*m);
    ASSERT(screenHas(vera, READY));
    IVideoInspect* vi = gfx::findVideoInspect(m);
    ASSERT(vi == static_cast<IVideoInspect*>(vera));
    ASSERT_EQ(vi->spriteCount(), 128);
    ASSERT_EQ(vi->layerCount(), 2);

    // The KERNAL's text layer: layer 1, 1 bpp 8x8 tiles at $1F000, colours
    // from the map's most common attribute byte (white on blue: $61)
    IVideoInspect::CharsetInfo ci;
    ASSERT(vi->layerCharsetInfo(1, ci));
    ASSERT_EQ(ci.bitsPerPixel, 1);
    ASSERT_EQ((int)ci.addr, 0x1F000);
    ASSERT_EQ(ci.tileW, 8);
    ASSERT_EQ(ci.tileH, 8);
    ASSERT_EQ(ci.fg, 1);
    ASSERT_EQ((int)ci.bg[0], 6);

    // Sprite 3: data at $13000 (bits 12:5 and 16:13), 8 bpp, 32 wide, 16 tall,
    // palette offset 5, Z-depth 3
    const uint32_t a = 0x1FC00 + 3 * 8;
    vera->vramPoke(a + 0, (uint8_t)((0x13000 >> 5) & 0xFF));
    vera->vramPoke(a + 1, (uint8_t)(0x80 | ((0x13000 >> 13) & 0x0F)));
    vera->vramPoke(a + 6, 0x0C);
    vera->vramPoke(a + 7, (uint8_t)((1 << 6) | (2 << 4) | 5));
    IVideoInspect::SpriteInfo si;
    ASSERT(vi->spriteInfo(3, si));
    ASSERT(si.enabled);
    ASSERT_EQ((int)si.dataAddr, 0x13000);
    ASSERT_EQ(si.bitsPerPixel, 8);
    ASSERT_EQ(si.width, 32);
    ASSERT_EQ(si.height, 16);
    ASSERT_EQ(si.paletteBase, 80);
    ASSERT(!vi->spriteInfo(128, si));

    // The view reads VRAM (not the CPU bus), picks 32x16 at 8 bpp and offsets 1-15
    vera->vramPoke(0x13000, 2);                     // -> palette 82
    vera->vramPoke(0x13001, 0x40);                  // >= 16: unmodified
    gfx::ViewRequest req;
    req.source = gfx::Source::Sprite;
    req.sprite = 3;
    gfx::View v = gfx::buildView(req, m->buses[0].bus, vi);
    ASSERT(v.error.empty());
    ASSERT(v.format == gfx::Format::VeraSprite32x8);
    ASSERT_EQ(v.sheet.cellH, 16);
    auto px = [&](int x, int y) { return v.sheet.argb[(size_t)(v.sheet.gap + y) * v.sheet.width + v.sheet.gap + x]; };
    ASSERT_EQ(px(0, 0), vi->paletteColor(82));
    ASSERT_EQ(px(1, 0), vi->paletteColor(0x40));

    // Layer 1's tiles: the character set, 1 bpp, in the text colours
    req = gfx::ViewRequest{};
    req.source = gfx::Source::Charset;
    req.layer = 1;
    req.count = 256;
    v = gfx::buildView(req, m->buses[0].bus, vi);
    ASSERT(v.error.empty());
    ASSERT(v.format == gfx::Format::VeraTile8x1);
    ASSERT_EQ((int)v.start, 0x1F000);
    delete m;
}
