#pragma once

#include "libcore/main/icore.h"
#include <cstdint>

/**
 * WDC 65C816 CPU core (#12, the Apple IIgs).
 *
 * After the W65C816S data sheet (WDC): the 16-bit accumulator and index
 * registers (M and X flags), the direct page register, the data and
 * program bank registers, emulation mode (E, entered by XCE) with the
 * 6502's stack page and direct-page wrap, every instruction and addressing
 * mode (24-bit long and indirect long, stack relative, block moves), the
 * decimal mode in 8 and 16 bits, COP/BRK, WAI and STP, the native and
 * emulation vectors. Each bus cycle is one access to the data bus (24-bit
 * addresses) or one internal cycle, in the data sheet's order (including
 * the conditional ones: direct page low byte not zero, index page
 * crossings, a branch crossing a page in emulation mode), so step()
 * returns the cycle count. Checked against Tom Harte's SingleStepTests
 * (tests/65816).
 *
 * Interrupts: a level-sensitive /IRQ, an edge-triggered /NMI, /ABORT not
 * used.
 */
class W65C816 : public ICore {
public:
    W65C816();

    enum Reg { R_A, R_X, R_Y, R_S, R_D, R_DBR, R_PBR, R_PC, R_P, R_E, R_COUNT };
    static constexpr uint8_t FC = 0x01, FZ = 0x02, FI = 0x04, FD = 0x08, FX = 0x10, FM = 0x20, FV = 0x40, FN = 0x80;

    const char* isaName()     const override { return "65816"; }
    const char* variantName() const override { return "WDC 65C816"; }
    uint32_t    isaCaps()     const override { return 0; }

    int                  regCount()             const override { return R_COUNT; }
    const RegDescriptor* regDescriptor(int idx) const override;
    uint32_t             regRead (int idx)      const override;
    void                 regWrite(int idx, uint32_t val) override;

    /** The full 24-bit program counter (PBR:PC). */
    uint32_t pc() const override { return (uint32_t)m_pbr << 16 | m_pc; }
    void     setPc(uint32_t addr) override { m_pbr = (addr >> 16) & 0xFF; m_pc = addr & 0xFFFF; m_wait = m_stop = false; }
    uint32_t sp() const override { return m_s; }

    int  step() override;
    /** /RESET: emulation mode, the vector at $FFFC. */
    void reset() override;

    void triggerIrq()   override { m_irqLine = true; }
    void triggerNmi()   override { m_nmiPending = true; }
    void triggerReset() override { reset(); }
    void setIrqLine(bool asserted) override { m_irqLine = asserted; }
    void setNmiLine(bool asserted) override { if (asserted && !m_nmiLine) m_nmiPending = true; m_nmiLine = asserted; }
    bool isHalted() const override { return m_wait || m_stop; }

    int  disassembleOne  (IBus* bus, uint32_t addr, char* buf, int bufsz) override;
    int  disassembleEntry(IBus* bus, uint32_t addr, void* entryOut) override;

    int  isCallAt    (IBus* bus, uint32_t addr) override;
    bool isReturnAt  (IBus* bus, uint32_t addr) override;
    bool isProgramEnd(IBus* bus) override { (void)bus; return m_stop; }
    int  writeCallHarness(IBus* bus, uint32_t scratch, uint32_t routineAddr) override;

    void setDataBus(IBus* bus) override { m_bus = bus; }
    void setCodeBus(IBus* bus) override { (void)bus; }
    void setIoBus  (IBus* bus) override { (void)bus; }
    IBus* getDataBus() const override { return m_bus; }
    IBus* getCodeBus() const override { return m_bus; }
    IBus* getIoBus()   const override { return nullptr; }
    uint64_t cycles() const override { return m_cycles; }

    size_t stateSize() const override { return 20; }
    void   saveState(uint8_t* buf) const override;
    void   loadState(const uint8_t* buf) override;

    uint16_t a() const { return m_a; }
    uint16_t x() const { return m_x; }
    uint16_t y() const { return m_y; }
    uint8_t p() const { return m_p; }
    bool emulation() const { return m_e; }
    /** /VP (vector pull) is low: the current read is of an interrupt or reset vector. */
    bool vectorPull() const { return m_vpb; }
    /** Internal (bus-idle) cycles happen here; tests can observe them. */
    struct IdleHook { virtual ~IdleHook() = default; virtual void idle(uint32_t addr) = 0; };
    void setIdleHook(IdleHook* h) { m_idleHook = h; }

private:
    // Bus cycles
    uint8_t read(uint32_t a) { ++m_cycles; return m_bus ? m_bus->read8(a & 0xFFFFFF) : 0xFF; }
    void write(uint32_t a, uint8_t v) { ++m_cycles; if (m_bus) m_bus->write8(a & 0xFFFFFF, v); }
    void idle() { ++m_cycles; if (m_idleHook) m_idleHook->idle(m_lastAddr); }
    void idleAt(uint32_t a) { ++m_cycles; if (m_idleHook) m_idleHook->idle(a); }

    uint8_t fetch() { m_lastAddr = (uint32_t)m_pbr << 16 | m_pc; uint8_t v = read(m_lastAddr); m_pc++; return v; }
    // Direct page (bank 0): wraps inside the page in emulation mode when DL = 0
    uint32_t directAddr(uint16_t off) const {
        if (m_e && !(m_d & 0xFF)) return (uint32_t)((m_d & 0xFF00) | (off & 0xFF));
        return (uint16_t)(m_d + off);
    }
    uint8_t readDirect(uint16_t off) { return read(directAddr(off)); }
    void writeDirect(uint16_t off, uint8_t v) { write(directAddr(off), v); }
    uint8_t readDirectN(uint16_t off) { return read((uint16_t)(m_d + off)); }
    // The stack: page 1 in emulation mode
    void push(uint8_t v) {
        write(m_s, v);
        if (m_e) m_s = (uint16_t)(0x100 | ((m_s - 1) & 0xFF)); else m_s--;
    }
    uint8_t pull() {
        if (m_e) m_s = (uint16_t)(0x100 | ((m_s + 1) & 0xFF)); else m_s++;
        return read(m_s);
    }
    // The "new" 65816 instructions push and pull without the page-1 wrap
    void pushN(uint8_t v) { write(m_s, v); m_s--; }
    uint8_t pullN() { m_s++; return read(m_s); }
    void fixStack() { if (m_e) m_s = (uint16_t)(0x100 | (m_s & 0xFF)); }

    void idle2() { if (m_d & 0xFF) idle(); }                          // DL != 0
    void idle4(uint16_t from, uint16_t to) { if (!(m_p & FX) || (from & 0xFF00) != (to & 0xFF00)) idle(); }
    void idle6(uint16_t to) { if (m_e && (m_pc & 0xFF00) != (to & 0xFF00)) idle(); }

    bool m8() const { return m_p & FM; }
    bool x8() const { return m_p & FX; }
    void setP(uint8_t v);
    void nz8(uint8_t v) { m_p = (uint8_t)((m_p & ~(FN | FZ)) | (v & 0x80) | (v ? 0 : FZ)); }
    void nz16(uint16_t v) { m_p = (uint8_t)((m_p & ~(FN | FZ)) | ((v >> 8) & 0x80) | (v ? 0 : FZ)); }
    void flag(uint8_t f, bool on) { if (on) m_p |= f; else m_p &= (uint8_t)~f; }

    void interrupt(uint16_t nativeVec, uint16_t emuVec, bool hardware);
    int execute(uint8_t op);

    IBus* m_bus = nullptr;
    IdleHook* m_idleHook = nullptr;
    uint16_t m_a = 0, m_x = 0, m_y = 0, m_s = 0x1FF, m_d = 0, m_pc = 0;
    uint8_t m_dbr = 0, m_pbr = 0, m_p = 0x34;
    bool m_e = true;
    bool m_irqLine = false, m_nmiLine = false, m_nmiPending = false, m_wait = false, m_stop = false;
    bool m_vpb = false;
    uint32_t m_lastAddr = 0;
    uint64_t m_cycles = 0;
};
