#include "test_harness.h"
#include "trs80_test_rom.h"
#include "plugins/z80/main/cpu_z80.h"
#include "plugins/devices/wd17xx/main/floppy_disk.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ivideo_output.h"
#include <cstdlib>
#include <cstdio>
#include <string>
#include <unistd.h>

using namespace trs80test;
using trs80::Model;

namespace {

trs80::System* sysOf(MachineDescriptor* m) { return trs80::systemOf(m); }

std::string screenText(MachineDescriptor* m, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += (char)m->buses[0].bus->peek8((uint16_t)(0x3C00 + i));
    return s;
}

std::vector<uint32_t> frame(MachineDescriptor* m, int& w, int& h) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    IVideoOutput* v = nullptr;
    for (auto* x : hs) if (auto* vo = dynamic_cast<IVideoOutput*>(x)) v = vo;
    auto d = v->getDimensions();
    w = d.width;
    h = d.height;
    std::vector<uint32_t> px((size_t)w * h);
    v->renderFrame(px.data());
    return px;
}

std::string tmpPath(const std::string& name) {
    return "/tmp/mmsim_trs80_" + std::to_string((long)getpid()) + "_" + name;
}

}  // namespace

// A missing ROM names the file; the machine is still made, with a stand-in saying so
TEST_CASE(trs80_missing_rom_message) {
    const char* old = std::getenv("MMEMU_TRS80_FIRMWARE");
    std::string keep = old ? old : "";
    setenv("MMEMU_TRS80_FIRMWARE", "/nonexistent-trs80-roms", 1);
    std::string err;
    MachineDescriptor* m = trs80::create(Model::M1Level2, &err);
    MachineDescriptor* m4 = trs80::create(Model::M4, nullptr);
    if (old) setenv("MMEMU_TRS80_FIRMWARE", keep.c_str(), 1); else unsetenv("MMEMU_TRS80_FIRMWARE");
    ASSERT(m != nullptr);
    ASSERT(err.find("level2.rom") != std::string::npos);
    ASSERT(err.find("MMEMU_TRS80_FIRMWARE") != std::string::npos);
    ASSERT(!m->warnings.empty());
    run(m, 100000);
    ASSERT_EQ(screenText(m, 22), std::string("NO ROM: PUT LEVEL2.ROM"));
    ASSERT(m4 != nullptr);
    run(m4, 100000);
    ASSERT_EQ(screenText(m4, 22), std::string("NO ROM: PUT MODEL4.ROM"));
    delete m;
    delete m4;
}

// Model I: ROM, the EI's registers, keyboard, video (D6 not stored), RAM
TEST_CASE(trs80_m1_memory_map) {
    auto rom = idleRom();
    rom[0x1234] = 0x5A;
    MachineDescriptor* m = make(Model::M1Level2, rom);
    ASSERT(m);
    auto* sys = sysOf(m);
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->read8(0x1234), 0x5A);
    bus->write8(0x1234, 0x00);                  // ROM
    ASSERT_EQ(bus->read8(0x1234), 0x5A);
    ASSERT_EQ(bus->read8(0x3000), 0xFF);        // nothing at $3000-$37DF
    bus->write8(0x4000, 0x11);
    bus->write8(0xFFFF, 0x22);                  // 48K: RAM to the top
    ASSERT_EQ(bus->read8(0x4000), 0x11);
    ASSERT_EQ(bus->read8(0xFFFF), 0x22);
    // Video: without the lower case mod D6 reads NOT (D5 or D7)
    bus->write8(0x3C00, 'A');
    bus->write8(0x3C01, 'a');
    bus->write8(0x3C02, 0x01);
    bus->write8(0x3C03, 0xBF);
    ASSERT_EQ(bus->read8(0x3C00), 'A');
    ASSERT_EQ(bus->read8(0x3C01), 0x21);
    ASSERT_EQ(bus->read8(0x3C02), 0x41);
    ASSERT_EQ(bus->read8(0x3C03), 0xBF);
    // The printer in the EI: always ready
    ASSERT_EQ(bus->read8(0x37E8), 0x30);
    bus->write8(0x37E8, 'P');
    ASSERT_EQ(sys->printer.printed.size(), (size_t)1);
    ASSERT_EQ(sys->printer.printed[0], 'P');
    // No disk anywhere: the FDC's status reads $FF (the ROM skips the boot)
    ASSERT_EQ(bus->read8(0x37EC), 0xFF);
    // With the lower case modification D6 is kept
    std::string err;
    ASSERT(m->expansionSlot("lowercase")->install("lowercase", "", err));
    m->onReset(*m);
    bus->write8(0x3C01, 'a');
    ASSERT_EQ(bus->read8(0x3C01), 'a');
    // Without the EI: 16K and nothing at $37E0-$37EF
    ASSERT(m->expansionSlot("ei")->install("", "", err));
    m->onReset(*m);
    ASSERT_EQ(sys->cfg().ramKb, 16);
    bus->write8(0x7FFF, 0x33);
    ASSERT_EQ(bus->read8(0x7FFF), 0x33);
    ASSERT_EQ(bus->read8(0x8000), 0xFF);
    ASSERT_EQ(bus->read8(0x37E8), 0xFF);
    ASSERT(!m->expansionSlot("ram")->install("ram", "48K", err));   // needs the EI
    // Level I: 4 KB of ROM, 4K of RAM
    MachineDescriptor* l1 = make(Model::M1Level1, idleRom(0x1000));
    ASSERT(l1);
    ASSERT_EQ(sysOf(l1)->cfg().ramKb, 4);
    l1->buses[0].bus->write8(0x4FFF, 0x44);
    ASSERT_EQ(l1->buses[0].bus->read8(0x4FFF), 0x44);
    ASSERT_EQ(l1->buses[0].bus->read8(0x5000), 0xFF);
    delete l1;
    delete m;
}

// The keyboard matrix: address bits select rows, keys read 1
TEST_CASE(trs80_keyboard_matrix) {
    MachineDescriptor* m = make(Model::M1Level2, idleRom());
    auto* sys = sysOf(m);
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->read8(0x38FF), 0x00);
    ASSERT(m->onKey("a", true));
    ASSERT_EQ(bus->read8(0x3801), 0x02);        // row 0 bit 1
    ASSERT_EQ(bus->read8(0x3802), 0x00);
    ASSERT(m->onKey("return", true));
    ASSERT_EQ(bus->read8(0x3840), 0x01);        // row 6 bit 0
    ASSERT_EQ(bus->read8(0x3841), 0x03);        // rows 0 and 6 ORed
    ASSERT(m->onKey("shift", true));
    ASSERT_EQ(bus->read8(0x3880), 0x01);
    ASSERT(m->onKey("dquote", true));           // SHIFT and 2
    ASSERT_EQ(bus->read8(0x3810), 0x04);
    m->onKey("a", false);
    m->onKey("return", false);
    m->onKey("shift", false);
    m->onKey("dquote", false);
    ASSERT_EQ(bus->read8(0x38FF), 0x00);
    ASSERT(!m->onKey("f1", true));              // the Model 4's
    // Typed text: a key at a time
    sys->keyboard.enqueueText("B?");
    ASSERT(sys->keyboard.typing());
    runFor(m, 0.02);
    ASSERT_EQ(bus->read8(0x3801), 0x04);        // B down
    runFor(m, 0.1);
    ASSERT_EQ(bus->read8(0x3820), 0x80);        // then / with SHIFT
    ASSERT_EQ(bus->read8(0x3880), 0x01);
    runFor(m, 0.2);
    ASSERT(!sys->keyboard.typing());
    ASSERT_EQ(bus->read8(0x38FF), 0x00);
    // The Model 4's extra keys in row 7
    MachineDescriptor* m4 = make(Model::M4, idleRom());
    ASSERT(m4->onKey("ctrl", true));
    ASSERT(m4->onKey("f3", true));
    ASSERT(m4->onKey("right_shift", true));
    ASSERT_EQ(m4->buses[0].bus->read8(0x3880), 0x46);
    delete m4;
    delete m;
}

// Model III: ROM to $37FF with the printer over $37E8, ports
TEST_CASE(trs80_m3_memory_map) {
    auto rom = idleRom(0x3800);
    rom[0x37FF] = 0xA5;
    rom[0x37E8] = 0x00;
    MachineDescriptor* m = make(Model::M3, rom);
    auto* sys = sysOf(m);
    IBus* bus = m->buses[0].bus;
    IBus* io = m->buses[1].bus;
    ASSERT_EQ(bus->read8(0x37FF), 0xA5);
    ASSERT_EQ(bus->read8(0x37E8), 0x30);        // the printer's status
    bus->write8(0xC000, 0x77);
    ASSERT_EQ(bus->read8(0xC000), 0x77);
    bus->write8(0x3C00, 'a');                   // all 8 bits of video
    ASSERT_EQ(bus->read8(0x3C00), 'a');
    // The interrupt latch reads active low; the mask is written at $E0
    ASSERT_EQ(io->read8(0xE0), 0xEF);           // only the UART's transmitter empty (bit 4)
    io->write8(0xE0, 0x04);
    ASSERT_EQ(sys->intMask(), 0x04);
    runFor(m, 0.05);                            // a 30 Hz tick
    ASSERT_EQ(io->read8(0xE0) & 0x04, 0);
    io->read8(0xEC);                            // acknowledged
    ASSERT_EQ(io->read8(0xE0) & 0x04, 0x04);
    // The printer and port $FF
    io->write8(0xF8, 'Q');
    ASSERT_EQ(io->read8(0xF8), 0x30);
    ASSERT_EQ(sys->printer.printed.back(), 'Q');
    io->write8(0xEC, 0x04);                     // 32 columns
    ASSERT_EQ(io->read8(0xFF) & 0x7E, 0x04);
    ASSERT(sys->wide());
    // RS-232 loopback through the TR1602
    ASSERT(sys->uart.attachSerial("loopback"));
    io->write8(0xE8, 0);
    io->write8(0xE9, 0xEE);                     // 9600 baud
    io->write8(0xEA, 0x6C);                     // 8 bits, no parity
    io->write8(0xE0, 0x20);                     // receive interrupts
    io->write8(0xEB, 'X');
    ASSERT_EQ(io->read8(0xEA) & 0x40, 0);       // sending
    runFor(m, 0.003);
    ASSERT_EQ(io->read8(0xEA) & 0xC0, 0xC0);    // sent, and received back
    ASSERT_EQ(io->read8(0xE0) & 0x20, 0);       // the receive interrupt
    ASSERT_EQ(io->read8(0xEB), 'X');
    ASSERT_EQ(io->read8(0xEA) & 0x80, 0);
    ASSERT_EQ(sys->uart.sent.size(), (size_t)1);
    delete m;
}

// Model 4: port $84's memory maps, 32K banks and video page
TEST_CASE(trs80_m4_bank_switching) {
    auto rom = idleRom(0x3800);
    rom[0x0100] = 0xC3;
    MachineDescriptor* m = make(Model::M4, rom);
    auto* sys = sysOf(m);
    IBus* bus = m->buses[0].bus;
    IBus* io = m->buses[1].bus;
    // Map 0: the Model III's
    ASSERT_EQ(bus->read8(0x0100), 0xC3);
    bus->write8(0x8000, 0x01);
    bus->write8(0x4000, 0x02);
    // Upper 32K from block 2 (bits 6-4 = 2), then 3
    io->write8(0x84, 0x20);
    ASSERT_EQ(bus->read8(0x8000), 0x00);
    bus->write8(0x8000, 0x21);
    io->write8(0x84, 0x30);
    bus->write8(0x8000, 0x31);
    io->write8(0x84, 0x00);
    ASSERT_EQ(bus->read8(0x8000), 0x01);
    ASSERT_EQ(sys->ramByte(0x10000), 0x21);
    ASSERT_EQ(sys->ramByte(0x18000), 0x31);
    // Lower 32K from block 2 (6) or 3 (7): $4000 moves too
    io->write8(0x84, 0x60);
    ASSERT_EQ(bus->read8(0x4000), sys->ramByte(0x14000));
    bus->write8(0x4000, 0x62);
    ASSERT_EQ(sys->ramByte(0x14000), 0x62);
    io->write8(0x84, 0x00);
    ASSERT_EQ(bus->read8(0x4000), 0x02);
    // Map 1: RAM for the ROM; keyboard and video stay
    io->write8(0x84, 0x01);
    bus->write8(0x0100, 0x55);
    ASSERT_EQ(bus->read8(0x0100), 0x55);
    bus->write8(0x3C00, 'M');
    ASSERT_EQ(sys->videoByte(0), 'M');
    io->write8(0x84, 0x00);
    ASSERT_EQ(bus->read8(0x0100), 0xC3);
    // The video page: $3C00 shows the second 1K
    io->write8(0x84, 0x80);
    bus->write8(0x3C00, 'P');
    ASSERT_EQ(sys->videoByte(0x400), 'P');
    ASSERT_EQ(sys->videoByte(0), 'M');
    // Map 2: keyboard at $F400, the 2K of video at $F800
    io->write8(0x84, 0x02);
    ASSERT_EQ(bus->read8(0xF800), 'M');
    ASSERT_EQ(bus->read8(0xFC00), 'P');
    ASSERT_EQ(bus->read8(0x0100), 0x55);        // RAM
    m->onKey("a", true);
    ASSERT_EQ(bus->read8(0xF401), 0x02);
    m->onKey("a", false);
    // Map 3: all RAM
    io->write8(0x84, 0x03);
    bus->write8(0xF800, 0x99);
    ASSERT_EQ(sys->videoByte(0), 'M');
    ASSERT_EQ(bus->read8(0xF800), 0x99);
    // 64K: no banks
    std::string err;
    ASSERT(m->expansionSlot("ram")->install("ram", "64K", err));
    m->onReset(*m);
    bus->write8(0x8000, 0x07);
    io->write8(0x84, 0x20);
    ASSERT_EQ(bus->read8(0x8000), 0x07);
    // The 4 MHz clock (port $EC bit 6)
    io->write8(0xEC, 0x40);
    ASSERT(sys->fast());
    ASSERT_EQ(sys->ticksPerT(), 5);
    delete m;
}

// Model 4P: the boot ROM over $0000-$0FFF until port $9C bit 0 goes low
TEST_CASE(trs80_m4p_boot_rom) {
    auto rom = idleRom(0x1000);
    rom[0x0800] = 0x4B;
    MachineDescriptor* m = make(Model::M4P, rom);
    IBus* bus = m->buses[0].bus;
    IBus* io = m->buses[1].bus;
    ASSERT_EQ(bus->read8(0x0800), 0x4B);
    io->write8(0x84, 0x01);                     // map 1: writes reach RAM
    bus->write8(0x0800, 0x22);
    bus->write8(0x2000, 0x33);
    ASSERT_EQ(bus->read8(0x0800), 0x4B);        // the ROM still in
    io->write8(0x9C, 0x00);
    ASSERT_EQ(bus->read8(0x0800), 0x22);
    io->write8(0x84, 0x00);                     // map 0 reads the RAM below $3800
    ASSERT_EQ(bus->read8(0x2000), 0x33);
    io->write8(0x9C, 0x01);
    ASSERT_EQ(bus->read8(0x0800), 0x4B);
    delete m;
}

// Video: the characters, block graphics and modes as pixels
TEST_CASE(trs80_video_modes) {
    int w, h;
    // Model I: 64 x 16 of 6 x 12, each line twice
    MachineDescriptor* m = make(Model::M1Level2, idleRom());
    IBus* bus = m->buses[0].bus;
    bus->write8(0x3C00, 'A');
    bus->write8(0x3C01, 0x80 | 0x01 | 0x20);    // blocks: top left, bottom right
    auto px = frame(m, w, h);
    ASSERT_EQ(w, 384);
    ASSERT_EQ(h, 384);
    const uint32_t FG = 0xFFF0F0F0, BG = 0xFF000000;
    // 'A' line 1: ..#...
    ASSERT_EQ(px[(size_t)2 * w + 2], FG);
    ASSERT_EQ(px[(size_t)3 * w + 2], FG);
    ASSERT_EQ(px[(size_t)2 * w + 1], BG);
    // line 3: #...#.
    ASSERT_EQ(px[(size_t)6 * w + 0], FG);
    ASSERT_EQ(px[(size_t)6 * w + 4], FG);
    // The graphics character: 3 x 4 dots a block
    ASSERT_EQ(px[(size_t)0 * w + 6], FG);
    ASSERT_EQ(px[(size_t)7 * w + 8], FG);
    ASSERT_EQ(px[(size_t)0 * w + 9], BG);
    ASSERT_EQ(px[(size_t)(8 * 2) * w + 6], BG);
    ASSERT_EQ(px[(size_t)(8 * 2) * w + 11], FG);
    ASSERT_EQ(px[(size_t)(11 * 2) * w + 9], FG);
    // 32 columns (port $FF bit 3): even characters twice as wide
    m->buses[1].bus->write8(0xFF, 0x08);
    px = frame(m, w, h);
    ASSERT_EQ(px[(size_t)2 * w + 4], FG);
    ASSERT_EQ(px[(size_t)2 * w + 5], FG);
    ASSERT_EQ(px[(size_t)2 * w + 2], BG);
    ASSERT_EQ(m->buses[1].bus->read8(0xFF) & 0x40, 0);
    delete m;

    // Model III: 8-dot cells; $C0-$FF the special set
    m = make(Model::M3, idleRom(0x3800));
    bus = m->buses[0].bus;
    bus->write8(0x3C00, 'A');
    bus->write8(0x3C01, 0xC1);
    px = frame(m, w, h);
    ASSERT_EQ(w, 512);
    ASSERT_EQ(h, 384);
    ASSERT_EQ(px[(size_t)2 * w + 3], FG);
    ASSERT_EQ(px[(size_t)2 * w + 2], BG);
    ASSERT_EQ(px[(size_t)2 * w + 8 + 3], BG);   // $C1: 'A' inverted (the built-in stand-in)
    ASSERT_EQ(px[(size_t)2 * w + 8 + 2], FG);
    delete m;

    // Model 4: 80 x 24 of 8 x 10, and inverse video
    m = make(Model::M4, idleRom(0x3800));
    bus = m->buses[0].bus;
    IBus* io = m->buses[1].bus;
    bus->write8(0x3C00, 'A');
    px = frame(m, w, h);
    ASSERT_EQ(w, 640);
    ASSERT_EQ(h, 480);
    ASSERT_EQ(px[(size_t)(24 + 1) * 2 * w + 64 + 3], FG);   // 64 x 16 centred
    io->write8(0x84, 0x04);                                 // 80 x 24
    bus->write8(0x3C01, 0x81);
    px = frame(m, w, h);
    ASSERT_EQ(px[(size_t)2 * w + 3], FG);
    ASSERT_EQ(px[(size_t)2 * w + 8 + 0], FG);               // a graphics block (bit 0)
    io->write8(0x84, 0x0C);                                 // inverse: bit 7 inverts
    px = frame(m, w, h);
    ASSERT_EQ(px[(size_t)2 * w + 8 + 3], BG);
    ASSERT_EQ(px[(size_t)2 * w + 8 + 2], FG);
    ASSERT_EQ(px[(size_t)2 * w + 3], FG);
    delete m;
}

// The heartbeat: Model I EI 40 Hz, Model III 30 Hz, Model 4 at 4 MHz 60 Hz
TEST_CASE(trs80_rtc_interrupt_rate) {
    // Model I: the latch at $37E0 (bit 7), read to acknowledge
    {
        Asm a;
        a.emit({0xF3, 0x31, 0x00, 0x7F, 0xED, 0x56});        // DI; LD SP,$7F00; IM 1
        a.emit({0x21, 0x00, 0x50, 0x36, 0x00});              // LD HL,$5000; LD (HL),0
        a.emit({0xFB});                                      // EI
        a.jr(0x18, a.pc);                                    // JR $
        a.at(0x38);
        a.emit({0xF5, 0x3A, 0xE0, 0x37, 0xE6, 0x80});        // PUSH AF; LD A,($37E0); AND $80
        a.emit({0x28, 0x01, 0x34});                          // JR Z,+1; INC (HL)
        a.emit({0xF1, 0xFB, 0xED, 0x4D});                    // POP AF; EI; RETI
        MachineDescriptor* m = make(Model::M1Level2, a.b);
        runFor(m, 1.0);
        const int n = m->buses[0].bus->read8(0x5000);
        ASSERT(n >= 39 && n <= 41);
        delete m;
    }
    // Model III / 4: masked at $E0 (bit 2), acknowledged by reading $EC
    for (int fast = 0; fast < 3; ++fast) {
        Asm a(0, 0x3800);
        a.emit({0xF3, 0x31, 0x00, 0x7F, 0xED, 0x56});
        a.emit({0x21, 0x00, 0x50, 0x36, 0x00});
        a.emit({0x3E, 0x04, 0xD3, 0xE0});                    // LD A,4; OUT ($E0),A
        if (fast == 2) a.emit({0x3E, 0x40, 0xD3, 0xEC});     // LD A,$40; OUT ($EC),A: 4 MHz
        a.emit({0xFB});
        a.jr(0x18, a.pc);
        a.at(0x38);
        a.emit({0xF5, 0xDB, 0xEC, 0x34, 0xF1, 0xFB, 0xED, 0x4D});   // PUSH AF; IN A,($EC); INC (HL); POP AF; EI; RETI
        MachineDescriptor* m = make(fast ? Model::M4 : Model::M3, a.b);
        runFor(m, 1.0);
        const int n = m->buses[0].bus->read8(0x5000);
        const int want = fast == 2 ? 60 : 30;
        ASSERT(n >= want - 1 && n <= want + 1);
        delete m;
    }
}

// Model III: a sector read as the DOSes do it: WAIT on DRQ through port
// $F4, INI, and INTRQ's NMI to end it
TEST_CASE(trs80_m3_fdc_sector_read_nmi) {
    const std::string path = tmpPath("m3.dmk");
    {
        FloppyDisk d(40, 1);
        std::vector<FloppySectorSpec> secs;
        for (int s = 1; s <= 18; ++s) {
            FloppySectorSpec sp;
            sp.track = 0; sp.side = 0; sp.sector = (uint8_t)s; sp.sizeCode = 1; sp.dd = true;
            for (int i = 0; i < 256; ++i) sp.data.push_back((uint8_t)(i ^ (s * 7)));
            secs.push_back(sp);
        }
        ASSERT(d.formatTrack(0, 0, secs));
        std::string err;
        ASSERT(d.saveAs(path, FloppyFormat::DMK, &err));
    }
    Asm a(0, 0x3800);
    a.emit({0xF3, 0x31, 0x00, 0x7F});            // DI; LD SP,$7F00
    a.emit({0x3E, 0xC0, 0xD3, 0xE4});            // INTRQ and motor timeout to NMI
    a.emit({0x3E, 0x81, 0xD3, 0xF4});            // drive 0, double density
    a.emit({0x3E, 0x03, 0xD3, 0xF2});            // sector 3
    a.emit({0x3E, 0x80, 0xD3, 0xF0});            // READ SECTOR
    a.emit({0x21, 0x00, 0x50, 0x0E, 0xF3, 0x16, 0xC1});  // LD HL,$5000; LD C,$F3; LD D,$C1
    const uint16_t loop = a.pc;
    a.emit({0x7A, 0xD3, 0xF4, 0xED, 0xA2});      // LD A,D; OUT ($F4),A (WAIT); INI
    a.jr(0x18, loop);
    a.at(0x66);                                  // NMI: the sector is done
    a.emit({0xDB, 0xE4, 0x32, 0x02, 0x4F});      // IN A,($E4); LD ($4F02),A: why
    a.emit({0xDB, 0xF0, 0x32, 0x00, 0x4F});      // IN A,($F0); LD ($4F00),A: the status
    a.emit({0x3E, 0xAA, 0x32, 0x01, 0x4F});      // LD A,$AA; LD ($4F01),A
    a.jr(0x18, a.pc);
    MachineDescriptor* m = make(Model::M3, a.b);
    // As the CLI's -i: the first drive that takes it
    IOHandler* h = m->ioRegistry->mountDisk(1, path);
    ASSERT(h != nullptr);
    ASSERT_EQ(std::string(h->name()), std::string("Floppy"));
    runFor(m, 0.5);
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->read8(0x4F01), 0xAA);
    ASSERT_EQ(bus->read8(0x4F00) & 0x1D, 0);     // not busy, no lost data, CRC or RNF
    ASSERT_EQ(bus->read8(0x4F02) & 0x80, 0);     // INTRQ shown (active low)
    for (int i = 0; i < 256; ++i) ASSERT_EQ(bus->read8((uint16_t)(0x5000 + i)), (uint8_t)(i ^ 21));
    ASSERT_EQ(bus->read8(0x5100), 0x00);         // no more
    delete m;
    std::remove(path.c_str());
}

// Model I: the FD1771 polled at $37EC, a JV1 image as a .dsk
TEST_CASE(trs80_m1_fdc_sector_read) {
    const std::string path = tmpPath("m1.dsk");
    {
        FloppyDisk d(35, 1);
        std::vector<FloppySectorSpec> secs;
        for (int s = 0; s < 10; ++s) {
            FloppySectorSpec sp;
            sp.track = 0; sp.side = 0; sp.sector = (uint8_t)s; sp.sizeCode = 1; sp.dd = false;
            for (int i = 0; i < 256; ++i) sp.data.push_back((uint8_t)(i + s));
            secs.push_back(sp);
        }
        ASSERT(d.formatTrack(0, 0, secs));
        std::string err;
        ASSERT(d.saveAs(path, FloppyFormat::JV1, &err));
    }
    Asm a;
    a.emit({0xF3, 0x31, 0x00, 0x7F});            // DI; LD SP,$7F00
    a.emit({0x3E, 0x01, 0x32, 0xE1, 0x37});      // drive 0
    a.emit({0x3E, 0x04, 0x32, 0xEE, 0x37});      // sector 4
    a.emit({0x3E, 0x88, 0x32, 0xEC, 0x37});      // READ SECTOR (IBM lengths)
    a.emit({0x21, 0x00, 0x50});                  // LD HL,$5000
    const uint16_t loop = a.pc;
    a.emit({0x3A, 0xEC, 0x37, 0xCB, 0x4F});      // LD A,($37EC); BIT 1,A
    const uint16_t jrByte = a.pc;
    a.emit({0x20, 0x00});                        // JR NZ,byte (patched)
    a.emit({0xCB, 0x47});                        // BIT 0,A
    a.jr(0x20, loop);                            // JR NZ,loop
    a.emit({0x32, 0x00, 0x4F, 0x3E, 0xAA, 0x32, 0x01, 0x4F});
    a.jr(0x18, a.pc);
    const uint16_t byte = a.pc;
    a.b[jrByte + 1] = (uint8_t)(byte - (jrByte + 2));
    a.emit({0x3A, 0xEF, 0x37, 0x77, 0x23});      // LD A,($37EF); LD (HL),A; INC HL
    a.jr(0x18, loop);
    MachineDescriptor* m = make(Model::M1Level2, a.b);
    std::string err;
    ASSERT(m->expansionSlot("doubler")->install("percom", "", err));
    ASSERT(sysOf(m)->mountDisk(0, path));
    runFor(m, 0.5);
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->read8(0x4F01), 0xAA);
    ASSERT_EQ(bus->read8(0x4F00) & 0x1C, 0);
    for (int i = 0; i < 256; ++i) ASSERT_EQ(bus->read8((uint16_t)(0x5000 + i)), (uint8_t)(i + 4));
    // The Percom Doubler's controller select: command $FF double density, $FE single
    bus->write8(0x37EC, 0xFF);
    ASSERT(sysOf(m)->fdc().doubleDensity());
    bus->write8(0x37EC, 0xFE);
    ASSERT(!sysOf(m)->fdc().doubleDensity());
    // The INTRQ latch bit (6) at $37E0
    bus->write8(0x37EC, 0xD8);                   // force interrupt now
    ASSERT_EQ(bus->read8(0x37E0) & 0x40, 0x40);
    delete m;
    std::remove(path.c_str());
}

// -i image.dmk: no program loader takes it, the drives do (unit 1 = drive :0)
TEST_CASE(trs80_dmk_mount) {
    const std::string path = tmpPath("mount.dmk");
    {
        FloppyDisk d(40, 1);
        std::vector<FloppySectorSpec> secs(1);
        secs[0].data.assign(256, 0xE5);
        ASSERT(d.formatTrack(0, 0, secs));
        ASSERT(d.saveAs(path, FloppyFormat::DMK));
    }
    MachineDescriptor* m = make(Model::M4, idleRom(0x3800));
    IOHandler* h = m->ioRegistry->mountDisk(1, path);
    ASSERT(h != nullptr);
    ASSERT_EQ(h->getMountedDiskPath(1), path);
    ASSERT(sysOf(m)->fdc().disk(0) != nullptr);
    ASSERT_EQ((int)sysOf(m)->fdc().disk(0)->format(), (int)FloppyFormat::DMK);
    int t, s;
    bool led;
    ASSERT(h->getDiskStatus(2, t, s, led));
    ASSERT(!h->getDiskStatus(5, t, s, led));
    ASSERT(m->ioRegistry->mountDisk(2, path) != nullptr);
    ASSERT(sysOf(m)->fdc().disk(1) != nullptr);
    h->ejectDisk(1);
    ASSERT(sysOf(m)->fdc().disk(0) == nullptr);
    ASSERT(m->ioRegistry->mountDisk(1, "/nonexistent.dmk") == nullptr);
    delete m;
    std::remove(path.c_str());
}

// /CMD programs: load blocks and the transfer address
TEST_CASE(trs80_cmd_loader) {
    MachineDescriptor* m = make(Model::M1Level2, idleRom());
    std::vector<uint8_t> cmd = {
        0x05, 0x04, 'T', 'E', 'S', 'T',           // module name
        0x01, 0x05, 0x00, 0x60, 0x3E, 0x42, 0x76, // $6000: LD A,$42; HALT
        0x01, 0x04, 0x00, 0x3C, 'H', 'I',         // to the screen
        0x02, 0x02, 0x00, 0x60,                   // transfer to $6000
    };
    std::string err;
    ASSERT(trs80::loadCmd(m, cmd, err));
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->read8(0x6000), 0x3E);
    ASSERT_EQ(bus->read8(0x6002), 0x76);
    ASSERT_EQ(screenText(m, 2), std::string("HI"));
    ASSERT_EQ(m->cpus[0].cpu->pc(), 0x6000u);
    run(m, 100);
    ASSERT_EQ(static_cast<Z80*>(m->cpus[0].cpu)->a(), 0x42);
    ASSERT(!trs80::loadCmd(m, {0x05, 0x01, 'X'}, err));
    delete m;
}

// Snapshots keep memory, the ports and the interrupt state
TEST_CASE(trs80_snapshot_roundtrip) {
    MachineDescriptor* m = make(Model::M4, idleRom(0x3800));
    IBus* bus = m->buses[0].bus;
    IBus* io = m->buses[1].bus;
    bus->write8(0x9000, 0x5E);
    io->write8(0x84, 0x84);
    io->write8(0xE0, 0x04);
    std::vector<uint8_t> st(bus->stateSize());
    bus->saveState(st.data());
    m->onReset(*m);
    ASSERT_EQ(bus->read8(0x9000), 0x00);
    bus->loadState(st.data());
    ASSERT_EQ(bus->read8(0x9000), 0x5E);
    ASSERT_EQ(sysOf(m)->port84(), 0x84);
    ASSERT_EQ(sysOf(m)->intMask(), 0x04);
    delete m;
}
