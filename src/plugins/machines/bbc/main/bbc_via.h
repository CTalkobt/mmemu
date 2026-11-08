#pragma once

#include <cstdint>
#include <functional>

namespace bbc {

/**
 * A 6522 VIA as b-em has it (via.c, GPL-2.0-or-later): clocked in 2 MHz
 * cycles, with the timers held in half 1 MHz cycles (a latch value n counts
 * down as 2n + ...), so reads, reloads and the IRQ land where they do on a
 * BBC Micro. The ports, CA2/CB2 outputs and timer 1's expiry go to the
 * machine through the callbacks.
 */
class BbcVia {
public:
    std::function<uint8_t()> readPortA, readPortB;
    std::function<void(uint8_t)> writePortA, writePortB;
    std::function<void(int)> setCa2Out, setCb2Out;

    void reset();
    /** Run `cycles` 2 MHz cycles. */
    void poll(int cycles);
    uint8_t read(uint16_t addr);
    void write(uint16_t addr, uint8_t v);
    void setCa1(int level);
    void setCa2(int level);
    void setCb1(int level);
    void setCb2(int level);

    bool irq() const { return m_ifr & 0x80; }
    uint8_t ifr() const { return m_ifr; }
    uint8_t ier() const { return m_ier; }
    uint8_t ora() const { return m_ora; }
    uint8_t orb() const { return m_orb; }
    uint8_t ddra() const { return m_ddra; }
    uint8_t ddrb() const { return m_ddrb; }
    int t1Counter() const { return m_t1c; }

private:
    void updateIfr();
    void shift(int cycles);
    uint8_t portA() { return readPortA ? readPortA() : 0xFF; }
    uint8_t portB() { return readPortB ? readPortB() : 0xFF; }
    void outA(uint8_t v) { if (writePortA) writePortA(v); }
    void outB(uint8_t v) { if (writePortB) writePortB(v); }
    void ca2Out(int l) { if (setCa2Out) setCa2Out(l); }
    void cb2Out(int l) { if (setCb2Out) setCb2Out(l); }

    uint8_t m_ora = 0, m_orb = 0, m_ira = 0, m_irb = 0;
    uint8_t m_ddra = 0, m_ddrb = 0;
    uint8_t m_sr = 0, m_acr = 0, m_pcr = 0, m_ifr = 0, m_ier = 0;
    uint8_t m_t1pb7 = 0x80;
    int m_t1l = 0x1FFFE, m_t2l = 0x1FFFE, m_t1c = 0x1FFFE, m_t2c = 0x1FFFE;
    int m_t1hit = 1, m_t2hit = 1;
    int m_ca1 = 0, m_ca2 = 0, m_cb1 = 0, m_cb2 = 0;
    int m_srCount = 0;
};

}  // namespace bbc
