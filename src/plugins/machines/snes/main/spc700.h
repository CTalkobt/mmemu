#pragma once

#include "libcore/main/icore.h"
#include <cstdint>

/**
 * Sony SPC700 (the S-SMP of the SNES sound system, #15).
 *
 * After the SPC700 instruction set as documented by Anomie, the fullsnes
 * notes and bsnes/Mesen2 (Core/SNES/Spc.Instructions.cpp, GPL-3.0); checked
 * against Tom Harte's SingleStepTests (github.com/SingleStepTests/spc700)
 * for registers, memory and cycle counts.
 *
 * The registers A, X, Y, SP (stack in page 1), PSW (N V P B H I Z C; P
 * puts the direct page at $0100) and PC; every instruction including
 * MUL/DIV, DAA/DAS, the 16-bit YA instructions, the bit instructions on
 * 13-bit addresses, TCALL/PCALL/BRK, SLEEP and STOP. Each read8/write8 on
 * the bus is one access; step() returns the instruction's cycles (the
 * SPC700's internal cycles are not separate bus calls).
 */
class Spc700 : public ICore {
public:
    Spc700();

    enum Reg { R_A, R_X, R_Y, R_SP, R_PC, R_PSW, R_YA, R_COUNT };
    static constexpr uint8_t FC = 0x01, FZ = 0x02, FI = 0x04, FH = 0x08, FB = 0x10, FP = 0x20, FV = 0x40, FN = 0x80;

    const char* isaName()     const override { return "spc700"; }
    const char* variantName() const override { return "Sony SPC700"; }
    uint32_t    isaCaps()     const override { return 0; }

    int                  regCount()             const override { return R_COUNT; }
    const RegDescriptor* regDescriptor(int idx) const override;
    uint32_t             regRead (int idx)      const override;
    void                 regWrite(int idx, uint32_t val) override;

    uint32_t pc() const override { return m_pc; }
    void     setPc(uint32_t addr) override { m_pc = (uint16_t)addr; m_stopped = false; }
    uint32_t sp() const override { return 0x100 | m_sp; }

    int  step() override;
    /** Reset: the vector at $FFFE (the IPL ROM's start). */
    void reset() override;

    void triggerIrq()   override {}
    void triggerNmi()   override {}
    void triggerReset() override { reset(); }
    void setIrqLine(bool) override {}
    void setNmiLine(bool) override {}
    /** SLEEP or STOP: halted until reset. */
    bool isHalted() const override { return m_stopped; }

    int  disassembleOne  (IBus* bus, uint32_t addr, char* buf, int bufsz) override;
    int  disassembleEntry(IBus* bus, uint32_t addr, void* entryOut) override;
    int  isCallAt    (IBus* bus, uint32_t addr) override;
    bool isReturnAt  (IBus* bus, uint32_t addr) override;
    bool isProgramEnd(IBus* bus) override { (void)bus; return m_stopped; }
    int  writeCallHarness(IBus* bus, uint32_t scratch, uint32_t routineAddr) override;

    void setDataBus(IBus* bus) override { m_bus = bus; }
    void setCodeBus(IBus* bus) override { (void)bus; }
    void setIoBus  (IBus* bus) override { (void)bus; }
    IBus* getDataBus() const override { return m_bus; }
    IBus* getCodeBus() const override { return m_bus; }
    IBus* getIoBus()   const override { return nullptr; }
    uint64_t cycles() const override { return m_cycles; }

    size_t stateSize() const override { return 16; }
    void   saveState(uint8_t* buf) const override;
    void   loadState(const uint8_t* buf) override;

    uint8_t a() const { return m_a; }
    uint8_t x() const { return m_x; }
    uint8_t y() const { return m_y; }
    uint8_t psw() const { return m_psw; }

private:
    uint8_t rd(uint16_t a);
    void wr(uint16_t a, uint8_t v);
    uint8_t fetch() { return rd(m_pc++); }
    uint16_t fetch16() { uint16_t lo = fetch(); return (uint16_t)(lo | fetch() << 8); }
    uint16_t dp(uint8_t o) const { return (uint16_t)(((m_psw & FP) ? 0x100 : 0) | o); }
    uint16_t rdDp16(uint8_t o) { uint16_t lo = rd(dp(o)); return (uint16_t)(lo | rd(dp((uint8_t)(o + 1))) << 8); }
    void push(uint8_t v) { wr((uint16_t)(0x100 | m_sp--), v); }
    uint8_t pull() { return rd((uint16_t)(0x100 | ++m_sp)); }
    void nz(uint8_t v) { m_psw = (uint8_t)((m_psw & ~(FN | FZ)) | (v & 0x80) | (v ? 0 : FZ)); }
    void nz16(uint16_t v) { m_psw = (uint8_t)((m_psw & ~(FN | FZ)) | ((v >> 8) & 0x80) | (v ? 0 : FZ)); }
    void flag(uint8_t f, bool on) { if (on) m_psw |= f; else m_psw &= (uint8_t)~f; }

    uint8_t alu(int op, uint8_t a, uint8_t b);   // 0 OR 1 AND 2 EOR 3 CMP 4 ADC 5 SBC
    uint8_t adc(uint8_t a, uint8_t b);
    uint8_t shift(int op, uint8_t v);            // 0 ASL 1 ROL 2 LSR 3 ROR
    bool branch(bool take);
    void call(uint16_t to) { push((uint8_t)(m_pc >> 8)); push((uint8_t)m_pc); m_pc = to; }

    IBus* m_bus = nullptr;
    uint8_t m_a = 0, m_x = 0, m_y = 0, m_sp = 0xEF, m_psw = 0x02;
    uint16_t m_pc = 0xFFC0;
    bool m_stopped = false;
    int m_extra = 0;
    uint64_t m_cycles = 0;
};
