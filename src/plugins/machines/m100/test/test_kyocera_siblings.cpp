// The Model 100's siblings: the NEC PC-8201A, the Olivetti M10, the
// Kyotronic 85 (issue #358), with stand-in ROMs.

#include "test_harness.h"
#include "m100_test_rom.h"
#include "plugins/8085/main/cpu8085.h"
#include <cstdlib>
#include <string>

using namespace m100test;
using m100::Model;

namespace {

m100::System* sysOf(MachineDescriptor* m) { return m100::systemOf(m); }
IBus* io(MachineDescriptor* m) { return m->buses[1].bus; }

/** Rising and falling edges in a recorded .wav's 8-bit samples (from its midpoint). */
int wavEdges(const std::vector<uint8_t>& w, size_t from, size_t count) {
    int n = 0;
    for (size_t i = 44 + from + 1; i < 44 + from + count && i < w.size(); ++i)
        if ((w[i] >= 128) != (w[i - 1] >= 128)) ++n;
    return n;
}

}  // namespace

// Each sibling by name, with its ROM file named when it is missing, and its RAM choices
TEST_CASE(kyocera_siblings_descriptors) {
    const char* old = std::getenv("MMEMU_M100_FIRMWARE");
    std::string keep = old ? old : "";
    setenv("MMEMU_M100_FIRMWARE", "/nonexistent-m100-roms", 1);
    std::string ePc, eM10, eKc;
    MachineDescriptor* pc = m100::create(Model::PC8201, &ePc);
    MachineDescriptor* m10 = m100::create(Model::M10, &eM10);
    MachineDescriptor* kc = m100::create(Model::KC85, &eKc);
    if (old) setenv("MMEMU_M100_FIRMWARE", keep.c_str(), 1); else unsetenv("MMEMU_M100_FIRMWARE");
    ASSERT(pc && m10 && kc);
    ASSERT(ePc.find("pc8201a.rom") != std::string::npos);
    ASSERT(eM10.find("m10.rom") != std::string::npos);
    ASSERT(eKc.find("kc85.rom") != std::string::npos);
    ASSERT_EQ(pc->machineId, std::string("pc8201a"));
    ASSERT_EQ(m10->machineId, std::string("m10"));
    ASSERT_EQ(kc->machineId, std::string("kc85"));
    // The stand-in draws its message on the shared LCD
    runFor(pc, 0.2);
    ASSERT(sysOf(pc)->cpu()->isHalted());
    ASSERT(sysOf(pc)->pixel(1, 4) || sysOf(pc)->pixel(0, 4));
    ASSERT_EQ(sysOf(pc)->ramKb(), 96);
    ASSERT_EQ(sysOf(m10)->ramKb(), 32);
    std::string err;
    ASSERT(pc->expansionSlots[0].install("ram", "16K", err));
    ASSERT_EQ(sysOf(pc)->ramKb(), 16);
    ASSERT(!pc->expansionSlots[0].install("ram", "8K", err));
    ASSERT(m10->expansionSlots[0].install("ram", "8K", err));
    ASSERT(pc->romSocket("optrom") != nullptr);
    delete pc;
    delete m10;
    delete kc;
}

// PC-8201A: port $A0 bits 0-1 pick $0000-$7FFF (ROM, option ROM, RAM banks 2-3),
// bits 2-3 $8000-$FFFF (internal RAM, nothing, banks 2-3); bits 6-7 read back $90's
TEST_CASE(pc8201_memory_banks) {
    auto rom = idleRom(Model::PC8201);
    rom[0x0010] = 0x5A;
    std::vector<uint8_t> opt(0x8000, 0x00);
    opt[0x0010] = 0xA5;
    MachineDescriptor* m = make(Model::PC8201, rom, 96, opt);
    auto* s = sysOf(m);
    IBus* bus = m->buses[0].bus;
    IBus* ports = io(m);
    ASSERT_EQ(bus->read8(0x0010), 0x5A);
    bus->write8(0x0010, 0);
    ASSERT_EQ(bus->read8(0x0010), 0x5A);         // ROM: read only
    bus->write8(0x8000, 0x11);                    // internal RAM (32 KB of 96)
    ASSERT_EQ(bus->read8(0x8000), 0x11);
    ports->write8(0xA0, 0x01);                    // the option ROM
    ASSERT_EQ(bus->read8(0x0010), 0xA5);
    ASSERT_EQ(s->romBank(), 1);
    ports->write8(0xA0, 0x02);                    // RAM bank 2 low: writable
    bus->write8(0x0010, 0x22);
    ASSERT_EQ(bus->read8(0x0010), 0x22);
    ports->write8(0xA0, 0x03);                    // bank 3
    bus->write8(0x0010, 0x33);
    ASSERT_EQ(bus->read8(0x0010), 0x33);
    ports->write8(0xA0, 0x08);                    // ROM low, bank 2 high: the same 32 KB
    ASSERT_EQ(bus->read8(0x8010), 0x22);
    ASSERT_EQ(bus->read8(0x0010), 0x5A);
    ports->write8(0xA0, 0x0C);
    ASSERT_EQ(bus->read8(0x8010), 0x33);
    ASSERT_EQ(s->ramBank(), 3);
    ports->write8(0xA0, 0x04);                    // bank 1: nothing
    bus->write8(0x8000, 0x44);
    ASSERT_EQ(bus->read8(0x8000), 0xFF);
    ports->write8(0xA0, 0x00);
    ASSERT_EQ(bus->read8(0x8000), 0x11);
    // The serial select from $90 bits 6-7 and the bank in $A0's read
    ports->write8(0x90, 0x80);
    ports->write8(0xA1, 0x09);                    // mirrored over $A0-$AF
    ASSERT_EQ(ports->read8(0xA0), 0x89);
    ASSERT_EQ(s->scpPort(), 0x80);
    ASSERT_EQ(ports->read8(0x8C), 0xFF);          // no ROM cassette
    // Code in the internal RAM switching banks, as the ROM's does
    ports->write8(0xA0, 0x00);
    runInRam(m, 0xF000,
             "MVI A,$02\nOUT $A0\nMVI A,$77\nSTA $1000\n"     // bank 2 at $0000
             "MVI A,$08\nOUT $A0\nLDA $9000\nSTA $F100\n"     // and at $8000 (code at $F000 gone)
             "HLT\n");
    run(m, 200);
    // After the switch the CPU runs on in bank 2 (empty: RST 7s); look at the RAM itself
    ports->write8(0xA0, 0x02);
    ASSERT_EQ(bus->read8(0x1000), 0x77);
    delete m;
    // 32 KB: no banks 2-3; 16 KB: from $C000
    m = make(Model::PC8201, rom, 32);
    io(m)->write8(0xA0, 0x02);
    m->buses[0].bus->write8(0x0010, 0x12);
    ASSERT_EQ(m->buses[0].bus->read8(0x0010), 0xFF);
    delete m;
    m = make(Model::PC8201, rom, 16);
    m->buses[0].bus->write8(0xBFFF, 0x12);
    m->buses[0].bus->write8(0xC000, 0x34);
    ASSERT_EQ(m->buses[0].bus->read8(0xBFFF), 0xFF);
    ASSERT_EQ(m->buses[0].bus->read8(0xC000), 0x34);
    delete m;
}

// PC-8201A port $90: the printer strobe (a byte as it rises), the cassette motor;
// $E0 writes do nothing (no option ROM bit), the 81C55, LCD and UART as the Model 100
TEST_CASE(pc8201_ports) {
    MachineDescriptor* m = make(Model::PC8201, romFrom(
        "LXI SP,$0000\nMVI A,$03\nOUT $B0\n"
        "MVI A,$48\nOUT $B1\nMVI A,$20\nOUT $90\nXRA A\nOUT $90\n"     // 'H'
        "MVI A,$49\nOUT $B1\nMVI A,$20\nOUT $90\nMVI A,$08\nOUT $90\n" // 'I', then the motor
        "MVI A,$01\nOUT $E0\nLDA $0100\nSTA $F000\nHLT\n"));
    auto* s = sysOf(m);
    run(m, 2000);
    ASSERT(s->cpu()->isHalted());
    ASSERT_EQ(std::string(s->printer.printed.begin(), s->printer.printed.end()), std::string("HI"));
    ASSERT(s->tape.motor());
    ASSERT_EQ(s->romBank(), 0);
    ASSERT_EQ(m->buses[0].bus->read8(0xF000), 0xFF);   // still the ROM ($FF there)
    delete m;
}

// PC-8201A keyboard: MAME's pc8201a matrix, CAPS LOCK at column 8 bit 4
TEST_CASE(pc8201_keyboard_scan) {
    MachineDescriptor* m = make(Model::PC8201, idleRom(Model::PC8201));
    auto* s = sysOf(m);
    ASSERT(m->onKey("bracket_left", true));       // @: column 3 bit 2
    ASSERT(m->onKey("space", true));              // column 5 bit 6
    ASSERT(m->onKey("return", true));             // column 6 bit 7
    ASSERT(m->onKey("f5", true));                 // column 7 bit 4
    ASSERT(!m->onKey("f6", true));                // five function keys
    ASSERT(m->onKey("f8", true));                 // STOP: column 7 bit 7
    ASSERT_EQ(s->keyboard.read(0x1F7), 0xFB);
    ASSERT_EQ(s->keyboard.read(0x1DF), 0xBF);
    ASSERT_EQ(s->keyboard.read(0x1BF), 0x7F);
    ASSERT_EQ(s->keyboard.read(0x17F), 0x6F);
    s->keyboard.clearKeys();
    // Through the 81C55 and $E0 as the ROM scans
    m->onKey("up", true);                         // column 6 bit 1
    runInRam(m, 0xF000, "MVI A,$03\nOUT $B0\nMVI A,$BF\nOUT $B1\nMVI A,$EF\nOUT $B2\nIN $E0\nSTA $F100\nHLT\n");
    run(m, 500);
    ASSERT_EQ(m->buses[0].bus->read8(0xF100), 0xFD);
    m->onKey("up", false);
    // Typed: " is SHIFT 2 (column 4 bit 1), = is SHIFT - (column 5 bit 4)
    s->keyboard.enqueueText("\"");
    runFor(m, 0.03);
    ASSERT_EQ(s->keyboard.read(0x1EF), 0xFD);
    ASSERT_EQ(s->keyboard.read(0x0FF), 0xFE);     // SHIFT
    runFor(m, 0.2);
    s->keyboard.enqueueText("=");
    runFor(m, 0.07);      // (after the last key's 40 ms gap)
    ASSERT_EQ(s->keyboard.read(0x1DF), 0xEF);
    runFor(m, 0.2);
    ASSERT(!s->keyboard.typing());
    m->onKey("caps_lock", true); m->onKey("caps_lock", false);
    ASSERT_EQ(s->keyboard.read(0x0FF), 0xEF);
    delete m;
}

// PC-8201A cassette: the modulator makes 1200 Hz with SOD low, 2400 Hz with SOD high
TEST_CASE(pc8201_cassette_modulator) {
    for (int sod = 0; sod < 2; ++sod) {
        MachineDescriptor* m = make(Model::PC8201, romFrom(
            std::string("MVI A,$08\nOUT $90\nMVI A,") + (sod ? "$C0" : "$40") + "\nSIM\nloop_: JMP loop_\n"));
        auto* s = sysOf(m);
        ASSERT(s->tape.startTapeRecord());
        runFor(m, 0.2);
        const auto w = s->tape.recordedWav();
        const uint32_t rate = (uint32_t)w[24] | (uint32_t)w[25] << 8 | (uint32_t)w[26] << 16 | (uint32_t)w[27] << 24;
        // 0.1 s from 0.05 s in: 240 (1200 Hz) or 480 (2400 Hz) edges
        const int edges = wavEdges(w, rate / 20, rate / 10);
        const int want = sod ? 480 : 240;
        ASSERT(edges > want - 30 && edges < want + 30);
        delete m;
    }
}

// Olivetti M10: its own matrix (SHIFT at column 8 bit 2, CAPS LOCK bit 6), an AZERTY ROM
TEST_CASE(m10_keyboard_scan) {
    MachineDescriptor* m = make(Model::M10, idleRom(Model::M10));
    auto* s = sysOf(m);
    ASSERT(m->onKey("q", true));                  // column 0 bit 0
    ASSERT(m->onKey("5", true));                  // column 0 bit 7
    ASSERT(m->onKey("y", true));                  // column 1 bit 0
    ASSERT(m->onKey("m", true));                  // column 2 bit 7
    ASSERT(m->onKey("space", true));              // column 5 bit 7
    ASSERT(m->onKey("return", true));             // column 6 bit 6
    ASSERT(m->onKey("left_shift", true));         // column 8 bit 2
    ASSERT(!m->onKey("escape", true));
    ASSERT_EQ(s->keyboard.read(0x1FE), 0x7E);
    ASSERT_EQ(s->keyboard.read(0x1FD), 0xFE);
    ASSERT_EQ(s->keyboard.read(0x1FB), 0x7F);
    ASSERT_EQ(s->keyboard.read(0x1DF), 0x7F);
    ASSERT_EQ(s->keyboard.read(0x1BF), 0xBF);
    ASSERT_EQ(s->keyboard.read(0x0FF), 0xFB);
    s->keyboard.clearKeys();
    // Typed: the ROM's a is the Q key's place; 2 needs SHIFT (column 0 bit 4); + is SHIFT =
    s->keyboard.enqueueText("a");
    runFor(m, 0.03);
    ASSERT_EQ(s->keyboard.read(0x1FE), 0xFE);
    ASSERT_EQ(s->keyboard.read(0x0FF), 0xFF);
    runFor(m, 0.2);
    s->keyboard.enqueueText("2");
    runFor(m, 0.07);
    ASSERT_EQ(s->keyboard.read(0x1FE), 0xEF);
    ASSERT_EQ(s->keyboard.read(0x0FF), 0xFB);
    runFor(m, 0.2);
    s->keyboard.enqueueText("+");
    runFor(m, 0.07);
    ASSERT_EQ(s->keyboard.read(0x1FD), 0x7F);
    runFor(m, 0.2);
    m->onKey("caps_lock", true); m->onKey("caps_lock", false);
    ASSERT_EQ(s->keyboard.read(0x0FF), 0xBF);
    delete m;
}

// M10 / Kyotronic 85: the Model 100's memory map and option ROM switch; $A0 is no modem
TEST_CASE(m10_kc85_memory_map) {
    for (Model model : {Model::M10, Model::KC85}) {
        auto rom = idleRom(model);
        rom[0x0010] = 0xAA;
        std::vector<uint8_t> opt(0x8000, 0x55);
        MachineDescriptor* m = make(model, rom, 16, opt);
        IBus* bus = m->buses[0].bus;
        ASSERT_EQ(bus->read8(0x0010), 0xAA);
        bus->write8(0xC000, 0x12);
        bus->write8(0xBFFF, 0x34);
        ASSERT_EQ(bus->read8(0xC000), 0x12);
        ASSERT_EQ(bus->read8(0xBFFF), 0xFF);
        io(m)->write8(0xE0, 0x01);
        ASSERT_EQ(bus->read8(0x0010), 0x55);
        io(m)->write8(0xE0, 0x00);
        ASSERT_EQ(bus->read8(0x0010), 0xAA);
        delete m;
    }
    // The Kyotronic 85 has the Model 100's keyboard
    MachineDescriptor* kc = make(Model::KC85, idleRom(Model::KC85));
    ASSERT(kc->onKey("z", true));
    ASSERT_EQ(sysOf(kc)->keyboard.read(0x1FE), 0xFE);
    delete kc;
}

// A PC-8201A snapshot keeps its bank and port $90
TEST_CASE(pc8201_snapshot) {
    MachineDescriptor* m = make(Model::PC8201, idleRom(Model::PC8201));
    auto* s = sysOf(m);
    io(m)->write8(0xA0, 0x0A);
    io(m)->write8(0x90, 0x40);
    m->buses[0].bus->write8(0x8123, 0x99);
    std::vector<uint8_t> st(s->stateSize());
    s->saveState(st.data());
    io(m)->write8(0xA0, 0x00);
    io(m)->write8(0x90, 0x00);
    s->loadState(st.data());
    ASSERT_EQ(s->bankRegister(), 0x0A);
    ASSERT_EQ(s->scpPort(), 0x40);
    ASSERT_EQ(m->buses[0].bus->read8(0x8123), 0x99);
    delete m;
}
