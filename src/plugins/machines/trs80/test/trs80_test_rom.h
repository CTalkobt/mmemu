#pragma once

// Hand-assembled Z80 stand-ins for the TRS-80 ROMs (the real ones are
// copyright and not in the repository): just enough to exercise the
// hardware the way the ROMs and DOSes do.

#include "plugins/machines/trs80/main/machine_trs80.h"
#include "plugins/machines/trs80/main/trs80_system.h"
#include "libcore/main/machine_desc.h"
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace trs80test {

/** A tiny assembler: bytes at an origin, relative jumps by target. */
struct Asm {
    std::vector<uint8_t> b;
    uint16_t org, pc;
    explicit Asm(uint16_t origin = 0, size_t size = 0x3000, uint8_t fill = 0xFF)
        : b(size, fill), org(origin), pc(origin) {}
    void emit(std::initializer_list<int> v) { for (int x : v) b[(size_t)(pc++ - org)] = (uint8_t)x; }
    /** JR (0x18), JR NZ (0x20), JR Z (0x28), JR NC (0x30), JR C (0x38) to `target`. */
    void jr(int op, uint16_t target) { emit({op, (target - (pc + 2)) & 0xFF}); }
    void at(uint16_t a) { pc = a; }
};

/** A stand-in that does nothing: DI and spin. */
inline std::vector<uint8_t> idleRom(size_t size = 0x3000) {
    Asm a(0, size);
    a.emit({0xF3});                              // DI
    a.jr(0x18, a.pc);                            // JR $
    return a.b;
}

inline MachineDescriptor* make(trs80::Model model, const std::vector<uint8_t>& rom) {
    trs80::Roms roms;
    roms.rom = rom;
    std::string err;
    return trs80::create(model, &err, &roms);
}

/** Run for `cycles` device cycles (the base CPU clock). */
inline uint64_t run(MachineDescriptor* m, uint64_t cycles) {
    uint64_t t = 0;
    while (t < cycles) t += (uint64_t)m->schedulerStep(*m);
    return t;
}

/** Run for `seconds` of machine time. */
inline void runFor(MachineDescriptor* m, double seconds) {
    run(m, (uint64_t)(seconds * m->clockHz));
}

}  // namespace trs80test
