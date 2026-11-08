#pragma once

#include "libcore/main/icore.h"
#include <cstdint>

/**
 * Acorn ARM1 / ARM2 CPU core (#13, the BBC Micro's ARM Evaluation System
 * second processor): the 26-bit architecture of the ARM2 datasheet (VLSI
 * VL86C010) and the "Acorn RISC Machine" ARM1 manual.
 *
 * R15 holds the PC (bits 2-25) and the PSR: N Z C V (31-28), I F (27-26),
 * the mode (1-0: user, FIQ, IRQ, supervisor). Instructions: data
 * processing with the barrel shifter (immediate and register shifts, RRX,
 * the shifter's carry), the P forms (TEQP etc.) setting the PSR, MUL/MLA
 * (ARM2 only; undefined on the ARM1), LDR/STR (byte and word, pre/post
 * index, writeback, rotated unaligned loads), LDM/STM (with ^ for the user
 * bank or the PSR), B/BL, SWI, and the undefined-instruction trap for
 * coprocessor instructions (none fitted). Banked R8-R14 (FIQ) and R13-R14
 * (IRQ, supervisor). Exceptions: reset ($00), undefined ($04), SWI ($08),
 * address exception ($14, a data address above 64 MB), IRQ ($18), FIQ
 * ($1C). The pipeline: R15 reads as the instruction's address + 8 (+12
 * as a register-shift operand or STR's source).
 *
 * Memory is byte-addressed through the data bus (32-bit words little-
 * endian, from four byte accesses). step() returns cycles (sequential,
 * non-sequential and internal counted alike).
 */
class Arm2 : public ICore {
public:
    explicit Arm2(bool arm1 = false);

    enum Reg { R_R0, R_R15 = 15, R_PC = 16, R_PSR, R_COUNT };
    static constexpr uint32_t FLAG_N = 1u << 31, FLAG_Z = 1u << 30, FLAG_C = 1u << 29, FLAG_V = 1u << 28;
    static constexpr uint32_t FLAG_I = 1u << 27, FLAG_F = 1u << 26;
    static constexpr uint32_t PC_MASK = 0x03FFFFFC;
    enum Mode { USR = 0, FIQ = 1, IRQ = 2, SVC = 3 };

    const char* isaName()     const override { return "arm"; }
    const char* variantName() const override { return m_arm1 ? "Acorn ARM1" : "Acorn ARM2"; }
    uint32_t    isaCaps()     const override { return 0; }

    int                  regCount()             const override { return R_COUNT; }
    const RegDescriptor* regDescriptor(int idx) const override;
    uint32_t             regRead (int idx)      const override;
    void                 regWrite(int idx, uint32_t val) override;

    /** The address of the next instruction. */
    uint32_t pc() const override { return m_pc; }
    void     setPc(uint32_t addr) override { m_pc = addr & PC_MASK; }
    uint32_t sp() const override { return m_r[13]; }

    int  step() override;
    void reset() override;

    void triggerIrq()   override { m_irq = true; }
    void triggerNmi()   override { m_fiq = true; }
    void triggerReset() override { reset(); }
    void setIrqLine(bool asserted) override { m_irq = asserted; }
    /** /FIQ (level-sensitive; the ICore NMI line). */
    void setNmiLine(bool asserted) override { m_fiq = asserted; }
    void setFiqLine(bool asserted) { m_fiq = asserted; }

    int  disassembleOne  (IBus* bus, uint32_t addr, char* buf, int bufsz) override;
    int  disassembleEntry(IBus* bus, uint32_t addr, void* entryOut) override;

    int  isCallAt    (IBus* bus, uint32_t addr) override;
    bool isReturnAt  (IBus* bus, uint32_t addr) override;
    bool isProgramEnd(IBus* bus) override;
    int  writeCallHarness(IBus* bus, uint32_t scratch, uint32_t routineAddr) override;

    void setDataBus(IBus* bus) override { m_bus = bus; }
    void setCodeBus(IBus* bus) override { (void)bus; }
    void setIoBus  (IBus* bus) override { (void)bus; }
    IBus* getDataBus() const override { return m_bus; }
    IBus* getCodeBus() const override { return m_bus; }
    IBus* getIoBus()   const override { return nullptr; }
    uint64_t cycles() const override { return m_cycles; }

    size_t stateSize() const override { return 4 * (16 + 2 + 7 + 7 + 4); }
    void   saveState(uint8_t* buf) const override;
    void   loadState(const uint8_t* buf) override;

    uint32_t reg(int n) const { return m_r[n & 15]; }
    uint32_t psr() const { return m_psr; }
    Mode mode() const { return (Mode)(m_psr & 3); }

private:
    uint32_t rd32(uint32_t a);
    void     wr32(uint32_t a, uint32_t v);
    uint8_t  rd8(uint32_t a) { return m_bus ? m_bus->read8(a & 0x03FFFFFF) : 0xFF; }
    void     wr8(uint32_t a, uint8_t v) { if (m_bus) m_bus->write8(a & 0x03FFFFFF, v); }

    uint32_t r15(int extra) const { return ((m_pc + 4 + (uint32_t)extra) & PC_MASK) | m_psr; }
    uint32_t regOp(int n, int extra) const { return n == 15 ? r15(extra) : m_r[n]; }
    void setMode(Mode m);
    void setPsr(uint32_t v, bool privileged);
    void exception(uint32_t vector, Mode m, bool maskFiq, uint32_t link);
    uint32_t shifter(uint32_t op, bool& carry, int& cycles);
    bool cond(uint32_t c) const;

    int dataProcessing(uint32_t op);
    int multiply(uint32_t op);
    int singleTransfer(uint32_t op);
    int blockTransfer(uint32_t op);

    IBus* m_bus = nullptr;
    bool m_arm1;
    uint32_t m_r[16] = {};
    uint32_t m_pc = 0;                  // the next instruction's address
    uint32_t m_psr = FLAG_I | FLAG_F | SVC;   // N Z C V I F, mode
    uint32_t m_usr[7] = {};             // R8-R14 user
    uint32_t m_fiqBank[7] = {};         // R8-R14 FIQ
    uint32_t m_irqBank[2] = {};         // R13-R14 IRQ
    uint32_t m_svcBank[2] = {};         // R13-R14 supervisor
    bool m_irq = false, m_fiq = false;
    uint64_t m_cycles = 0;
};
