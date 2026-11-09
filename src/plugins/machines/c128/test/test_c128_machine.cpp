#include "test_harness.h"
#include "plugins/machines/c128/main/machine_c128.h"
#include "plugins/devices/vdc8563/main/vdc8563.h"
#include "plugins/devices/reu/main/reu.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libmem/main/ibus.h"
#include "libdevices/main/ivideo_output.h"
#include <cstdio>
#include <cstring>
#include <tuple>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <string>
#include <vector>

namespace {
// Screen codes of a text at the 40-column screen ($0400)
bool screenHas(MachineDescriptor* m, const std::string& text) {
    IBus* bus = m->buses[0].bus;
    std::vector<uint8_t> want;
    for (char c : text) want.push_back((uint8_t)(c >= 'A' && c <= 'Z' ? c - 'A' + 1 : c));
    for (uint32_t a = 0x0400; a + want.size() <= 0x0400 + 1000; ++a) {
        bool ok = true;
        for (size_t i = 0; i < want.size() && ok; ++i) ok = bus->peek8(a + (uint32_t)i) == want[i];
        if (ok) return true;
    }
    return false;
}
void run(MachineDescriptor* m, uint64_t ticks) {
    for (uint64_t t = 0; t < ticks;) t += (uint64_t)m->schedulerStep(*m);
}
MachineDescriptor* boot(std::string& err) {
    MachineDescriptor* m = c128::create(false, &err);
    if (!m) return nullptr;
    m->onReset(*m);
    run(m, 3000000);
    return m;
}
}  // namespace

// Boots to BASIC 7.0 READY on the 40-column screen; the editor set up the VDC
TEST_CASE(c128_boots_to_ready) {
    std::string err;
    MachineDescriptor* m = boot(err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    ASSERT(m->machineId == "c128");
    ASSERT(screenHas(m, "COMMODORE BASIC V7.0 122365 BYTES FREE"));
    ASSERT(screenHas(m, "READY."));
    auto* vdc = dynamic_cast<Vdc8563*>(m->ioRegistry->findHandler("VDC"));
    ASSERT(vdc != nullptr);
    EXPECT_EQ(vdc->reg(1), 80);                    // 80 characters a row
    delete m;
}

// Typed BASIC 7 with both RAM banks
TEST_CASE(c128_basic_banks) {
    std::string err;
    MachineDescriptor* m = boot(err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    auto* kbd = dynamic_cast<IKeyboardMatrix*>(m->ioRegistry->findHandler("Keyboard"));
    ASSERT(kbd != nullptr);
    kbd->enqueueText("BANK 1:POKE 4096,42:A=PEEK(4096):BANK 0:B=PEEK(4096):BANK 15:PRINT A*1000+B\n");
    run(m, 16000000);                              // ~160 ms a typed key
    ASSERT(screenHas(m, " 42007"));                // $1000 in bank 0 still holds 7
    delete m;
}

// The MMU: CR at $FF00 and $D500, the preconfiguration registers, the
// RAM banks under ROM, shared RAM and page 0 relocation (VICE c128mmu.c)
TEST_CASE(c128_mmu_banking) {
    std::string err;
    MachineDescriptor* m = c128::create(false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    m->onReset(*m);
    IBus* bus = m->buses[0].bus;
    // Reset: CR 0 = BASIC, KERNAL, I/O
    EXPECT_EQ(bus->peek8(0xFF00), 0x00);
    EXPECT_EQ(bus->peek8(0x4000), 0x4C);           // BASIC low (JMP)
    EXPECT_EQ(bus->peek8(0xD505) & 0xC1, 0x80);    // 40/80 up, the Z80 (it runs first)
    EXPECT_EQ(bus->peek8(0xD50B), 0x20);
    // All RAM, bank 0, then bank 1
    bus->write8(0xFF00, 0x3F);
    bus->write8(0x8000, 0x12);
    bus->write8(0xFF00, 0x7F);
    bus->write8(0x8000, 0x34);
    EXPECT_EQ(bus->peek8(0x8000), 0x34);
    bus->write8(0xFF00, 0x3F);
    EXPECT_EQ(bus->peek8(0x8000), 0x12);
    // PCR A, then a write to $FF01 loads it into CR
    bus->write8(0xFF00, 0x00);                     // I/O in to reach $D501
    bus->write8(0xD501, 0x7F);
    bus->write8(0xFF01, 0x00);
    EXPECT_EQ(bus->peek8(0xFF00), 0x7F);
    EXPECT_EQ(bus->peek8(0x8000), 0x34);
    // Shared RAM: 1 KB at the bottom (RCR bit 2): bank 1 sees bank 0's
    bus->write8(0xFF00, 0x3F);
    bus->write8(0x0300, 0x55);
    bus->write8(0xFF00, 0x00);
    bus->write8(0xD506, 0x04);
    bus->write8(0xFF00, 0x7F);
    EXPECT_EQ(bus->peek8(0x0300), 0x55);
    bus->write8(0x0300, 0x66);
    bus->write8(0xFF00, 0x3F);
    EXPECT_EQ(bus->peek8(0x0300), 0x66);
    // Page 0 at $2000 (P0H latched, committed by P0L): they swap
    bus->write8(0xFF00, 0x00);
    bus->write8(0xD506, 0x00);
    bus->write8(0xD508, 0x00);
    bus->write8(0xD507, 0x20);
    bus->write8(0xFF00, 0x3F);
    bus->write8(0x2080, 0x5A);                     // the old page 0 now answers at $2000
    bus->write8(0x0080, 0xA5);                     // lands at $2080
    EXPECT_EQ(bus->peek8(0x0080), 0xA5);
    EXPECT_EQ(bus->peek8(0x2080), 0x5A);
    bus->write8(0xFF00, 0x00);
    bus->write8(0xD507, 0x00);                     // page 0 back at $0000
    bus->write8(0xFF00, 0x3F);
    EXPECT_EQ(bus->peek8(0x2080), 0xA5);
    EXPECT_EQ(bus->peek8(0x0080), 0x5A);
    delete m;
}

// The VIC-IIe: $D030 (2 MHz) and the extra keyboard lines on $D02F; the
// character ROM the VIC sees follows 8502 port bit 2
TEST_CASE(c128_vic_iie) {
    std::string err;
    MachineDescriptor* m = boot(err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    IBus* bus = m->buses[0].bus;
    bus->write8(0xD030, 0x01);
    EXPECT_EQ(bus->peek8(0xD030), 0xFD);
    bus->write8(0xD030, 0x00);
    // HELP (K0, bit 0): select K0 only and read CIA1 port B
    m->onKey("help", true);
    bus->write8(0xDC00, 0xFF);
    bus->write8(0xD02F, 0x06);
    EXPECT_EQ(bus->peek8(0xDC01), 0xFE);
    bus->write8(0xD02F, 0x07);
    EXPECT_EQ(bus->peek8(0xDC01), 0xFF);
    m->onKey("help", false);
    delete m;
}

// With 40/80 down at reset the editor uses the VDC: the banner is in VDC
// RAM (screen at $0000) and the display shows the VDC's frame
TEST_CASE(c128_boots_80_columns) {
    std::string err;
    MachineDescriptor* m = c128::create(false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    m->onKey("40_80", true);
    m->onKey("40_80", false);
    m->onReset(*m);
    run(m, 3000000);
    auto* vdc = dynamic_cast<Vdc8563*>(m->ioRegistry->findHandler("VDC"));
    ASSERT(vdc != nullptr);
    const std::string want = "COMMODORE BASIC V7.0";
    bool found = false;
    for (uint16_t a = 0; a < 2000 && !found; ++a) {
        bool ok = true;
        for (size_t i = 0; i < want.size() && ok; ++i) {
            char c = want[i];
            ok = vdc->ramByte((uint16_t)(a + i)) == (uint8_t)(c >= 'A' && c <= 'Z' ? c - 'A' + 1 : c);
        }
        found = ok;
    }
    ASSERT(found);
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    IVideoOutput* screen = nullptr;
    for (auto* h : hs) if ((screen = dynamic_cast<IVideoOutput*>(h))) break;
    ASSERT(screen != nullptr && dynamic_cast<Vdc8563*>(screen) == nullptr);   // the display, not a chip
    std::vector<uint32_t> f((size_t)Vdc8563::FRAME_W * Vdc8563::FRAME_H);
    screen->renderFrame(f.data());
    EXPECT_EQ(f[0], Vdc8563::palette(0));                      // the VDC's black, not the VIC's border
    delete m;
}

// C64 mode: GO64 from BASIC, and C= held through a reset; a reset goes back
// to C128 mode
TEST_CASE(c128_c64_mode) {
    std::string err;
    MachineDescriptor* m = boot(err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    auto* kbd = dynamic_cast<IKeyboardMatrix*>(m->ioRegistry->findHandler("Keyboard"));
    kbd->enqueueText("GO64\nY\n");
    run(m, 5000000);
    ASSERT(screenHas(m, "**** COMMODORE 64 BASIC V2 ****"));
    ASSERT(screenHas(m, "38911 BASIC BYTES FREE"));
    kbd->enqueueText("PRINT 6*7;PEEK(53296)\n");               // ';' via the US C64 KERNAL's table
    run(m, 6000000);
    ASSERT(screenHas(m, " 42  252"));
    m->onReset(*m);
    run(m, 3000000);
    ASSERT(screenHas(m, "COMMODORE BASIC V7.0"));
    m->onKey("cbm", true);
    m->onReset(*m);
    run(m, 8000000);                                           // C128 reset, then the C64 KERNAL's RAM test
    m->onKey("cbm", false);
    ASSERT(screenHas(m, "**** COMMODORE 64 BASIC V2 ****"));
    delete m;
}

// The C64 map in C64 mode: $01 banks BASIC/KERNAL/I/O/character ROM, no MMU
// at $D500 or $FF00, the VDC still at $D600, the C64 character set
TEST_CASE(c128_c64_mode_memory) {
    std::string err;
    MachineDescriptor* m = c128::create(false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    m->onReset(*m);
    IBus* bus = m->buses[0].bus;
    bus->write8(0xD505, 0xF1);                                 // MCR: C64 mode
    EXPECT_EQ(bus->peek8(0xA000), 0x94);                       // BASIC 64 ($E394 cold start)
    EXPECT_EQ(bus->peek8(0xFFFC), 0xE2);                       // KERNAL 64 reset vector $FCE2
    EXPECT_EQ(bus->peek8(0xD505), 0xFF);                       // no MMU
    bus->write8(0xFF00, 0x3F);                                 // $FF00: just ROM/RAM
    EXPECT_EQ(bus->peek8(0xA000), 0x94);
    bus->write8(0xD600, 1);
    bus->write8(0xD601, 80);
    uint8_t v = bus->peek8(0xD601);
    EXPECT_EQ(v, 80);                                          // the VDC (columns) is still there
    IBus* cpuBus = m->cpus[0].cpu->getDataBus();               // the 8502 port is on the CPU's bus
    cpuBus->write8(0x0000, 0x2F);
    cpuBus->write8(0x0001, 0x33);                              // character ROM in
    EXPECT_EQ(bus->peek8(0xD008), 0x18);                       // C64 'A' (character 1), first line
    cpuBus->write8(0x0001, 0x34);                              // all RAM
    bus->write8(0xA000, 0x5A);
    EXPECT_EQ(bus->peek8(0xA000), 0x5A);
    cpuBus->write8(0x0001, 0x37);
    EXPECT_EQ(bus->peek8(0xA000), 0x94);
    delete m;
}

// The Z80's view (VICE z80mem.c): in bank 0 its $0000-$0FFF is the boot BIOS
// (KERNAL ROM $D000) and writes land at RAM $D000; $D000-$DFFF is RAM, not
// I/O; IN/OUT reach the chips; MCR bit 0 picks the CPU
TEST_CASE(c128_z80_view) {
    std::string err;
    MachineDescriptor* m = c128::create(false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    m->onReset(*m);
    ASSERT(m->cpus.size() == 2 && m->cpus[1].active);              // the Z80 runs first
    IBus* zmem = m->cpus[1].dataBus;
    IBus* zio = m->cpus[1].ioBus;
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(zmem->peek8(0x0000), 0x3E);                          // BIOS: LD A,$3E
    zmem->write8(0x0100, 0x77);
    bus->write8(0xFF00, 0x3F);                                     // 8502: all RAM, bank 0
    EXPECT_EQ(bus->peek8(0xD100), 0x77);
    bus->write8(0xD020, 0x12);                                     // RAM under I/O
    EXPECT_EQ(zmem->peek8(0xD020), 0x12);                          // Z80: RAM at $D000
    bus->write8(0xFF00, 0x00);
    zio->write8(0xD020, 0x05);                                     // OUT: the VIC-II
    EXPECT_EQ(bus->peek8(0xD020), 0xF5);
    EXPECT_EQ(zio->peek8(0xD505) & 0x01, 0x00);                    // MCR via IN: Z80
    zio->write8(0xD505, 0xB1);                                     // hand over to the 8502
    m->schedulerStep(*m);
    ASSERT(m->cpus[0].active && !m->cpus[1].active);
    delete m;
}

// Function ROMs (VICE functionrom.c, c128cart.c): a cartridge (external)
// with "CBM" at $8007 and $01 at $8006 is started by the KERNAL; CR picks
// internal (1) or external (2) per area; a 16 KB image is mirrored
TEST_CASE(c128_function_roms) {
    std::string err;
    MachineDescriptor* m = c128::create(false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    std::vector<uint8_t> rom(0x4000, 0xEA);
    const uint8_t code[] = {0x4C, 0x0A, 0x80, 0, 0, 0, 0x01, 'C', 'B', 'M',
                            0xA9, 0x5A, 0x8D, 0x00, 0x04, 0x4C, 0x0F, 0x80};   // LDA #$5A, STA $0400, JMP *
    std::copy(code, code + sizeof code, rom.begin());
    ASSERT(c128::insertCartridge(m, rom, "t.bin", err));
    EXPECT_EQ(c128::cartridgeInfo(m), std::string("t.bin (16 KB function ROM)"));
    run(m, 3000000);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0x0400), 0x5A);
    EXPECT_EQ(m->cpus[0].cpu->pc(), 0x800F);
    // CR: $8000 external (bits 2-3 = 2), $C000 internal (bits 4-5 = 1), I/O off
    std::vector<uint8_t> intRom(0x8000, 0x11);
    intRom[0x4000] = 0x22;                         // its $C000
    auto* sock = m->romSocket("function");           // the internal one, as the frontends fit it
    ASSERT(sock);
    EXPECT_EQ(sock->info(), std::string(""));
    ASSERT(sock->fit(intRom, "int.bin", err));
    EXPECT_EQ(sock->info(), std::string("int.bin (32 KB function ROM)"));
    bus->write8(0xFF00, 0x19);
    EXPECT_EQ(bus->peek8(0x8007), 'C');
    EXPECT_EQ(bus->peek8(0xC000), 0x22);           // $C000 is the internal ROM here
    bus->write8(0xFF00, 0x29);                     // $C000 external: the 16 KB mirror
    EXPECT_EQ(bus->peek8(0xC007), 'C');
    bus->write8(0xFF00, 0x15);                     // $8000 internal, $C000 internal
    EXPECT_EQ(bus->peek8(0x8000), 0x11);
    EXPECT_EQ(bus->peek8(0xC000), 0x22);
    c128::removeCartridge(m);
    bus->write8(0xFF00, 0x09);
    EXPECT_EQ(bus->peek8(0x8000), 0xFF);           // nothing fitted
    bus->write8(0xFF00, 0x15);
    EXPECT_EQ(bus->peek8(0x8000), 0x11);
    ASSERT(sock->fit({}, "", err));                // empty the socket
    EXPECT_EQ(sock->info(), std::string(""));
    EXPECT_EQ(bus->peek8(0x8000), 0xFF);
    ASSERT(!c128::insertCartridge(m, std::vector<uint8_t>(3000, 0), "x.bin", err));
    delete m;
}

// 1571 fast serial: the KERNAL finds the drive fast ($0A1C) and loads a
// file with the burst fastload
TEST_CASE(c128_fast_serial_load) {
    std::string err;
    MachineDescriptor* m = c128::create(false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    std::string dir = "/tmp/mmemu_c128_fast_" + std::to_string(getpid());
    std::filesystem::create_directories(dir);
    {
        std::vector<uint8_t> f = {0x00, 0x30};
        for (int i = 0; i < 1000; ++i) f.push_back((uint8_t)(i * 7 + 3));
        std::ofstream(dir + "/BIG", std::ios::binary).write((const char*)f.data(), (std::streamsize)f.size());
    }
    ASSERT(m->ioRegistry->findHandler("Drive8")->mountDisk(8, dir));
    m->onReset(*m);
    run(m, 3000000);
    auto* kbd = dynamic_cast<IKeyboardMatrix*>(m->ioRegistry->findHandler("Keyboard"));
    kbd->enqueueText("BLOAD\"BIG\",B0\n");
    run(m, 8000000);
    IBus* bus = m->buses[0].bus;
    bus->write8(0xFF00, 0x3F);                                     // bank 0 RAM
    EXPECT_EQ(bus->peek8(0x0A1C) & 0xC0, 0xC0);                    // a fast drive
    EXPECT_EQ(bus->peek8(0x3000), 0x03);
    EXPECT_EQ(bus->peek8(0x3000 + 999), (uint8_t)(999 * 7 + 3));
    std::filesystem::remove_all(dir);
    delete m;
}

// Badlines steal the bus from the 8502 at 1 MHz (25 a frame, ~40 cycles
// each); in FAST mode the VIC-IIe steals nothing (VICE) and the CPU does
// twice the work
TEST_CASE(c128_badlines_and_fast_mode) {
    std::string err;
    MachineDescriptor* m = boot(err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    IBus* bus = m->buses[0].bus;
    // $0B00: SEI; loop: INC $FB; BNE loop; INC $FC; JMP loop
    const uint8_t code[] = {0x78, 0xE6, 0xFB, 0xD0, 0xFC, 0xE6, 0xFC, 0x4C, 0x01, 0x0B};
    for (size_t i = 0; i < sizeof code; ++i) bus->write8(0x0B00 + (uint32_t)i, code[i]);
    m->cpus[0].cpu->setPc(0x0B00);
    run(m, 100);
    auto passes = [&](uint8_t d011, uint8_t d030) {
        bus->write8(0xD011, d011);
        bus->write8(0xD030, d030);
        run(m, 2 * 19656);                         // settle
        bus->write8(0xFB, 0); bus->write8(0xFC, 0);
        run(m, 10 * 19656);                        // 10 PAL frames
        return bus->peek8(0xFB) | (bus->peek8(0xFC) << 8);
    };
    const int on = passes(0x1B, 0xFC), off = passes(0x0B, 0xFC), fast = passes(0x1B, 0xFD);
    // A pass is 8 cycles: 10 frames of 25 badlines x 40 cycles = ~1250 fewer
    printf("  passes: display on %d, off %d, FAST %d\n", on, off, fast);
    EXPECT_TRUE(off - on > 1100 && off - on < 1500);
    EXPECT_NEAR(fast, 2 * off, 50);
    delete m;
}

namespace {
// A C128 .crt: hardware type `hw`, CHIP packets {bank, start, data}
std::vector<uint8_t> c128Crt(int hw, const std::vector<std::tuple<int, int, std::vector<uint8_t>>>& chips) {
    std::vector<uint8_t> f(0x40, 0);
    std::memcpy(f.data(), "C128 CARTRIDGE  ", 16);
    f[0x13] = 0x40; f[0x14] = 1; f[0x16] = 0; f[0x17] = (uint8_t)hw;
    for (auto& [bank, start, data] : chips) {
        const uint32_t len = 16 + (uint32_t)data.size();
        const uint8_t h[16] = {'C', 'H', 'I', 'P', (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len,
                               0, 0, (uint8_t)(bank >> 8), (uint8_t)bank, (uint8_t)(start >> 8), (uint8_t)start,
                               (uint8_t)(data.size() >> 8), (uint8_t)data.size()};
        f.insert(f.end(), h, h + 16);
        f.insert(f.end(), data.begin(), data.end());
    }
    return f;
}
// 16 KB of `fill` with `first` at offset 0
std::vector<uint8_t> chip16(uint8_t first, uint8_t fill = 0xEA) {
    std::vector<uint8_t> c(0x4000, fill);
    c[0] = first;
    return c;
}
}  // namespace

// Bank-switching C128 cartridges (VICE c128/cart/): Magic Desk 128 and
// Comal 80 bank through $DE00; Warp Speed 128 shows its ROM at $DE00/$DF00
TEST_CASE(c128_bank_switching_carts) {
    std::string err;
    MachineDescriptor* m = c128::create(false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    IBus* bus = m->buses[0].bus;
    // Magic Desk 128: a raw 64 KB image, 4 banks of 16 KB at $8000
    std::vector<uint8_t> md;
    for (int b = 0; b < 4; ++b) { auto c = chip16((uint8_t)(0x40 + b)); md.insert(md.end(), c.begin(), c.end()); }
    ASSERT(c128::insertCartridge(m, md, "md.bin", err));
    EXPECT_EQ(c128::cartridgeInfo(m), std::string("md.bin (Magic Desk 128)"));
    bus->write8(0xFF00, 0x08);                       // $8000 external, I/O in
    EXPECT_EQ(bus->read8(0x8000), 0x40);
    bus->write8(0xDE00, 2);
    EXPECT_EQ(bus->read8(0x8000), 0x42);
    bus->write8(0xDE00, 7);                          // masked to 4 banks
    EXPECT_EQ(bus->read8(0x8000), 0x43);
    bus->write8(0xFF00, 0x20);                       // $C000 external: nothing there
    EXPECT_EQ(bus->read8(0xC000), 0xFF);
    // Comal 80: 6 x 16 KB at $C000; $DE00 bits 6-5 the chip, bit 4 its half
    std::vector<uint8_t> comal;
    for (int b = 0; b < 6; ++b) { auto c = chip16((uint8_t)(0x10 + b)); comal.insert(comal.end(), c.begin(), c.end()); }
    ASSERT(c128::insertCartridge(m, comal, "comal.bin", err));
    bus->write8(0xFF00, 0x20);
    const uint8_t sel[6] = {0x00, 0x10, 0x40, 0x50, 0x60, 0x70};
    for (int b = 0; b < 6; ++b) {
        bus->write8(0xDE00, sel[b]);
        EXPECT_EQ(bus->read8(0xC000), 0x10 + b);
    }
    EXPECT_EQ(bus->read8(0xDE00), 0x70);             // the register's upper bits
    // Warp Speed 128 (.crt type 1): 16 KB mirrored; $DE00/$DF00 are its $9E00/$9F00
    auto ws = chip16(0x57);
    ws[0x1E05] = 0x5A; ws[0x1F07] = 0xA5;
    ASSERT(c128::insertCartridge(m, c128Crt(1, {{0, 0x8000, ws}}), "ws.crt", err));
    EXPECT_EQ(c128::cartridgeInfo(m), std::string("ws.crt (Warp Speed 128)"));
    bus->write8(0xFF00, 0x28);
    EXPECT_EQ(bus->read8(0x8000), 0x57);
    EXPECT_EQ(bus->read8(0xC000), 0x57);
    EXPECT_EQ(bus->read8(0xDE05), 0x5A);
    EXPECT_EQ(bus->read8(0xDF07), 0xA5);
    // Not a C128 cartridge
    ASSERT(!c128::insertCartridge(m, std::vector<uint8_t>(40000, 0), "x.bin", err));
    delete m;
}

// Partner 128: 64 x 128 bytes of RAM at $DE00-$DE7F, $DE80 the bank; the
// freeze button forces the NMI vector to $DEDE until $DE80 bit 6
TEST_CASE(c128_partner128) {
    std::string err;
    MachineDescriptor* m = c128::create(false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    IBus* bus = m->buses[0].bus;
    auto rom = chip16(0x50);
    rom[0x1E90] = 0x99;
    ASSERT(c128::insertCartridge(m, c128Crt(2, {{0, 0x8000, rom}}), "p.crt", err));
    bus->write8(0xFF00, 0x00);                       // ROMs and I/O
    bus->write8(0xDE80, 3);
    bus->write8(0xDE10, 0x77);
    EXPECT_EQ(bus->read8(0xDE10), 0x77);
    bus->write8(0xD605, 0x66);                       // $D600-$D60F also write the RAM
    EXPECT_EQ(bus->read8(0xDE05), 0x66);
    bus->write8(0xDE80, 0);
    EXPECT_EQ(bus->read8(0xDE10), 0xFF);
    EXPECT_EQ(bus->read8(0xDE90), 0x99);             // the ROM's $9E90
    const uint8_t vec = bus->read8(0xFFFA);
    m->onKey("freeze", true);
    m->onKey("freeze", false);
    EXPECT_EQ(bus->read8(0xFFFA), 0xDE);
    EXPECT_EQ(bus->read8(0xFFFB), 0xDE);
    bus->write8(0xDE80, 0x40);                       // release
    EXPECT_EQ(bus->read8(0xFFFA), vec);
    delete m;
}

// GMod2-C128: 32 x 16 KB of 29F040 flash at $8000 and $C000, $DE00 bits
// 0-4 the bank; flash programming; the M93C86 EEPROM through $DE00 bits 4-7
TEST_CASE(c128_gmod2) {
    std::string err;
    MachineDescriptor* m = c128::create(false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    IBus* bus = m->buses[0].bus;
    ASSERT(c128::insertCartridge(m, c128Crt(5, {{0, 0x8000, chip16(0x30)}, {3, 0x8000, chip16(0x33, 0xFF)}}), "g.crt", err));
    bus->write8(0xFF00, 0x28);                       // $8000 and $C000 external
    EXPECT_EQ(bus->read8(0x8000), 0x30);
    bus->write8(0xDE00, 3);
    EXPECT_EQ(bus->read8(0x8000), 0x33);
    EXPECT_EQ(bus->read8(0xC000), 0x33);
    // Program $12 at bank 3 + $10: $5555 <- $AA (bank 1 + $1555), $2AAA <- $55, $5555 <- $A0
    bus->write8(0xDE00, 1); bus->write8(0x9555, 0xAA);
    bus->write8(0xDE00, 0); bus->write8(0xAAAA, 0x55);
    bus->write8(0xDE00, 1); bus->write8(0x9555, 0xA0);
    bus->write8(0xDE00, 3); bus->write8(0x8010, 0x12);
    EXPECT_EQ(bus->read8(0x8010), 0x12);
    // EEPROM: write enable, write word 5 = $1234, read it back
    auto clockBit = [&](int bit) {
        bus->write8(0xDE00, (uint8_t)(0x40 | (bit << 4)));          // CS, data, clock low
        bus->write8(0xDE00, (uint8_t)(0x60 | (bit << 4)));          // clock high
    };
    auto send = [&](uint32_t bits, int n) { for (int i = n - 1; i >= 0; --i) clockBit((bits >> i) & 1); };
    auto deselect = [&]() { bus->write8(0xDE00, 0x00); };
    deselect(); send(0x13 << 8, 13); deselect();                        // 1 0011 xxxxxxxx: WEN
    send((0x5u << 26) | (5u << 16) | 0x1234, 29); deselect();            // 101 addr10 data16: WRITE
    send((0x6u << 10) | 5u, 13);                                         // 110 addr10: READ
    unsigned word = 0;
    for (int i = 0; i < 16; ++i) {
        clockBit(0);
        word = (word << 1) | ((bus->read8(0xDE00) >> 7) & 1);
    }
    deselect();
    EXPECT_EQ(word, 0x1234u);
    delete m;
}

// An REU on the C128's expansion port: I/O2 in C128 mode, the $FF00 trigger
// beside the MMU's configuration register, DMA through the 8502's map
TEST_CASE(c128_reu_expansion) {
    std::string err;
    MachineDescriptor* m = boot(err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    auto* slot = m->expansionSlot("port");
    ASSERT(slot != nullptr);
    ASSERT(slot->install("reu", "", err));          // the default: 512K
    EXPECT_EQ(slot->info(), std::string("reu (REU, 512 KB)"));
    m->onReset(*m);
    run(m, 3000000);
    ASSERT(screenHas(m, "READY."));
    auto* reu = dynamic_cast<Reu*>(m->ioRegistry->findHandler("REU"));
    ASSERT(reu != nullptr);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0xDF00), 0x10);
    auto regs = [&](uint16_t host, uint32_t reuAddr, uint16_t len) {
        bus->write8(0xDF02, (uint8_t)host); bus->write8(0xDF03, (uint8_t)(host >> 8));
        bus->write8(0xDF04, (uint8_t)reuAddr); bus->write8(0xDF05, (uint8_t)(reuAddr >> 8));
        bus->write8(0xDF06, (uint8_t)(reuAddr >> 16));
        bus->write8(0xDF07, (uint8_t)len); bus->write8(0xDF08, (uint8_t)(len >> 8));
    };
    regs(0x0400, 0x050000, 1000);
    bus->write8(0xDF01, 0x90);
    int cycles = 0;
    while (reu->dmaActive()) cycles += m->schedulerStep(*m);
    EXPECT_TRUE(cycles >= 1000);
    bool same = true;
    for (int i = 0; i < 1000; ++i) same = same && reu->ram()[0x50000 + (size_t)i] == bus->peek8(0x0400 + (uint32_t)i);
    EXPECT_TRUE(same);
    // $FF00: the MMU keeps its configuration (the same value written back)
    // and the REU starts
    regs(0x2000, 0x050000, 1000);
    bus->write8(0xDF01, 0x81);
    const uint8_t cr = bus->peek8(0xFF00);
    bus->write8(0xFF00, cr);
    EXPECT_TRUE(reu->dmaActive());
    while (reu->dmaActive()) m->schedulerStep(*m);
    same = true;
    for (int i = 0; i < 1000; ++i) same = same && bus->peek8(0x2000 + (uint32_t)i) == reu->ram()[0x50000 + (size_t)i];
    EXPECT_TRUE(same);
    ASSERT(slot->install("", "", err));
    EXPECT_TRUE(m->ioRegistry->findHandler("REU") == nullptr);
    EXPECT_EQ(slot->info(), std::string(""));
    delete m;
}

// BASIC 7.0's own REU commands: STASH, FETCH and SWAP go through the
// KERNAL's DMA call ($FF50), which arms the REU and triggers it with its
// write to $FF00
TEST_CASE(c128_reu_basic_stash_fetch) {
    std::string err;
    MachineDescriptor* m = c128::create(false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    ASSERT(m->expansionSlot("port")->install("reu", "512K", err));
    m->onReset(*m);
    run(m, 3000000);
    auto* kbd = dynamic_cast<IKeyboardMatrix*>(m->ioRegistry->findHandler("Keyboard"));
    ASSERT(kbd != nullptr);
    kbd->enqueueText("BANK 0:POKE 4864,77:POKE 4865,88:STASH 2,4864,100,3:POKE 4864,1:POKE 4865,2\n");
    run(m, 20000000);
    auto* reu = dynamic_cast<Reu*>(m->ioRegistry->findHandler("REU"));
    ASSERT(reu != nullptr);
    EXPECT_EQ(reu->ram()[0x30064], 77);              // bank 3, address 100
    EXPECT_EQ(reu->ram()[0x30065], 88);
    kbd->enqueueText("FETCH 2,4880,100,3:SWAP 2,4864,100,3:PRINT PEEK(4880)*1000+PEEK(4881);PEEK(4864)\n");
    run(m, 20000000);
    EXPECT_TRUE(screenHas(m, " 77088  77"));
    EXPECT_EQ(reu->ram()[0x30064], 1);               // SWAP put 1 and 2 there
    EXPECT_EQ(reu->ram()[0x30065], 2);
    delete m;
}
