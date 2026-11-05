#include "test_harness.h"
#include "plugins/machines/plus4/main/machine_plus4.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libmem/main/ibus.h"
#include <cstdio>
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
