// The Model 100/200 add-ons (issue #358): the built-in modem, the bar code
// wand, the RAMPAC and REX, driven by hand-built 8085 stand-ins.

#include "test_harness.h"
#include "m100_test_rom.h"
#include "plugins/8085/main/cpu8085.h"
#include "libdevices/main/io_registry.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace m100test;
using m100::Model;

namespace {

m100::System* sysOf(MachineDescriptor* m) { return m100::systemOf(m); }

IOHandler* handler(MachineDescriptor* m, const std::string& name) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs)
        if (name == h->name()) return h;
    return nullptr;
}

/** Offer `spec` to the handlers as the CLI's `serial` does: the first that takes it. */
std::string serial(MachineDescriptor* m, const std::string& spec) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs)
        if (h->attachSerial(spec)) return h->name();
    return "";
}

std::string tempFile(const std::string& tag) {
    return (std::filesystem::temp_directory_path() / ("mmsim_m100_" + tag + "_" + std::to_string(::getpid()) + ".bin"))
        .string();
}

std::string timerSetup(int count) {
    char b[160];
    std::snprintf(b, sizeof b, "MVI A,$%02X\nOUT $B4\nMVI A,$%02X\nOUT $B5\nMVI A,$C3\nOUT $B0\n", count & 0xFF,
                  0x40 | ((count >> 8) & 0x3F));
    return b;
}

const std::string RX_HANDLER =
    "rx_: PUSH PSW\nPUSH H\nIN $C0\nLHLD $F0F0\nMOV M,A\nINX H\nSHLD $F0F0\nPOP H\nPOP PSW\nEI\nRET\n";

}  // namespace

// The Model 100's modem, as TELCOM originates: port B bit 3 (the UART to the modem) with bit 7 low (the
// line held), $A0 bit 0 (the relay); the far end's tone on $D0 bit 0; then $A0 bit 1 (our carrier on).
// A byte goes out on the phone line (a loopback) and comes back
TEST_CASE(m100_modem_loopback) {
    const std::string src =
        "JMP start_\nORG 0034\nJMP rx_\nORG 0040\n"
        "start_: LXI SP,$0000\nLXI H,$F100\nSHLD $F0F0\n" + timerSetup(512) +   // 300 baud
        "MVI A,$1C\nOUT $D0\n"
        "MVI A,$08\nOUT $B2\n"              // port B: the modem, the line held
        "IN $B3\nSTA $F0F2\n"               // ORIG/ANS
        "MVI A,$01\nOUT $A0\n"              // the relay
        "cd_: IN $D0\nANI $01\nJZ cd_\n"    // the far end's tone
        "MVI A,$03\nOUT $A0\n"              // our carrier on
        "MVI A,$0D\nSIM\nEI\n"
        "MVI A,$4D\nOUT $C0\n"
        "idle_: HLT\nJMP idle_\n" + RX_HANDLER;
    MachineDescriptor* m = make(Model::M100, romFrom(src));
    ASSERT_EQ(serial(m, "loopback"), std::string("RS-232"));
    ASSERT_EQ(serial(m, "modem:loopback"), std::string("Modem"));
    auto* s = sysOf(m);
    runFor(m, 0.05);
    ASSERT(!s->modem.carrier());            // the carrier-detect delay
    runFor(m, 0.25);
    ASSERT(s->modem.enabled() && s->modem.offHook() && s->modem.carrier());
    ASSERT(s->modemSelected());
    ASSERT_EQ(m->buses[0].bus->read8(0xF0F2) & 0x10, 0);     // the switch on ORIG
    ASSERT_EQ(s->modem.sentToLine.size(), 1u);
    ASSERT_EQ(s->modem.sentToLine[0], 0x4D);
    ASSERT_EQ(m->buses[0].bus->read8(0xF100), 0x4D);
    ASSERT_EQ(m->buses[0].bus->read8(0xF0F0), 0x01);
    // The modem's state on the debugger
    DeviceInfo info;
    handler(m, "Modem")->getDeviceInfo(info);
    ASSERT(!info.state.empty());
    delete m;
}

// The line is held only with the modem selected, port B bit 7 low and the relay on; the far tone toggles
// $D0 bit 0; the ORIG/ANS switch reads on port C bit 4 while the modem is selected; the relay off drops it
TEST_CASE(m100_modem_hook_and_switch) {
    MachineDescriptor* m = make(Model::M100, idleRom(Model::M100));
    auto* s = sysOf(m);
    IBus* io = m->buses[1].bus;
    ASSERT_EQ(serial(m, "modem:loopback"), std::string("Modem"));
    io->write8(0xB0, 0x03);                 // the 81C55's ports A and B out
    io->write8(0xB2, 0x00);
    io->write8(0xA0, 0x01);
    runFor(m, 0.2);
    ASSERT(!s->modem.offHook());            // RS-232 selected
    io->write8(0xB2, 0x88);
    runFor(m, 0.2);
    ASSERT(!s->modem.offHook());            // bit 7 high: the line open
    ASSERT_EQ(io->read8(0xB3) & 0x10, 0);   // ORIG
    io->write8(0xB2, 0x08);
    runFor(m, 0.2);
    ASSERT(s->modem.offHook() && s->modem.carrier());
    int ones = 0;
    for (int i = 0; i < 100; ++i) { ones += io->read8(0xD0) & 1; run(m, 100); }
    ASSERT(ones > 20 && ones < 80);         // a tone, not a level
    ASSERT_EQ(serial(m, "modem:answer"), std::string("Modem"));
    ASSERT_EQ(io->read8(0xB3) & 0x10, 0x10);
    ASSERT(!s->modem.originate());
    io->write8(0xA0, 0x00);                 // the relay off: on hook
    ASSERT(!s->modem.carrier());
    ASSERT_EQ(io->read8(0xD0) & 0x01, 0);
    // A Model 102 has no modem
    delete m;
    m = make(Model::M102, idleRom(Model::M102));
    ASSERT(serial(m, "modem:loopback") != "Modem");
    ASSERT(handler(m, "Modem") == nullptr);
    delete m;
}

// TELCOM pulse-dials with port B bit 7 (1 opens the line; ROM $540A): the digits are decoded
TEST_CASE(m100_modem_pulse_dial) {
    // Dial "21": 60 ms breaks, 40 ms makes (10 pps), 700 ms between digits, timed by a delay loop
    const std::string src =
        "JMP start_\nORG 0040\n"
        "start_: LXI SP,$0000\nMVI A,$03\nOUT $B0\nMVI A,$08\nOUT $B2\nMVI A,$01\nOUT $A0\n"
        "MVI B,25\nCALL wait_\n"            // 500 ms off hook
        "MVI C,2\nCALL digit_\nMVI B,35\nCALL wait_\n"
        "MVI C,1\nCALL digit_\nMVI B,35\nCALL wait_\n"
        "idle_: HLT\nJMP idle_\n"
        "digit_: MVI A,$88\nOUT $B2\nMVI B,3\nCALL wait_\nMVI A,$08\nOUT $B2\nMVI B,2\nCALL wait_\nDCR C\nJNZ digit_\nRET\n"
        // B x 20 ms: 2.4576 MHz / 20 ms = 49152 clocks; the inner loop (DCX D 6, MOV A,D 4, ORA E 4, JNZ 10) 24 each
        "wait_: LXI D,2048\nw2_: DCX D\nMOV A,D\nORA E\nJNZ w2_\nDCR B\nJNZ wait_\nRET\n";
    MachineDescriptor* m = make(Model::M100, romFrom(src));
    ASSERT_EQ(serial(m, "modem:loopback"), std::string("Modem"));
    runFor(m, 2.5);
    ASSERT_EQ(sysOf(m)->modem.dialled(), std::string("21"));
    delete m;
}

// The Tandy 200's modem: port B 1 ORIG/ANS, 3 select, 6 MEN, 7 CALL; the far tone on port C bit 4; the 8251's
// byte goes to the line
TEST_CASE(m100_t200_modem) {
    const std::string src =
        "JMP start_\nORG 0034\nJMP rx_\nORG 0040\n"
        "start_: LXI SP,$0000\nLXI H,$F100\nSHLD $F0F0\n" + timerSetup(512) +
        "MVI A,$4E\nOUT $C1\nMVI A,$37\nOUT $C1\n"
        "MVI A,$CA\nOUT $B2\n"              // CALL, MEN, modem, originate
        "cd_: IN $B3\nANI $10\nJZ cd_\n"
        "MVI A,$0D\nSIM\nEI\n"
        "MVI A,$54\nOUT $C0\n"
        "idle_: HLT\nJMP idle_\n" + RX_HANDLER;
    MachineDescriptor* m = make(Model::M200, romFrom(src, 0x12000));
    ASSERT_EQ(serial(m, "modem:loopback"), std::string("Modem"));
    runFor(m, 0.3);
    auto* s = sysOf(m);
    ASSERT(s->modem.originate() && s->modem.offHook() && s->modem.carrier());
    ASSERT_EQ(s->modem.sentToLine.size(), 1u);
    ASSERT_EQ(m->buses[0].bus->read8(0xF100), 0x54);
    delete m;
}

// The bar code wand: Code 39 "A" swiped; RST 5.5 counts the bars (the handler waits for the space) and a
// poll of port C bit 3 counts them too: 15 bars in *A*
TEST_CASE(m100_barcode_rst55) {
    const std::string src =
        "JMP start_\nORG 002C\nJMP bar_\nORG 0040\n"
        "start_: LXI SP,$0000\nXRA A\nSTA $F100\n" + timerSetup(8) +
        "MVI A,$0E\nSIM\nEI\n"              // RST 5.5 unmasked
        "idle_: HLT\nJMP idle_\n"
        "bar_: PUSH PSW\nLDA $F100\nINR A\nSTA $F100\n"
        "sp_: IN $B3\nANI $08\nJNZ sp_\n"   // BCR (port C bit 3, as RST 5.5): wait for the space
        "POP PSW\nEI\nRET\n";
    MachineDescriptor* m = make(Model::M100, romFrom(src));
    auto* s = sysOf(m);
    runFor(m, 0.01);
    ASSERT_EQ(m->buses[1].bus->read8(0xB3) & 0x08, 0);     // no wand: 0
    ASSERT_EQ(serial(m, "barcode:code39:A"), std::string("Bar code reader"));
    ASSERT(s->wandPlugged && s->wand.scanning());
    ASSERT_EQ(m->buses[1].bus->read8(0xB3) & 0x08, 0);     // over the quiet zone: light, BCR low
    runFor(m, 0.1);
    ASSERT(!s->wand.scanning());
    ASSERT_EQ(m->buses[0].bus->read8(0xF100), 15);
    ASSERT_EQ(serial(m, "barcode:none"), std::string("Bar code reader"));
    ASSERT(!s->wandPlugged);
    ASSERT(serial(m, "barcode:code39:#") != "Bar code reader");
    delete m;
}

// The RAMPAC on the system bus: a sector written and read back by an 8085 program; persisted to its file
TEST_CASE(m100_rampac) {
    const std::string path = tempFile("rampac");
    std::filesystem::remove(path);
    const std::string src =
        "JMP start_\nORG 0040\n"
        "start_: LXI SP,$0000\n"
        "MVI A,7\nOUT $81\nMVI B,0\n"
        "wr_: MOV A,B\nOUT $83\nINR B\nJNZ wr_\n"   // 256 bytes 0..255 into sector 7
        "MVI A,7\nOUT $81\nIN $83\nSTA $F100\nIN $83\nSTA $F101\nIN $81\nSTA $F102\nIN $83\nSTA $F103\n"
        "idle_: HLT\nJMP idle_\n";
    MachineDescriptor* m = make(Model::M100, romFrom(src));
    auto* slot = m->expansionSlot("sysbus");
    ASSERT(slot != nullptr);
    std::string err;
    ASSERT(slot->install("rampac", path, err));
    m->onReset(*m);
    runFor(m, 0.05);
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->read8(0xF100), 0);
    ASSERT_EQ(bus->read8(0xF101), 1);
    ASSERT_EQ(bus->read8(0xF102), 0xFF);       // IN $81: a select, nothing on the bus
    ASSERT_EQ(bus->read8(0xF103), 0);          // ... and the counter back at byte 0
    ASSERT_EQ(sysOf(m)->rampac()->ram()[7 * 1024 + 200], 200);
    ASSERT(slot->info().find("rampac") == 0);
    // A snapshot carries it
    std::vector<uint8_t> snap(bus->stateSize());
    bus->saveState(snap.data());
    sysOf(m)->rampac()->ram()[7 * 1024 + 200] = 0;
    bus->loadState(snap.data());
    ASSERT_EQ(sysOf(m)->rampac()->ram()[7 * 1024 + 200], 200);
    ASSERT(slot->install("", "", err));     // removed: written to the file
    ASSERT(sysOf(m)->rampac() == nullptr);
    ASSERT_EQ(m->buses[1].bus->read8(0x83), 0xFF);
    {
        std::ifstream f(path, std::ios::binary);
        f.seekg(7 * 1024 + 200);
        ASSERT_EQ(f.get(), 200);
    }
    delete m;
    // Back in another machine: the data is there
    m = make(Model::M200, idleRom(Model::M200));
    ASSERT(m->expansionSlot("sysbus")->install("datapac", path, err));
    m->buses[1].bus->write8(0x81, 7);
    m->buses[1].bus->read8(0x83);
    ASSERT_EQ(m->buses[1].bus->read8(0x83), 1);
    delete m;
    std::filesystem::remove(path);
}

// REX in the Model 100's option socket, driven from RAM: the key, the ID, a byte programmed and read back,
// the image written to its file
TEST_CASE(m100_rex_program) {
    const std::string path = tempFile("rex");
    std::filesystem::remove(path);
    MachineDescriptor* m = make(Model::M100, idleRom(Model::M100));
    std::string err;
    ASSERT(m->expansionSlot("rex")->install("rex", path, err));
    auto* s = sysOf(m);
    s->rex()->flash().data()[0x0100] = 0xC9;
    runInRam(m, 0xC000,
             "DI\nLXI SP,$0000\nMVI A,$01\nOUT $E0\n"   // the option socket in
             "LDA $0100\nSTA $F100\n"                   // reading mode
             "LDA $00B8\nLDA $01F2\nLDA $0234\nLDA $03B0\nLDA $0431\nLDA $05BF\n"   // the key
             "LDA $0007\nSTA $F101\nLDA $0000\n"        // ID
             "LDA $0001\nLDA $0002\n"                   // sector 2
             "LDA $00AA\nLDA $00AA\nLDA $0055\nLDA $0000\n"    // AA@AA, 55@55
             "LDA $00AA\nLDA $00A0\n"                   // A0@AA: program
             "LDA $0006\nLDA $0345\nLDA $003C\n"        // $3C at $0345
             "LDA $0000\n"                              // back to reading
             "LDA $0345\nSTA $F102\n"
             "XRA A\nOUT $E0\nHLT\n");
    runFor(m, 0.01);
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->read8(0xF100), 0xC9);
    ASSERT_EQ(bus->read8(0xF101), 0x11);
    ASSERT_EQ(s->rex()->flash().data()[0x10345], 0x3C);
    ASSERT_EQ(bus->read8(0xF102), 0x3C);
    ASSERT_EQ(s->rex()->state(), 7);
    // The debugger's view does not step the state machine
    m->buses[1].bus->write8(0xE0, 0x01);
    for (int i = 0; i < 6; ++i) bus->peek8(0x00B8);
    ASSERT_EQ(s->rex()->state(), 7);
    ASSERT_EQ(bus->peek8(0x0345), 0x3C);
    m->buses[1].bus->write8(0xE0, 0x00);
    ASSERT(m->expansionSlot("rex")->install("", "", err));    // removed: flushed
    delete m;
    std::ifstream f(path, std::ios::binary);
    f.seekg(0x10345);
    ASSERT_EQ(f.get(), 0x3C);
    f.seekg(0x0100);
    ASSERT_EQ(f.get(), 0xC9);
    f.close();
    std::filesystem::remove(path);
}

// REX# in the Tandy 200's ROM bank 2 (the data before the address)
TEST_CASE(m100_t200_rexsharp) {
    MachineDescriptor* m = make(Model::M200, idleRom(Model::M200));
    std::string err;
    ASSERT(m->expansionSlot("rex")->install("rexsharp", "", err));
    auto* s = sysOf(m);
    runInRam(m, 0xC000,
             "DI\nLXI SP,$0000\nMVI A,$02\nOUT $D0\n"
             "LDA $00B8\nLDA $01F2\nLDA $0234\nLDA $03B0\nLDA $0431\nLDA $05BF\n"
             "LDA $0007\nSTA $F101\nLDA $0000\n"
             "LDA $00AA\nLDA $00AA\nLDA $00AA\nLDA $0055\nLDA $0055\nLDA $0055\n"
             "LDA $00AA\nLDA $00A0\nLDA $00AA\n"
             "LDA $0006\nLDA $0081\nLDA $0123\n"        // $81 at $0123
             "LDA $0000\nLDA $0123\nSTA $F102\n"
             "XRA A\nOUT $D0\nHLT\n");
    runFor(m, 0.01);
    ASSERT_EQ(m->buses[0].bus->read8(0xF101), 0x13);
    ASSERT_EQ(s->rex()->flash().data()[0x0123], 0x81);
    ASSERT_EQ(m->buses[0].bus->read8(0xF102), 0x81);
    delete m;
}
