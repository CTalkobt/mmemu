#pragma once

#include "libcore/main/icore.h"
#include <cstdint>

/**
 * Motorola 6809 CPU core (#13, the BBC Micro's 6809 Second Processor).
 *
 * After the MC6809 / MC6809E data sheet (Motorola): every documented
 * instruction on pages 0, 2 ($10) and 3 ($11), all the indexed modes
 * (5-, 8- and 16-bit offsets, accumulator offsets, auto-increment and
 * decrement, PC-relative, extended indirect and the indirect forms) with
 * their extra cycles, TFR/EXG, PSH/PUL, CWAI, SYNC, SWI/SWI2/SWI3, and the
 * NMI, FIRQ and IRQ sequences (NMI is not taken until S has been loaded,
 * as the data sheet says). Cycle counts are the data sheet's. Checked
 * against GCC6809's simulator (b-em's mc6809nc) instruction by
 * instruction.
 *
 * Interrupts: /IRQ and /FIRQ are level-sensitive (setIrqLine,
 * setFirqLine), /NMI is edge-triggered. step() returns cycles.
 */
class M6809 : public ICore {
public:
    M6809();

    enum Reg { R_A, R_B, R_D, R_X, R_Y, R_U, R_S, R_PC, R_DP, R_CC, R_COUNT };
    static constexpr uint8_t CC_C = 0x01, CC_V = 0x02, CC_Z = 0x04, CC_N = 0x08,
                             CC_I = 0x10, CC_H = 0x20, CC_F = 0x40, CC_E = 0x80;

    const char* isaName()     const override { return "6809"; }
    const char* variantName() const override { return "Motorola 6809"; }
    uint32_t    isaCaps()     const override { return 0; }

    int                  regCount()             const override { return R_COUNT; }
    const RegDescriptor* regDescriptor(int idx) const override;
    uint32_t             regRead (int idx)      const override;
    void                 regWrite(int idx, uint32_t val) override;

    uint32_t pc() const override { return m_pc; }
    void     setPc(uint32_t addr) override { m_pc = (uint16_t)addr; m_wait = WAIT_NONE; }
    uint32_t sp() const override { return m_s; }

    int  step() override;
    /** Power-on / /RESET: the vector at $FFFE, I and F set, NMI disarmed. */
    void reset() override;

    void triggerIrq()   override { m_irq = true; }
    void triggerNmi()   override { m_nmiPending = true; }
    void triggerReset() override { reset(); }
    void setIrqLine(bool asserted) override { m_irq = asserted; }
    void setNmiLine(bool asserted) override { if (asserted && !m_nmiLine) m_nmiPending = true; m_nmiLine = asserted; }
    /** Where the vectors are read ($FFF0 by default): b-em's 6809 Second
     *  Processor board fetches them from $FEF0-$FEFF, inside its ROM. */
    void setVectorBase(uint16_t base) { m_vectors = base; }
    /** /FIRQ (level-sensitive). */
    void setFirqLine(bool asserted) { m_firq = asserted; }
    bool isHalted() const override { return m_wait != WAIT_NONE; }

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

    size_t stateSize() const override { return 16; }
    void   saveState(uint8_t* buf) const override;
    void   loadState(const uint8_t* buf) override;

    uint8_t a() const { return m_a; }
    uint8_t b() const { return m_b; }
    uint8_t cc() const { return m_cc; }
    uint16_t d() const { return (uint16_t)(m_a << 8 | m_b); }
    uint16_t x() const { return m_x; }
    uint16_t y() const { return m_y; }
    uint16_t u() const { return m_u; }

private:
    enum Wait { WAIT_NONE, WAIT_CWAI, WAIT_SYNC };

    uint8_t rd(uint16_t a) { return m_bus ? m_bus->read8(a) : 0xFF; }
    void    wr(uint16_t a, uint8_t v) { if (m_bus) m_bus->write8(a, v); }
    uint16_t rd16(uint16_t a) { uint8_t h = rd(a); return (uint16_t)(h << 8 | rd((uint16_t)(a + 1))); }
    void    wr16(uint16_t a, uint16_t v) { wr(a, v >> 8); wr((uint16_t)(a + 1), v & 0xFF); }
    uint8_t fetch() { return rd(m_pc++); }
    uint16_t fetch16() { uint16_t v = rd16(m_pc); m_pc = (uint16_t)(m_pc + 2); return v; }
    void push8(uint16_t& sp, uint8_t v) { wr(--sp, v); }
    void push16(uint16_t& sp, uint16_t v) { push8(sp, v & 0xFF); push8(sp, v >> 8); }
    uint8_t pull8(uint16_t& sp) { return rd(sp++); }
    uint16_t pull16(uint16_t& sp) { uint8_t h = pull8(sp); return (uint16_t)(h << 8 | pull8(sp)); }
    void setD(uint16_t v) { m_a = v >> 8; m_b = v & 0xFF; }

    // Flags
    void nz8(uint8_t v) { m_cc = (uint8_t)((m_cc & ~(CC_N | CC_Z)) | (v & 0x80 ? CC_N : 0) | (v ? 0 : CC_Z)); }
    void nz16(uint16_t v) { m_cc = (uint8_t)((m_cc & ~(CC_N | CC_Z)) | (v & 0x8000 ? CC_N : 0) | (v ? 0 : CC_Z)); }
    void flag(uint8_t f, bool on) { if (on) m_cc |= f; else m_cc &= (uint8_t)~f; }

    // Arithmetic
    uint8_t add8(uint8_t a, uint8_t b, int carry);
    uint8_t sub8(uint8_t a, uint8_t b, int borrow);
    uint16_t add16(uint16_t a, uint16_t b);
    uint16_t sub16(uint16_t a, uint16_t b);
    uint8_t rmw(int op, uint8_t v);            // NEG COM LSR ROR ASR ASL ROL DEC INC TST CLR

    // Addressing: the effective address of an indexed operand (extra cycles into m_extra)
    uint16_t indexed();
    uint16_t& indexReg(int r);
    uint16_t regTfr(int code) const;
    void setRegTfr(int code, uint16_t v);

    int execPage0(uint8_t op);
    int execPage2(uint8_t op);
    int execPage3(uint8_t op);
    void pushAll();                             // the entire state (E set)
    int interrupt();                            // 0 if none taken

    IBus* m_bus = nullptr;
    uint8_t m_a = 0, m_b = 0, m_dp = 0, m_cc = CC_I | CC_F;
    uint16_t m_x = 0, m_y = 0, m_u = 0, m_s = 0, m_pc = 0;
    bool m_irq = false, m_firq = false, m_nmiLine = false, m_nmiPending = false, m_nmiArmed = false;
    Wait m_wait = WAIT_NONE;
    bool m_cwaiStacked = false;
    int m_extra = 0;
    uint16_t m_vectors = 0xFFF0;
    uint16_t vec(int n) { return rd16((uint16_t)(m_vectors | n)); }
    uint64_t m_cycles = 0;
};
