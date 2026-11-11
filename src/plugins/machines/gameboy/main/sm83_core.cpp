// The SM83 as an ICore over gb::Gameboy (see sm83_core.h).

#include "sm83_core.h"
#include "sm83_disasm.h"
#include "libdebug/main/execution_observer.h"

#include <cstdio>

namespace gb {

// ---------------------------------------------------------------------------
// The bus
// ---------------------------------------------------------------------------

uint8_t GbBus::peek8(uint32_t addr) {
    const uint16_t a = (uint16_t)addr;
    if (a >= 0x8000 && a < 0xA000) return m_gb->ppu().vram()[(m_gb->ppu().vramBank() << 13) | (a & 0x1FFF)];
    if (a >= 0xFE00 && a < 0xFEA0) return m_gb->ppu().oam()[a & 0xFF];
    return m_gb->peek(a);
}

void GbBus::write8(uint32_t addr, uint8_t v) {
    const uint16_t a = (uint16_t)addr;
    if (m_observer) m_observer->onMemoryWrite(this, a, peek8(a), v);
    if (a >= 0x8000 && a < 0xA000) m_gb->ppu().vram()[(m_gb->ppu().vramBank() << 13) | (a & 0x1FFF)] = v;   // whatever the PPU's mode
    else if (a >= 0xFE00 && a < 0xFEA0) m_gb->ppu().oam()[a & 0xFF] = v;
    else m_gb->write(a, v);
}

void GbBus::cpuRead(uint16_t addr, uint8_t v) {
    if (m_observer) m_observer->onMemoryRead(this, addr, v);
}

void GbBus::cpuWrite(uint16_t addr, uint8_t before, uint8_t v) {
    if (m_observer) m_observer->onMemoryWrite(this, addr, before, v);
}

// ---------------------------------------------------------------------------
// The registers
// ---------------------------------------------------------------------------

namespace {

const RegDescriptor REGS[Sm83Core::R_COUNT] = {
    {"A",   RegWidth::R8,  REGFLAG_ACCUMULATOR, nullptr, nullptr},
    {"F",   RegWidth::R8,  REGFLAG_STATUS,      nullptr, "ZNHC----"},
    {"B",   RegWidth::R8,  0, nullptr, nullptr},
    {"C",   RegWidth::R8,  0, nullptr, nullptr},
    {"D",   RegWidth::R8,  0, nullptr, nullptr},
    {"E",   RegWidth::R8,  0, nullptr, nullptr},
    {"H",   RegWidth::R8,  0, nullptr, nullptr},
    {"L",   RegWidth::R8,  0, nullptr, nullptr},
    {"SP",  RegWidth::R16, REGFLAG_SP, nullptr, nullptr},
    {"PC",  RegWidth::R16, REGFLAG_PC, nullptr, nullptr},
    {"AF",  RegWidth::R16, REGFLAG_INTERNAL, nullptr, nullptr},
    {"BC",  RegWidth::R16, REGFLAG_INTERNAL, nullptr, nullptr},
    {"DE",  RegWidth::R16, REGFLAG_INTERNAL, nullptr, nullptr},
    {"HL",  RegWidth::R16, REGFLAG_INTERNAL, nullptr, nullptr},
    {"IME", RegWidth::R8,  REGFLAG_INTERNAL, nullptr, nullptr},
};

uint16_t pair(uint8_t hi, uint8_t lo) { return (uint16_t)(hi << 8 | lo); }

}  // namespace

Sm83Core::Sm83Core(Gameboy* gb, GbBus* bus) : m_gb(gb), m_bus(bus) {
    m_gb->cpu().setSplitDispatch(true);
}

const RegDescriptor* Sm83Core::regDescriptor(int idx) const {
    return idx >= 0 && idx < R_COUNT ? &REGS[idx] : nullptr;
}

uint32_t Sm83Core::regRead(int idx) const {
    const auto& s = m_gb->cpu().state();
    switch (idx) {
    case R_A: return s.a;
    case R_F: return s.f;
    case R_B: return s.b;
    case R_C: return s.c;
    case R_D: return s.d;
    case R_E: return s.e;
    case R_H: return s.h;
    case R_L: return s.l;
    case R_SP: return s.sp;
    case R_PC: return s.pc;
    case R_AF: return pair(s.a, s.f);
    case R_BC: return pair(s.b, s.c);
    case R_DE: return pair(s.d, s.e);
    case R_HL: return pair(s.h, s.l);
    case R_IME: return s.ime ? 1 : 0;
    default: return 0;
    }
}

void Sm83Core::regWrite(int idx, uint32_t val) {
    auto& s = m_gb->cpu().state();
    const uint8_t v = (uint8_t)val;
    switch (idx) {
    case R_A: s.a = v; break;
    case R_F: s.f = v & 0xF0; break;              // the low nibble always reads 0
    case R_B: s.b = v; break;
    case R_C: s.c = v; break;
    case R_D: s.d = v; break;
    case R_E: s.e = v; break;
    case R_H: s.h = v; break;
    case R_L: s.l = v; break;
    case R_SP: s.sp = (uint16_t)val; break;
    case R_PC: setPc(val); break;
    case R_AF: s.a = (uint8_t)(val >> 8); s.f = v & 0xF0; break;
    case R_BC: s.b = (uint8_t)(val >> 8); s.c = v; break;
    case R_DE: s.d = (uint8_t)(val >> 8); s.e = v; break;
    case R_HL: s.h = (uint8_t)(val >> 8); s.l = v; break;
    case R_IME: s.ime = val != 0; s.eiPending = false; break;
    }
}

void Sm83Core::setPc(uint32_t addr) {
    auto& s = m_gb->cpu().state();
    s.pc = (uint16_t)addr;
    // Running from somewhere else: no longer halted or stopped
    s.haltCounter = 0;
    s.haltBug = false;
    s.stopped = false;
}

void Sm83Core::saveState(uint8_t* b) const {
    const auto& s = m_gb->cpu().state();
    b[0] = s.a; b[1] = s.f; b[2] = s.b; b[3] = s.c; b[4] = s.d; b[5] = s.e; b[6] = s.h; b[7] = s.l;
    b[8] = (uint8_t)s.sp; b[9] = (uint8_t)(s.sp >> 8);
    b[10] = (uint8_t)s.pc; b[11] = (uint8_t)(s.pc >> 8);
    b[12] = (uint8_t)((s.ime ? 1 : 0) | (s.eiPending ? 2 : 0));
}

void Sm83Core::loadState(const uint8_t* b) {
    auto& s = m_gb->cpu().state();
    s.a = b[0]; s.f = b[1] & 0xF0; s.b = b[2]; s.c = b[3]; s.d = b[4]; s.e = b[5]; s.h = b[6]; s.l = b[7];
    s.sp = (uint16_t)(b[8] | b[9] << 8);
    s.pc = (uint16_t)(b[10] | b[11] << 8);
    s.ime = (b[12] & 1) != 0;
    s.eiPending = (b[12] & 2) != 0;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

int Sm83Core::step() {
    auto& cpu = m_gb->cpu();
    auto& s = cpu.state();
    const uint64_t start = cpu.cycles();
    m_gb->setAccessHook(m_bus->getObserver() ? m_bus : nullptr);

    // An interrupt's dispatch is a step of its own (exec() returns after it)
    if (!cpu.irqDispatchPending()) {
        // (halted: an interrupt requested and enabled wakes it to run the next one, IME or not)
        const bool runsOp = !s.stopped && (!cpu.halted() || s.haltBug || (s.haltCounter == 1 && m_gb->pendingIrq() != 0));
        if (m_observer && runsOp) {
            if (m_observer->needsDisasm()) {
                DisasmEntry entry;
                decodeEntry(m_bus, s.pc, &entry);
                if (!m_observer->onStep(this, m_bus, entry)) return 1;   // a breakpoint: not run
            } else {
                m_observer->onStepLite(this, s.pc);
            }
        }
    }
    cpu.exec();
    return (int)(cpu.cycles() - start);
}

int Sm83Core::disassembleOne(IBus* bus, uint32_t addr, char* buf, int bufsz) {
    DisasmEntry e;
    const int n = sm83Disassemble(bus, addr, e);
    if (buf && bufsz > 0) std::snprintf(buf, (size_t)bufsz, "%s", e.complete.c_str());
    return n;
}

int Sm83Core::disassembleEntry(IBus* bus, uint32_t addr, void* entryOut) {
    return sm83Disassemble(bus, addr, *static_cast<DisasmEntry*>(entryOut));
}

int Sm83Core::decodeEntry(IBus* bus, uint32_t addr, void* entryOut) {
    return sm83Disassemble(bus, addr, *static_cast<DisasmEntry*>(entryOut), nullptr, false);
}

int Sm83Core::isCallAt(IBus* bus, uint32_t addr) {
    DisasmEntry e;
    const int n = sm83Disassemble(bus, addr, e, nullptr, false);
    return e.isCall ? n : 0;
}

bool Sm83Core::isReturnAt(IBus* bus, uint32_t addr) {
    DisasmEntry e;
    sm83Disassemble(bus, addr, e, nullptr, false);
    return e.isReturn;
}

bool Sm83Core::isProgramEnd(IBus*) {
    // (haltCounter above 1: the CGB switching speed, not locked up)
    const auto& s = m_gb->cpu().state();
    return s.haltCounter == 1 && !s.stopped && (m_gb->peek(0xFFFF) & 0x1F) == 0;
}

int Sm83Core::writeCallHarness(IBus* bus, uint32_t scratch, uint32_t routineAddr) {
    bus->write8(scratch, 0xCD);                       // CALL routine
    bus->write8(scratch + 1, routineAddr & 0xFF);
    bus->write8(scratch + 2, (routineAddr >> 8) & 0xFF);
    bus->write8(scratch + 3, 0x18);                   // JR *
    bus->write8(scratch + 4, 0xFE);
    return 5;
}

}  // namespace gb
