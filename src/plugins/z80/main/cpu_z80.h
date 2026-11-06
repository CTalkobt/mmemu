#pragma once

#include "libcore/main/icore.h"
#include <cstdint>

/**
 * Zilog Z80 CPU core (#22, the C128's second CPU).
 *
 * All documented and undocumented instructions: the CB, ED, DD/FD and
 * DDCB/FDCB prefixes, IXH/IXL/IYH/IYL, SLL, the undocumented flag bits 3
 * and 5 (X, Y), MEMPTR (WZ) as BIT n,(HL) shows it, and the Q latch for
 * SCF/CCF (Patrik Rak's research). Flags and timings follow "The
 * Undocumented Z80 Documented" (Sean Young) and the Zilog manual; checked
 * with Frank Cringle's ZEXDOC and ZEXALL (tests/z80).
 *
 * Interrupts: a level-sensitive /INT (setIrqLine) in modes 0, 1 and 2 (the
 * data bus reads $FF: mode 0 runs RST 38h, mode 2 takes the vector at
 * I * 256 + $FF), an edge-triggered /NMI, HALT, and EI's one-instruction
 * delay. I/O goes to the I/O bus with the full 16-bit port address (B or A
 * in the high byte), as the C128 decodes it. step() returns T-states.
 */
class Z80 : public ICore {
public:
    Z80();

    // Registers, in regDescriptor() order
    enum Reg { R_AF, R_BC, R_DE, R_HL, R_IX, R_IY, R_SP, R_PC, R_AF2, R_BC2, R_DE2, R_HL2,
               R_I, R_R, R_IM, R_IFF, R_WZ, R_COUNT };

    const char* isaName()     const override { return "z80"; }
    const char* variantName() const override { return "Zilog Z80"; }
    uint32_t    isaCaps()     const override { return ISA_CAP_IO_PORT_SPACE | ISA_CAP_MULTI_PREFIX; }

    int                  regCount()             const override { return R_COUNT; }
    const RegDescriptor* regDescriptor(int idx) const override;
    uint32_t             regRead (int idx)      const override;
    void                 regWrite(int idx, uint32_t val) override;

    uint32_t pc() const override { return m_pc; }
    void     setPc(uint32_t addr) override { m_pc = (uint16_t)addr; m_halted = false; }
    uint32_t sp() const override { return m_sp; }

    int  step() override;
    void reset() override;

    void triggerIrq()   override { m_irqLine = true; }
    void triggerNmi()   override { m_nmiPending = true; }
    void triggerReset() override { reset(); }
    void setIrqLine(bool asserted) override { m_irqLine = asserted; }
    void setNmiLine(bool asserted) override { if (asserted && !m_nmiLine) m_nmiPending = true; m_nmiLine = asserted; }
    bool isHalted() const override { return m_halted; }

    int  disassembleOne  (IBus* bus, uint32_t addr, char* buf, int bufsz) override;
    int  disassembleEntry(IBus* bus, uint32_t addr, void* entryOut) override;

    int  isCallAt    (IBus* bus, uint32_t addr) override;
    bool isReturnAt  (IBus* bus, uint32_t addr) override;
    bool isProgramEnd(IBus* bus) override;
    int  writeCallHarness(IBus* bus, uint32_t scratch, uint32_t routineAddr) override;

    void setDataBus(IBus* bus) override { m_bus = bus; }
    void setCodeBus(IBus* bus) override { (void)bus; }
    void setIoBus  (IBus* bus) override { m_io = bus; }
    IBus* getDataBus() const override { return m_bus; }
    IBus* getCodeBus() const override { return m_bus; }
    IBus* getIoBus()   const override { return m_io; }
    uint64_t cycles() const override { return m_cycles; }

    size_t stateSize() const override { return 40; }
    void   saveState(uint8_t* buf) const override;
    void   loadState(const uint8_t* buf) override;

    // Flag bits
    static constexpr uint8_t FC = 0x01, FN = 0x02, FP = 0x04, FX = 0x08, FH = 0x10, FY = 0x20, FZ = 0x40, FS = 0x80;

    uint8_t a() const { return m_a; }
    uint8_t f() const { return m_f; }
    uint16_t bc() const { return (uint16_t)(m_b << 8 | m_c); }
    uint16_t de() const { return (uint16_t)(m_d << 8 | m_e); }
    uint16_t hl() const { return (uint16_t)(m_h << 8 | m_l); }

private:
    // Memory and I/O
    uint8_t rd(uint16_t a) { return m_bus ? m_bus->read8(a) : 0xFF; }
    void    wr(uint16_t a, uint8_t v) { if (m_bus) m_bus->write8(a, v); }
    uint16_t rd16(uint16_t a) { return (uint16_t)(rd(a) | rd((uint16_t)(a + 1)) << 8); }
    void    wr16(uint16_t a, uint16_t v) { wr(a, v & 0xFF); wr((uint16_t)(a + 1), v >> 8); }
    uint8_t in(uint16_t port) { return m_io ? m_io->read8(port) : 0xFF; }
    void    out(uint16_t port, uint8_t v) { if (m_io) m_io->write8(port, v); }
    uint8_t fetch() { return rd(m_pc++); }
    uint16_t fetch16() { uint16_t v = rd16(m_pc); m_pc = (uint16_t)(m_pc + 2); return v; }
    void push(uint16_t v) { m_sp = (uint16_t)(m_sp - 2); wr16(m_sp, v); }
    uint16_t pop() { uint16_t v = rd16(m_sp); m_sp = (uint16_t)(m_sp + 2); return v; }
    void incR() { m_r = (uint8_t)((m_r & 0x80) | ((m_r + 1) & 0x7F)); }

    // Register access by the opcode's 3-bit field (6 = (HL)/(IX+d))
    uint8_t  reg8(int r);
    void     setReg8(int r, uint8_t v);
    uint16_t rp(int p) const;               // BC DE HL/IX/IY SP
    void     setRp(int p, uint16_t v);
    uint16_t rp2(int p) const;              // BC DE HL/IX/IY AF
    void     setRp2(int p, uint16_t v);
    uint16_t& idx();                        // HL / IX / IY for the prefix
    uint16_t hlReg() const { return (uint16_t)(m_h << 8 | m_l); }
    void setHl(uint16_t v) { m_h = v >> 8; m_l = v & 0xFF; }
    bool cond(int y) const;

    // ALU
    void alu(int op, uint8_t v);
    uint8_t inc8(uint8_t v);
    uint8_t dec8(uint8_t v);
    uint8_t rot(int y, uint8_t v);          // CB rotates and shifts
    void add16(uint16_t& r, uint16_t v);
    void adc16(uint16_t v);
    void sbc16(uint16_t v);
    void bit(int n, uint8_t v, uint8_t xy);
    void setF(uint8_t f) { m_f = f; m_qNext = f; }

    int execMain(uint8_t op);
    int execCB();
    int execED();
    int execIndexCB();
    int interrupt();

    IBus* m_bus = nullptr;
    IBus* m_io = nullptr;

    uint8_t m_a = 0xFF, m_f = 0xFF, m_b = 0, m_c = 0, m_d = 0, m_e = 0, m_h = 0, m_l = 0;
    uint16_t m_af2 = 0xFFFF, m_bc2 = 0, m_de2 = 0, m_hl2 = 0;
    uint16_t m_ix = 0xFFFF, m_iy = 0xFFFF, m_sp = 0xFFFF, m_pc = 0, m_wz = 0;
    uint8_t m_i = 0, m_r = 0, m_im = 0;
    bool m_iff1 = false, m_iff2 = false;
    bool m_halted = false;
    bool m_irqLine = false, m_nmiLine = false, m_nmiPending = false;
    bool m_eiDelay = false;
    uint8_t m_q = 0, m_qNext = 0;           // flags written by the last instruction (SCF/CCF)

    // The current DD/FD prefix: 0 none, 1 IX, 2 IY; the (IX+d) address
    int m_prefix = 0;
    uint16_t m_dispAddr = 0;
    bool m_dispFetched = false;
    uint64_t m_cycles = 0;
};
