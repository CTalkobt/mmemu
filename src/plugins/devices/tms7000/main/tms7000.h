#pragma once

#include <cstdint>
#include <functional>
#include <vector>

/**
 * The Texas Instruments TMS7000 family's NMOS single-chip microcontroller,
 * as a TMS7040: 128 bytes of register file (RAM) at $0000-$007F, the
 * peripheral file at $0100-$010B (I/O control, timer 1, ports A-D), 4 KB
 * of mask ROM at $F000-$FFFF (a 2 KB image is a TMS7020's, at $F800).
 *
 * After MAME's tms7000 (src/devices/cpu/tms7000, hap and Tim Lindner,
 * BSD-3-Clause): the instruction set and its cycle counts, the status
 * register (C N Z I), INT1/INT3 (level, flagged in IOCNT0), timer 1 with
 * its 5-bit prescaler (internal source: every 16 x (prescale + 1)
 * oscillator clocks) raising INT2, the traps at $FFxx, IDLE, and RESET's
 * trap 0 (which, with SP $FF, pushes the PC into A and B as the chip does).
 * Single-chip mode only (IOCNT0's memory modes are not modelled; nothing
 * answers outside the internal map, which reads 0). No event counter, no
 * TMS70x2 serial port or timer 2.
 *
 * Time is machine cycles: the oscillator divided by 2.
 */
class Tms7000 {
public:
    Tms7000();

    /** The mask ROM: 4 KB at $F000 (2 KB at $F800); empty reads 0. */
    void setRom(std::vector<uint8_t> rom);
    bool hasRom() const { return !m_rom.empty(); }

    /** RESET: ports to input (B and, on NMOS, C and D high), SP $FF, trap 0. */
    void reset();
    /** One instruction (or IDLE, or taking an interrupt): its machine cycles. */
    int step();

    /** The /INT1 and /INT3 pins (true = asserted). INT3 also latches timer 1. */
    void setInt1(bool on) { setInput(0, on); }
    void setInt3(bool on) { setInput(1, on); }

    /** Ports: 0 A (input only), 1 B (output only), 2 C, 3 D. Reads see the
     *  pins (`portIn`) where the DDR is 0; writes send (data & DDR). */
    std::function<uint8_t(int port)> portIn;
    std::function<void(int port, uint8_t v)> portOut;

    uint16_t pc() const { return m_pc; }
    uint8_t sp() const { return m_sp; }
    uint8_t st() const { return m_sr; }
    bool idling() const { return m_idle; }
    uint8_t reg(uint8_t r) const { return m_rf[r & 0x7F]; }
    void setReg(uint8_t r, uint8_t v) { m_rf[r & 0x7F] = v; }
    void setPc(uint16_t pc) { m_pc = pc; }
    /** The address space as the CPU sees it, without side effects. */
    uint8_t peek(uint16_t a) const;
    uint8_t ioControl() const { return m_ioControl; }
    uint8_t timer() const { return (uint8_t)m_timerDec; }

    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    enum : uint8_t { SR_C = 0x80, SR_N = 0x40, SR_Z = 0x20, SR_I = 0x10 };
    using OpFunc = int (Tms7000::*)(uint8_t, uint8_t);

    void setInput(int line, bool on);
    void flagExt(int line);
    void checkInterrupts();
    void doInterrupt(int line);
    void timerReload();
    void timerTickLow();
    void timerAdvance(int cycles);

    uint8_t pfRead(uint8_t off);
    void pfWrite(uint8_t off, uint8_t v);
    uint8_t read(uint16_t a);
    void write(uint16_t a, uint8_t v);

    uint8_t rr(uint8_t r) { return read(r); }
    void wr(uint8_t r, uint8_t v) { write(r, v); }
    uint16_t rr16(uint8_t r) { return (uint16_t)(read((uint8_t)(r - 1)) << 8 | read(r)); }
    void wr16(uint8_t r, uint16_t v) { write((uint8_t)(r - 1), (uint8_t)(v >> 8)); write(r, (uint8_t)v); }
    uint8_t rp(uint8_t p) { return read((uint16_t)(0x100 + p)); }
    void wp(uint8_t p, uint8_t v) { write((uint16_t)(0x100 + p), v); }
    uint16_t rm16(uint16_t a) { return (uint16_t)(read(a) << 8 | read((uint16_t)(a + 1))); }
    void wm16(uint16_t a, uint16_t v) { write(a, (uint8_t)(v >> 8)); write((uint16_t)(a + 1), (uint8_t)v); }
    uint8_t imm8() { return read(m_pc++); }
    uint16_t imm16() { uint16_t v = (uint16_t)(read(m_pc++) << 8); return (uint16_t)(v | read(m_pc++)); }
    uint8_t pull8() { return read(m_sp--); }
    void push8(uint8_t v) { write(++m_sp, v); }
    uint16_t pull16() { uint16_t v = read(m_sp--); return (uint16_t)(v | read(m_sp--) << 8); }
    void push16(uint16_t v) { write(++m_sp, (uint8_t)(v >> 8)); write(++m_sp, (uint8_t)v); }

    void setC(unsigned x) { m_sr = (uint8_t)((m_sr & 0x7F) | ((x >> 1) & 0x80)); }
    void setNZ(unsigned x) { m_sr = (uint8_t)((m_sr & 0x9F) | ((x >> 1) & 0x40) | ((x & 0xFF) ? 0 : 0x20)); }
    void setCNZ(unsigned x) { m_sr = (uint8_t)((m_sr & 0x1F) | ((x >> 1) & 0xC0) | ((x & 0xFF) ? 0 : 0x20)); }
    int getC() const { return m_sr >> 7 & 1; }

    void execute(uint8_t op);
    // addressing modes
    void amA(OpFunc op), amB(OpFunc op), amR(OpFunc op);
    void amA2A(OpFunc op), amA2B(OpFunc op), amA2R(OpFunc op), amA2P(OpFunc op);
    void amB2A(OpFunc op), amB2B(OpFunc op), amB2R(OpFunc op), amB2P(OpFunc op);
    void amR2A(OpFunc op), amR2B(OpFunc op), amR2R(OpFunc op);
    void amI2A(OpFunc op), amI2B(OpFunc op), amI2R(OpFunc op), amI2P(OpFunc op);
    void amP2A(OpFunc op), amP2B(OpFunc op);
    // ALU (a result >= 0 is written back)
    int opClr(uint8_t, uint8_t), opDec(uint8_t, uint8_t), opInc(uint8_t, uint8_t), opInv(uint8_t, uint8_t);
    int opRl(uint8_t, uint8_t), opRlc(uint8_t, uint8_t), opRr(uint8_t, uint8_t), opRrc(uint8_t, uint8_t);
    int opSwap(uint8_t, uint8_t), opXchb(uint8_t, uint8_t);
    int opAdc(uint8_t, uint8_t), opAdd(uint8_t, uint8_t), opAnd(uint8_t, uint8_t), opCmp(uint8_t, uint8_t);
    int opDac(uint8_t, uint8_t), opDsb(uint8_t, uint8_t), opMpy(uint8_t, uint8_t), opMov(uint8_t, uint8_t);
    int opOr(uint8_t, uint8_t), opSbb(uint8_t, uint8_t), opSub(uint8_t, uint8_t), opXor(uint8_t, uint8_t);
    int opDjnz(uint8_t, uint8_t), opBtjo(uint8_t, uint8_t), opBtjz(uint8_t, uint8_t);
    void shortBranch(bool take);
    void jmp(bool take);
    void trap(uint8_t vec);

    std::vector<uint8_t> m_rom;
    uint8_t m_rf[128] = {};
    int m_cycles = 0;              // the current step's
    uint16_t m_pc = 0;
    uint8_t m_sp = 0xFF, m_sr = 0;
    bool m_irq[2] = {};
    bool m_idle = false;
    uint8_t m_ioControl = 0;
    uint8_t m_timerData = 0, m_timerControl = 0, m_timerCapture = 0;
    int m_timerDec = 0;
    int m_timerLeft = 0;           // cycles to the next decrement
    bool m_timerRunning = false;
    uint8_t m_portLatch[4] = {};
    uint8_t m_portDdr[4] = {0, 0xFF, 0, 0};
};
