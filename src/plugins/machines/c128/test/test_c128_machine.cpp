#include "test_harness.h"
#include "plugins/machines/c128/main/machine_c128.h"
#include "plugins/devices/vdc8563/main/vdc8563.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libmem/main/ibus.h"
#include <cstdio>
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
    run(m, 6000000);
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
    EXPECT_EQ(bus->peek8(0xD505) & 0xC1, 0x81);    // 40/80 up, 8502
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
