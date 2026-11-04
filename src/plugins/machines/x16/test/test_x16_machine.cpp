#include "test_harness.h"
#include "plugins/machines/x16/main/machine_x16.h"
#include "plugins/devices/vera/main/vera.h"
#include "plugins/6502/main/cpu65c02.h"
#include "libcore/main/machine_desc.h"
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
