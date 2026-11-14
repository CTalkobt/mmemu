#pragma once

#include "libcore/main/icore.h"
#include <cstdint>
#include <functional>

/**
 * Intel 8085A / Oki MSM80C85 CPU core (#334, the Tandy Model 100/102/200's CPU).
 *
 * The full 8080 instruction set plus RIM and SIM, the 8085's undocumented
 * instructions (DSUB, ARHL, RDEL, LDHI, LDSI, RSTV, SHLX, JNK, LHLX, JK) and
 * its undocumented flags V (bit 1, signed overflow) and K (bit 5, also X5 or
 * UI). T-states are the 8085's (Intel 8085AH data sheet, instruction set
 * summary), not the 8080's.
 *
 * Flags: S Z K AC 0 P V CY. Bit 3 always reads 0. As the 8085 data sheet
 * gives it, ANA/ANI set AC (the 8080 sets it to the OR of the operands' bit
 * 3). V and K after Ken Shirriff's reverse engineering of the 8085's flag
 * circuits (righto.com, "Looking at silicon to understand the 8085's
 * undocumented flags", 2013): V is the carry into bit 7 XOR the carry out of
 * it for the adds and subtracts (RLC/RAL count as A + A; INR only from $7F,
 * DCR only from $80; DAD from the high byte), 0 after the logic operations
 * and RRC/RAR; K is V XOR the result's sign for 8-bit operations and DAD, and
 * the 16-bit incrementer's carry for INX/DCX (INX to $0000, DCX to $FFFF).
 *
 * Interrupts, highest priority first: TRAP (rising edge, and still high when
 * sampled; not maskable), RST 7.5 (a rising-edge latch, cleared by SIM with
 * bit 4 or by its acknowledgement), RST 6.5 and RST 5.5 (level), each masked
 * by SIM, then INTR (level), which reads an instruction from the data bus in
 * INTA cycles (setIntaCallback(); RST 7 by default, RST n and CALL nn
 * handled). All but TRAP need the IE flag (EI takes effect after the next
 * instruction); taking any interrupt clears IE. HLT stops until an interrupt
 * is taken (PC is already past the HLT). RIM reads SID, the pending
 * interrupts, IE and the masks; after a TRAP the first RIM shows the IE from
 * before it. SID and SOD are the RIM/SIM serial bits.
 *
 * I/O: IN/OUT go to the I/O bus (setIoBus) at the 8-bit port number ($00-$FF;
 * the 8085 puts it on A0-A7 and A8-A15 alike). step() returns T-states.
 */
class I8085 : public ICore {
public:
    I8085();

    // Registers, in regDescriptor() order (GDB's Z80 order: AF BC DE HL SP PC)
    enum Reg { R_AF, R_BC, R_DE, R_HL, R_SP, R_PC, R_IM, R_COUNT };

    /** Interrupt sources, highest priority first. */
    enum class Irq { Trap, Rst75, Rst65, Rst55, Intr };

    // Flag bits
    static constexpr uint8_t FC = 0x01, FV = 0x02, FP = 0x04, FA = 0x10, FK = 0x20, FZ = 0x40, FS = 0x80;
    // The interrupt mask register as RIM reads it
    static constexpr uint8_t IM_M55 = 0x01, IM_M65 = 0x02, IM_M75 = 0x04, IM_IE = 0x08,
                             IM_I55 = 0x10, IM_I65 = 0x20, IM_I75 = 0x40, IM_SID = 0x80;

    const char* isaName()     const override { return "8085"; }
    const char* variantName() const override { return "Intel 8085A"; }
    uint32_t    isaCaps()     const override { return ISA_CAP_IO_PORT_SPACE; }

    int                  regCount()             const override { return R_COUNT; }
    const RegDescriptor* regDescriptor(int idx) const override;
    uint32_t             regRead (int idx)      const override;
    void                 regWrite(int idx, uint32_t val) override;

    uint32_t pc() const override { return m_pc; }
    void     setPc(uint32_t addr) override { m_pc = (uint16_t)addr; m_halted = false; }
    uint32_t sp() const override { return m_sp; }

    int  step() override;
    void reset() override;

    // ICore's lines: IRQ is INTR, NMI is TRAP
    void triggerIrq()   override { m_intr = true; }
    void triggerNmi()   override { m_trapPending = true; }
    void triggerReset() override { reset(); }
    void setIrqLine(bool asserted) override { setIntr(asserted); }
    void setNmiLine(bool asserted) override { setTrap(asserted); }
    bool isHalted() const override { return m_halted; }

    /** INTR (level). */
    void setIntr(bool level) { m_intr = level; }
    /** RST 5.5 (level). */
    void setRst55(bool level) { m_rst55 = level; }
    /** RST 6.5 (level). */
    void setRst65(bool level) { m_rst65 = level; }
    /** RST 7.5: a rising edge sets its latch (even while masked). */
    void setRst75(bool level) { if (level && !m_rst75) m_i75 = true; m_rst75 = level; }
    /** TRAP: a rising edge makes it pending; it must still be high when taken. */
    void setTrap(bool level) {
        if (level && !m_trap) m_trapPending = true;
        else if (!level) m_trapPending = false;
        m_trap = level;
    }

    /** The bytes INTR's acknowledge cycles read (an RST n or CALL nn): each call
     *  gives the next byte. Unset: $FF (RST 7). */
    void setIntaCallback(std::function<uint8_t()> fn) { m_inta = std::move(fn); }
    /** Called as each interrupt is taken, before its vector is read. */
    void setAckHook(std::function<void(Irq)> fn) { m_ack = std::move(fn); }
    /** The SID input RIM reads (a callback, else the level setSid() gave). */
    void setSidCallback(std::function<bool()> fn) { m_sidFn = std::move(fn); }
    void setSid(bool level) { m_sid = level; }
    /** SOD changes (SIM with bit 6). */
    void setSodCallback(std::function<void(bool)> fn) { m_sodFn = std::move(fn); }
    bool sod() const { return m_sod; }

    /** What RIM would read now (without the after-TRAP IE). */
    uint8_t rimValue() const;
    bool interruptsEnabled() const { return m_ie; }
    uint8_t masks() const { return m_mask; }

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
    void setCycles(uint64_t t) { m_cycles = t; }

    size_t stateSize() const override { return 24; }
    void   saveState(uint8_t* buf) const override;
    void   loadState(const uint8_t* buf) override;

    uint8_t a() const { return m_a; }
    uint8_t f() const { return m_f; }
    uint16_t bc() const { return (uint16_t)(m_b << 8 | m_c); }
    uint16_t de() const { return (uint16_t)(m_d << 8 | m_e); }
    uint16_t hl() const { return (uint16_t)(m_h << 8 | m_l); }

private:
    uint8_t rd(uint16_t a) { return m_bus ? m_bus->read8(a) : 0xFF; }
    void    wr(uint16_t a, uint8_t v) { if (m_bus) m_bus->write8(a, v); }
    uint8_t fetch() { return rd(m_pc++); }
    uint16_t fetch16() { uint8_t lo = fetch(); return (uint16_t)(lo | fetch() << 8); }
    uint16_t rd16(uint16_t a) { uint8_t lo = rd(a); return (uint16_t)(lo | rd((uint16_t)(a + 1)) << 8); }
    void    wr16(uint16_t a, uint16_t v) { wr(a, v & 0xFF); wr((uint16_t)(a + 1), v >> 8); }
    void    push(uint16_t v) { wr(--m_sp, v >> 8); wr(--m_sp, v & 0xFF); }
    uint16_t pop() { uint16_t v = rd16(m_sp); m_sp = (uint16_t)(m_sp + 2); return v; }

    uint8_t  reg8(int r);
    void     setReg8(int r, uint8_t v);
    uint16_t rp(int p) const;
    void     setRp(int p, uint16_t v);
    void     setHl(uint16_t v) { m_h = v >> 8; m_l = v & 0xFF; }
    void     setDe(uint16_t v) { m_d = v >> 8; m_e = v & 0xFF; }
    bool     cond(int y) const;

    // ALU
    void    add(uint8_t v, int carry);
    uint8_t sub(uint8_t v, int borrow);
    void    logic(uint8_t res, bool ac);
    uint8_t inr(uint8_t v);
    uint8_t dcr(uint8_t v);
    void    daa();
    void    dad(uint16_t v);
    void    dsub();

    int  exec(uint8_t op);
    int  checkInterrupts();
    int  takeInterrupt(Irq src, uint16_t vector);
    void setSod(bool level) { if (level != m_sod) { m_sod = level; if (m_sodFn) m_sodFn(level); } }

    IBus* m_bus = nullptr;
    IBus* m_io = nullptr;

    uint8_t m_a = 0, m_f = 0, m_b = 0, m_c = 0, m_d = 0, m_e = 0, m_h = 0, m_l = 0;
    uint16_t m_sp = 0, m_pc = 0;
    uint64_t m_cycles = 0;

    // Interrupts
    bool m_ie = false;
    bool m_eiDelay = false;
    bool m_halted = false;
    uint8_t m_mask = IM_M55 | IM_M65 | IM_M75;
    bool m_i75 = false;                     // the RST 7.5 latch
    bool m_intr = false, m_rst55 = false, m_rst65 = false, m_rst75 = false;
    bool m_trap = false, m_trapPending = false;
    int  m_trapIe = -1;                     // IE before the last TRAP, for the next RIM (-1: none)
    bool m_sid = false, m_sod = false;

    std::function<uint8_t()> m_inta;
    std::function<void(Irq)> m_ack;
    std::function<bool()> m_sidFn;
    std::function<void(bool)> m_sodFn;
};
