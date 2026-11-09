#pragma once

// The Ricoh 2A03's CPU, cycle by cycle (NTSC).
//
// After Mesen2 (Core/NES/NesCpu.cpp, NesCpu.h), GPL-3.0, Sour and the Mesen
// contributors. Every cycle is a bus access, as on the chip: the dummy reads
// of indexed and implied addressing, the read-modify-write instructions'
// double writes, the stack's dummy reads. Each access is split around the
// master clock (12 a CPU cycle, 4 a PPU dot) so that the PPU has run two of
// the cycle's three dots when it happens. NMI and IRQ are sampled at the end
// of every cycle, and an instruction is followed by an interrupt when the
// lines were active at the end of its second-to-last cycle. OAM DMA and the
// DMC's sample fetches halt the CPU on a read cycle and take their cycles
// there, with the DMA unit's reads of the internal registers.
//
// It is the NMOS 6502 core (MOS6502) for registers, disassembly and the
// debugger; only execution is its own.

#include "plugins/6502/main/cpu6502.h"

#include <cstdint>

namespace nes {

/** What the CPU drives: the rest of the console. */
class CpuSystem {
public:
    virtual ~CpuSystem() = default;
    /** Run the PPU up to master clock `mc` (4 master clocks a dot). */
    virtual void runPpu(uint64_t mc) = 0;
    /** One CPU cycle of the APU and the cartridge (at the start of each cycle). */
    virtual void clockCycle() = 0;
    /** A CPU (or DMA) read, with its side effects. */
    virtual uint8_t read(uint16_t addr) = 0;
    virtual void write(uint16_t addr, uint8_t value) = 0;
    /** The data bus's last value. */
    virtual uint8_t openBus() const = 0;
    /** /NMI (the PPU) and /IRQ (APU, cartridge), active high here. */
    virtual bool nmiLine() const = 0;
    virtual bool irqLine() const = 0;
    /** The DMC's next sample address, and the byte its DMA fetched. */
    virtual uint16_t dmcAddress() const = 0;
    virtual void dmcBuffer(uint8_t value) = 0;
};

class Cpu : public MOS6502 {
public:
    Cpu() = default;

    const char* variantName() const override { return "Ricoh 2A03"; }

    void setSystem(CpuSystem* sys) { m_sys = sys; }
    /** The original Famicom (HVC-001): every DMA dummy read of $4016/$4017 clocks the pads. */
    void setFamicom(bool on) { m_famicom = on; }

    /** One instruction, or an interrupt's 7 cycles; returns the CPU cycles taken (DMA included). */
    int step() override;
    /** Power on: registers cleared, PC from $FFFC, then the 8 cycles before the first instruction. */
    void reset() override;
    /** The RESET line: A/X/Y kept, S drops by 3, I set. */
    void resetButton();

    /** CPU cycles since power-on (the first is 0). */
    uint64_t cycleCount() const { return m_cycle; }

    /** A $4014 write: OAM DMA from page `page`, starting on the next read cycle. */
    void startOamDma(uint8_t page) { m_spriteDma = true; m_spriteDmaPage = page; m_needHalt = true; }
    /** The DMC wants a sample byte. */
    void startDmcDma() { m_dmcRunning = true; m_needDummyRead = true; m_needHalt = true; }
    /** The DMC was disabled: cancel a fetch not yet started, or abort one in its first cycle. */
    void stopDmcDma();

private:
    enum Mode : uint8_t {
        None, Acc, Imp, Imm, Rel,
        Zero, Abs, ZeroX, ZeroY,
        Ind, IndX, IndY, IndYW,
        AbsX, AbsXW, AbsY, AbsYW,
        Other
    };
    enum Op : uint8_t {
        AAC, ADC, AND, ANE, ARR, ASL_Acc, ASL_Memory, ASR, ATX, AXS, BCC, BCS, BEQ, BIT, BMI, BNE, BPL,
        BRK, BVC, BVS, CLC, CLD, CLI, CLV, CPA, CPX, CPY, DCP, DEC, DEX, DEY, EOR, HLT, INC, INX, INY,
        ISB, JMP_Abs, JMP_Ind, JSR, LAS, LAX, LDA, LDX, LDY, LSR_Acc, LSR_Memory, NOP, ORA, PHA, PHP,
        PLA, PLP, RLA, ROL_Acc, ROL_Memory, ROR_Acc, ROR_Memory, RRA, RTI, RTS, SAX, SBC, SEC, SED,
        SEI, SHAA, SHAZ, SHX, SHY, SLO, SRE, STA, STX, STY, TAS, TAX, TAY, TSX, TXA, TXS, TYA
    };
    static const Op OPS[256];
    static const Mode MODES[256];

    // One bus cycle each
    uint8_t memRead(uint16_t addr);
    void memWrite(uint16_t addr, uint8_t value);
    void startCycle(bool forRead);
    void endCycle(bool forRead);
    void processPendingDma(uint16_t readAddr);
    uint8_t dmaRead(uint16_t addr, uint16_t& prevReadAddr, bool internalRegs);

    void dummyRead() { memRead(m_state.pc); }
    uint8_t readByte() { return memRead(m_state.pc++); }
    uint16_t readWord() { const uint8_t lo = readByte(); return (uint16_t)(lo | (readByte() << 8)); }
    uint16_t readWordAt(uint16_t a) { const uint8_t lo = memRead(a); return (uint16_t)(lo | (memRead((uint16_t)(a + 1)) << 8)); }
    void push(uint8_t v) { memWrite((uint16_t)(0x100 | m_state.sp), v); --m_state.sp; }
    void pushWord(uint16_t v) { push((uint8_t)(v >> 8)); push((uint8_t)v); }
    uint8_t pop() { ++m_state.sp; return memRead((uint16_t)(0x100 | m_state.sp)); }
    uint16_t popWord() { const uint8_t lo = pop(); return (uint16_t)(lo | (pop() << 8)); }

    uint16_t fetchOperand();
    uint8_t operandValue();
    uint16_t zeroPageIndirect(uint8_t zp);
    uint16_t indexed(uint16_t base, uint8_t index, bool alwaysDummy);

    void setNZ(uint8_t v) { m_state.p = (uint8_t)((m_state.p & ~(FLAG_N | FLAG_Z)) | (v & 0x80) | (v ? 0 : FLAG_Z)); }
    void setA(uint8_t v) { m_state.a = v; setNZ(v); }
    void setX(uint8_t v) { m_state.x = v; setNZ(v); }
    void setY(uint8_t v) { m_state.y = v; setNZ(v); }
    void setP(uint8_t v) { m_state.p = (uint8_t)((v & 0xCF) | FLAG_U); }
    void add(uint8_t v);
    void compare(uint8_t reg, uint8_t v);
    uint8_t asl(uint8_t v);
    uint8_t lsr(uint8_t v);
    uint8_t rol(uint8_t v);
    uint8_t ror(uint8_t v);
    template <typename F> void modify(F f);
    void branch(bool taken);
    void shx(uint16_t base, uint8_t index, uint8_t value);
    void brk();
    void interrupt();
    void execute(Op op);

    CpuSystem* m_sys = nullptr;
    bool m_famicom = false;
    Mode m_mode = None;
    uint16_t m_operand = 0;

    uint64_t m_cycle = 0;          // the current cycle (first: 0)
    uint64_t m_master = 0;         // master clocks
    static constexpr uint64_t PPU_OFFSET = 1;

    // Interrupts: the NMI edge and the IRQ level, a cycle behind
    bool m_prevNmiFlag = false, m_needNmi = false, m_prevNeedNmi = false;
    bool m_runIrq = false, m_prevRunIrq = false;
    bool m_hltWarned = false;

    // DMA
    bool m_needHalt = false, m_needDummyRead = false;
    bool m_spriteDma = false, m_dmcRunning = false, m_abortDmc = false;
    uint8_t m_spriteDmaPage = 0;
};

}  // namespace nes
