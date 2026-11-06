#include "test_harness.h"
#include "plugins/machines/plus4/main/machine_plus4.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/device_info.h"
#include "libmem/main/ibus.h"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <string>
#include <vector>

namespace {
// Screen codes of a text at the Plus/4's screen ($0C00, 40 x 25)
bool screenHas(MachineDescriptor* m, const std::string& text) {
    IBus* bus = m->buses[0].bus;
    std::vector<uint8_t> want;
    for (char c : text) want.push_back((uint8_t)(c >= 'A' && c <= 'Z' ? c - 'A' + 1 : c));
    for (uint32_t a = 0x0C00; a + want.size() <= 0x0C00 + 1000; ++a) {
        bool ok = true;
        for (size_t i = 0; i < want.size() && ok; ++i) ok = bus->peek8(a + (uint32_t)i) == want[i];
        if (ok) return true;
    }
    return false;
}
void run(MachineDescriptor* m, uint64_t ticks) {
    for (uint64_t t = 0; t < ticks;) t += (uint64_t)m->schedulerStep(*m);
}
IKeyboardMatrix* keyboard(MachineDescriptor* m) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if (auto* k = dynamic_cast<IKeyboardMatrix*>(h)) return k;
    return nullptr;
}
}

// The Plus/4 boots BASIC 3.5 to READY.
TEST_CASE(plus4_boots_to_ready) {
    std::string err;
    MachineDescriptor* m = plus4::create(plus4::Model::Plus4, false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    m->onReset(*m);
    run(m, 6000000);                                // ~3.4 s of TED clock
    ASSERT(screenHas(m, "COMMODORE BASIC V3.5"));
    ASSERT(screenHas(m, "READY."));
    delete m;
}

// The C16 boots with 16 KB and runs a typed line
TEST_CASE(c16_boots_and_runs_basic) {
    std::string err;
    MachineDescriptor* m = plus4::create(plus4::Model::C16, false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    m->onReset(*m);
    run(m, 6000000);
    ASSERT(screenHas(m, "READY."));
    ASSERT(screenHas(m, "12277 BYTES FREE"));       // C16: 16 KB
    keyboard(m)->enqueueText("PRINT 6*7\n");
    run(m, 4000000);
    ASSERT(screenHas(m, " 42"));
    delete m;
}

// SAVE, LOAD and FILE NOT FOUND through the KERNAL's serial bus code to the
// IEC drive on unit 8 (a host folder)
TEST_CASE(plus4_iec_drive_save_load) {
    std::string err;
    MachineDescriptor* m = plus4::create(plus4::Model::C16, false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    char dir[] = "/tmp/mmemu-iec-XXXXXX";
    ASSERT(mkdtemp(dir) != nullptr);
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    bool mounted = false;
    for (auto* h : hs) mounted = mounted || h->mountDisk(8, dir);
    ASSERT(mounted);

    m->onReset(*m);
    run(m, 6000000);
    keyboard(m)->enqueueText("10 PRINT 40+3\nSAVE\"T\",8\n");
    run(m, 12000000);
    std::string path = std::string(dir) + "/T";
    FILE* f = fopen(path.c_str(), "rb");
    ASSERT(f != nullptr);
    uint8_t hdr[2] = {0, 0};
    ASSERT(fread(hdr, 1, 2, f) == 2);
    fclose(f);
    ASSERT(hdr[0] == 0x01 && hdr[1] == 0x10);       // BASIC at $1001

    keyboard(m)->enqueueText("NEW\nLOAD\"T\",8\nRUN\n");
    run(m, 16000000);
    ASSERT(screenHas(m, " 43"));

    keyboard(m)->enqueueText("LOAD\"NOPE\",8\n");
    run(m, 10000000);
    ASSERT(screenHas(m, "FILE NOT FOUND"));

    remove(path.c_str());
    rmdir(dir);
    delete m;
}

// The cassette port: the sense switch on $FD10 bit 2 and the motor on $01 bit 3
TEST_CASE(plus4_tape_port_lines) {
    std::string err;
    MachineDescriptor* m = plus4::create(plus4::Model::Plus4, false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    m->onReset(*m);
    IBus* bus = m->buses[0].bus;
    IOHandler* tape = m->ioRegistry->findHandler("Tape");
    ASSERT(tape != nullptr);
    bus->write8(0xFD10, 0xFF);
    ASSERT(bus->read8(0xFD10) & 0x04);                       // no button down
    tape->controlTape("play");
    ASSERT(!(bus->read8(0xFD10) & 0x04));                    // PLAY: sense low
    auto motorOn = [&]() {
        DeviceInfo info;
        tape->getDeviceInfo(info);
        for (auto& kv : info.state) if (kv.first == "Motor On") return kv.second == "yes";
        return false;
    };
    bus->write8(0x00, 0x0F);
    bus->write8(0x01, 0x08);                                 // bit 3 high: motor off
    tape->tick(1);
    ASSERT(!motorOn());
    bus->write8(0x01, 0x00);                                 // low: motor on
    tape->tick(1);
    ASSERT(motorOn());
    delete m;
}

// RS-232 through the KERNAL: the 6551 at $FD00, looped back (the C16 has none)
TEST_CASE(plus4_acia_kernal_loopback) {
    std::string err;
    MachineDescriptor* m = plus4::create(plus4::Model::Plus4, false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    IOHandler* acia = m->ioRegistry->findHandler("ACIA");
    ASSERT(acia != nullptr);
    ASSERT(acia->attachSerial("loopback"));
    m->onReset(*m);
    run(m, 6000000);
    // 1200 bps 8N1, DTR and transmit IRQs (the KERNAL sends from its IRQ)
    keyboard(m)->enqueueText("10 OPEN2,2,0,CHR$(8)+CHR$(5):PRINT#2,\"HE\"+\"LLO\";\n"
                             "20 FORI=1TO300:NEXT\n"
                             "30 GET#2,A$:IFA$<>\"\"THENPRINTA$;:GOTO30\n"
                             "40 CLOSE2\nRUN\n");
    run(m, 30000000);
    ASSERT(screenHas(m, "HELLO"));
    delete m;

    m = plus4::create(plus4::Model::C16, false, &err);
    ASSERT(m != nullptr);
    ASSERT(m->ioRegistry->findHandler("ACIA") == nullptr);
    delete m;
}
