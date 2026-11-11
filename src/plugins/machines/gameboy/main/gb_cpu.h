#pragma once

// The Game Boy's CPU, the Sharp SM83, after Mesen2 (Core/Gameboy/GbCpu.cpp;
// GPL-3.0, Sour and the Mesen contributors): every opcode, timed per machine
// cycle as Mesen (the machine cycle runs before the access in it), with the
// HALT bug, the interrupt dispatch that re-reads IE/IF part way (ie_push),
// EI's delay, STOP until a button, and the illegal opcodes locking up.

#include "gb_ppu.h"
#include <cstdint>

namespace gb {

class Gameboy;

class Cpu {
public:
    enum Flag : uint8_t { FLAG_Z = 0x80, FLAG_N = 0x40, FLAG_H = 0x20, FLAG_C = 0x10 };

    struct State {
        uint64_t cycles = 0;
        uint16_t pc = 0, sp = 0xFFFF;
        uint16_t haltCounter = 0;
        uint8_t a = 0, f = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0;
        bool eiPending = false, ime = false, haltBug = false, stopped = false;
    };

    void attach(Gameboy* gb, Ppu* ppu) { m_gb = gb; m_ppu = ppu; }
    /** As at power on: PC $0000, SP $FFFF, the clock at 6 (Mesen's, for boot_sclk_align). */
    void init();
    /** The first machine cycle's start (before the first instruction). */
    void powerOn() { nextCycleStart(); }
    /** One instruction (or interrupt dispatch, or a halted step). */
    void exec();

    State& state() { return m_s; }
    const State& state() const { return m_s; }
    uint64_t cycles() const { return m_s.cycles; }
    uint64_t& cycleRef() { return m_s.cycles; }
    bool halted() const { return m_s.haltCounter > 0; }

    // For the debugger (Sm83Core)
    /** The next exec() starts with an interrupt dispatch. */
    bool irqDispatchPending() const { return m_prevIrq && !m_s.haltBug && !m_s.stopped && m_s.ime; }
    /** On: exec() returns after an interrupt dispatch, before the handler's first instruction
     *  (the timing is unchanged: the next exec() runs it). */
    void setSplitDispatch(bool on) { m_splitDispatch = on; }

private:
    void execOp(uint8_t op);
    void execCb();
    void nextCycleStart();
    void haltBugStep();
    bool stoppedStep();

    void cycle();
    void masterCycle();
    uint8_t readOp();
    uint8_t readCode();
    uint16_t readCodeWord();
    template <OamCorruption C = OamCorruption::Read> uint8_t read(uint16_t addr);
    void write(uint16_t addr, uint8_t v);
    void pushByte(uint8_t v);
    void pushWord(uint16_t v);
    uint16_t popWord();

    uint16_t bc() const { return (uint16_t)(m_s.b << 8 | m_s.c); }
    uint16_t de() const { return (uint16_t)(m_s.d << 8 | m_s.e); }
    uint16_t hl() const { return (uint16_t)(m_s.h << 8 | m_s.l); }
    uint16_t af() const { return (uint16_t)(m_s.a << 8 | m_s.f); }
    void setBc(uint16_t v) { m_s.b = (uint8_t)(v >> 8); m_s.c = (uint8_t)v; }
    void setDe(uint16_t v) { m_s.d = (uint8_t)(v >> 8); m_s.e = (uint8_t)v; }
    void setHl(uint16_t v) { m_s.h = (uint8_t)(v >> 8); m_s.l = (uint8_t)v; }
    /** 16-bit register `i` (0 BC, 1 DE, 2 HL, 3 SP). */
    uint16_t rr(int i) const;
    void setRr(int i, uint16_t v);
    /** 8-bit register `i` of B C D E H L - A (6 is (HL), not here). */
    uint8_t& r8(int i);
    void setFlag(uint8_t f, bool on) { m_s.f = on ? (uint8_t)(m_s.f | f) : (uint8_t)(m_s.f & ~f); }
    bool flag(uint8_t f) const { return (m_s.f & f) != 0; }
    bool cond(int cc) const;

    // The ALU (Mesen's)
    void add(uint8_t v);
    void adc(uint8_t v);
    void sub(uint8_t v);
    void sbc(uint8_t v);
    void andA(uint8_t v);
    void orA(uint8_t v);
    void xorA(uint8_t v);
    void cp(uint8_t v);
    void inc(uint8_t& v);
    void dec(uint8_t& v);
    void daa();
    /** The CB rotates and shifts: 0 RLC .. 7 SRL. */
    void shiftOp(int op, uint8_t& v);
    void addHl(uint16_t v);
    void incRr(int i);
    void decRr(int i);
    void ret();
    void call(uint16_t to);

    void halt();
    void stop();
    void invalidOp();

    Gameboy* m_gb = nullptr;
    Ppu* m_ppu = nullptr;
    State m_s;
    uint8_t m_prevIrq = 0;
    bool m_splitDispatch = false;
};

}  // namespace gb
