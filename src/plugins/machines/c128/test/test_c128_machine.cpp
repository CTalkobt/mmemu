#include "test_harness.h"
#include "plugins/machines/c128/main/machine_c128.h"
#include "plugins/devices/vdc8563/main/vdc8563.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libmem/main/ibus.h"
#include "libdevices/main/ivideo_output.h"
#include <cstdio>
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
    ASSERT(c128::setInternalFunctionRom(m, intRom, err));
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
