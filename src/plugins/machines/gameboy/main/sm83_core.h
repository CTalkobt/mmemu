#pragma once

// The Game Boy's SM83 as the debugger sees it: an ICore over gb::Gameboy's
// CPU (gb_cpu.h). step() runs one instruction (or an interrupt dispatch, or
// one machine cycle halted or stopped), and with it the whole Game Boy, as
// gb::Cpu::exec() does. The registers are A F B C D E H L SP PC (and, hidden,
// AF BC DE HL and IME for expressions); F's flags are Z N H C.
//
// With the debugger attached, onStep() is called before each instruction
// (not for halted cycles, or the dispatch itself: an interrupt's dispatch is
// a step of its own, so a breakpoint on a handler stops at its first
// instruction), and the CPU's data reads and writes reach the bus observer
// (the watchpoints) through gb::AccessHook.

#include "libcore/main/icore.h"
#include "libmem/main/ibus.h"
#include "gameboy.h"

namespace gb {

/**
 * The CPU's 64 KB address space for the debugger and tools: reads peek
 * (no register side effects; VRAM and OAM whatever the PPU's mode), writes
 * go to memory or the register (a write to $0000-$7FFF selects banks, as
 * the CPU's would). No clocks pass.
 */
class GbBus : public IBus, public AccessHook {
public:
    explicit GbBus(Gameboy* gb) : m_gb(gb) { m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF}; }

    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "system"; }
    uint8_t read8(uint32_t addr) override { return peek8(addr); }
    uint8_t peek8(uint32_t addr) override;
    void write8(uint32_t addr, uint8_t v) override;

    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

    // AccessHook: the CPU's accesses, to the observer
    void cpuRead(uint16_t addr, uint8_t v) override;
    void cpuWrite(uint16_t addr, uint8_t before, uint8_t v) override;

private:
    Gameboy* m_gb;
    BusConfig m_cfg;
};

class Sm83Core : public ICore {
public:
    enum Reg { R_A, R_F, R_B, R_C, R_D, R_E, R_H, R_L, R_SP, R_PC, R_AF, R_BC, R_DE, R_HL, R_IME, R_COUNT };

    /** The CPU of `gb`, its memory seen through `bus`. */
    Sm83Core(Gameboy* gb, GbBus* bus);

    const char* isaName()     const override { return "sm83"; }
    const char* variantName() const override { return m_gb->isCgb() ? "Sharp SM83 (CGB)" : "Sharp SM83 (DMG)"; }
    uint32_t    isaCaps()     const override { return 0; }

    int                  regCount()             const override { return R_COUNT; }
    const RegDescriptor* regDescriptor(int idx) const override;
    uint32_t             regRead (int idx)      const override;
    void                 regWrite(int idx, uint32_t val) override;

    uint32_t pc() const override { return m_gb->cpu().state().pc; }
    void     setPc(uint32_t addr) override;
    uint32_t sp() const override { return m_gb->cpu().state().sp; }

    int  step() override;
    /** Power on again (the Game Boy has no reset button; the cartridge's RAM is kept). */
    void reset() override { m_gb->powerOn(); }
    void triggerIrq()   override {}
    void triggerNmi()   override {}
    void triggerReset() override { reset(); }
    bool isHalted() const override { return m_gb->cpu().halted(); }

    int  disassembleOne  (IBus* bus, uint32_t addr, char* buf, int bufsz) override;
    int  disassembleEntry(IBus* bus, uint32_t addr, void* entryOut) override;
    int  decodeEntry     (IBus* bus, uint32_t addr, void* entryOut) override;
    int  isCallAt    (IBus* bus, uint32_t addr) override;
    bool isReturnAt  (IBus* bus, uint32_t addr) override;
    /** Locked up: halted with no interrupt enabled (as after an unused opcode). */
    bool isProgramEnd(IBus* bus) override;
    /** CALL routine; then JR to itself. */
    int  writeCallHarness(IBus* bus, uint32_t scratch, uint32_t routineAddr) override;

    void setDataBus(IBus*) override {}
    void setCodeBus(IBus*) override {}
    void setIoBus  (IBus*) override {}
    IBus* getDataBus() const override { return m_bus; }
    IBus* getCodeBus() const override { return m_bus; }
    IBus* getIoBus()   const override { return nullptr; }
    uint64_t cycles() const override { return m_gb->cycles(); }

    /** The registers (A F B C D E H L, SP, PC, IME): for the trace and snapshots. */
    size_t stateSize() const override { return 13; }
    void   saveState(uint8_t* buf) const override;
    void   loadState(const uint8_t* buf) override;

    Gameboy* gameboy() const { return m_gb; }

private:
    Gameboy* m_gb;
    GbBus* m_bus;
};

}  // namespace gb
