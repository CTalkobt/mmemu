#pragma once

#include <cstdint>
#include <functional>
#include <vector>

/**
 * Motorola MC6821 Peripheral Interface Adapter, after the data sheet.
 *
 * mmsim's PIA6520 (the PET's and VIC-20's) is not used for the 6821: it
 * clears the B side's interrupt flags on a write to the port, comes out of
 * reset with the ports at $FF, and has no pulse-strobe Cx2 modes. This one
 * follows the 6821 data sheet:
 *
 *  - Registers (address & 3): 0 port A / DDRA, 1 CRA, 2 port B / DDRB,
 *    3 CRB. CRx bit 2 selects the port (1) or its DDR (0).
 *  - CRx: bit 0 Cx1 IRQ enable, 1 Cx1 active edge (1 rising), 2 port/DDR,
 *    3-5 Cx2 (input: bit 4 active edge, bit 3 IRQ enable; output: 11x
 *    set/reset to bit 3, 100 handshake, 101 pulse), 6 IRQx2 flag, 7 IRQx1
 *    flag (read only). Writing bit 5 (Cx2 output) clears IRQx2.
 *  - An active Cx1/Cx2 edge sets its flag whether or not the interrupt is
 *    enabled; /IRQA and /IRQB follow flag AND enable. Reading the port
 *    (not the DDR) clears both of its flags.
 *  - Port A: outputs with pull-ups, so a read returns the pins (an output a
 *    device pulls low reads 0). Port B: push-pull, so outputs read the
 *    output register and inputs the pins.
 *  - Handshake: CA2 goes low on a read of port A, CB2 on a write of port
 *    B, and back high on the Cx1 active edge; pulse mode restores it after
 *    the cycle.
 *  - Reset clears every register; the Cx2 lines are inputs.
 *  - /IRQx follows an active Cx1/Cx2 edge after tRS3 (1 us max; XRoar
 *    takes the full microsecond): the flag reads set at once, the output
 *    only once `irqDelay` has passed. Time is the caller's (advance());
 *    with irqDelay 0 the output follows at once.
 *
 * The outside world: per side, `inSink` (bits a device pulls low) and
 * `inSource` (bits a device drives high, port B only); pins() gives the
 * level on the pins. Hooks run before a port read (to set the inputs),
 * after a port or DDR write, and after a control change (Cx2 output);
 * onIrq after either IRQ output may have changed.
 */
class Mc6821 {
public:
    struct Side {
        uint8_t cr = 0, ddr = 0, out = 0;
        bool c1 = false, c2 = false;            // last level seen on Cx1, Cx2 (input)
        bool irq1 = false, irq2 = false;        // the flags
        uint64_t irq1At = 0, irq2At = 0;        // when each flag reaches /IRQx
        bool c2Out = true;                      // Cx2's driven level when an output
        uint8_t inSink = 0xFF, inSource = 0;    // external loads on the pins
        bool c2InSink = true, c2InSource = false;
        std::function<void()> preRead, postWrite, onControl;

        bool c2IsOutput() const { return cr & 0x20; }
    };

    Mc6821();

    Side a, b;
    std::function<void()> onIrq;
    /** tRS3: an active Cx1/Cx2 edge to /IRQx, in advance()'s units (0: at once). */
    uint64_t irqDelay = 0;

    /** The time is now `now` (never backwards): edges from here on are
     *  timed from it, and an /IRQ output that falls due asserts (onIrq). */
    void advance(uint64_t now);
    uint64_t now() const { return m_now; }

    void reset();
    uint8_t read(uint16_t addr);
    /** A read without side effects (no flags cleared, no hooks). */
    uint8_t peek(uint16_t addr) const;
    void write(uint16_t addr, uint8_t v);

    /** The level on CA1 / CB1. */
    void setCa1(bool level) { setC1(a, level, true); }
    void setCb1(bool level) { setC1(b, level, false); }
    /** Re-read Cx2 (an input) after its external load changed. */
    void updateCa2() { updateC2(a); }
    void updateCb2() { updateC2(b); }

    /** The pins as a device outside sees them. */
    uint8_t portA() const { return (uint8_t)((a.out | ~a.ddr) & a.inSink); }
    uint8_t portB() const { return (uint8_t)(((b.out & b.ddr) | b.inSource) & (b.out | ~b.ddr) & b.inSink); }
    bool ca2() const { return (a.c2IsOutput() ? a.c2Out : true) && a.c2InSink; }
    bool cb2() const { return (b.c2IsOutput() ? b.c2Out : b.c2InSource) && b.c2InSink; }
    bool irqA() const { return irqOut(a); }
    bool irqB() const { return irqOut(b); }
    bool irq() const { return irqA() || irqB(); }

    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    bool irqOut(const Side& s) const {
        return (s.irq1 && (s.cr & 1) && s.irq1At <= m_now) ||
               (s.irq2 && (s.cr & 8) && !(s.cr & 0x20) && s.irq2At <= m_now);
    }
    uint64_t m_now = 0;
    void setC1(Side& s, bool level, bool isA);
    void updateC2(Side& s);
    void strobe(Side& s);
    void writeCr(Side& s, uint8_t v);
    void irqChanged() { if (onIrq) onIrq(); }
};
