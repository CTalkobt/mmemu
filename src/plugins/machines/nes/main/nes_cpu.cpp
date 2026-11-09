// The Ricoh 2A03's CPU, cycle by cycle: after Mesen2 (Core/NES/NesCpu.cpp,
// NesCpu.h), GPL-3.0, Sour and the Mesen contributors. See nes_cpu.h.

#include "nes_cpu.h"
#include "libdebug/main/execution_observer.h"
#include "libtoolchain/main/idisasm.h"
#include "mmemu_plugin_api.h"

namespace nes {

// Mesen's tables: the operation and addressing mode of each opcode
const Cpu::Op Cpu::OPS[256] = {
    BRK, ORA, HLT, SLO, NOP, ORA, ASL_Memory, SLO, PHP, ORA, ASL_Acc, AAC, NOP, ORA, ASL_Memory, SLO,  // 0
    BPL, ORA, HLT, SLO, NOP, ORA, ASL_Memory, SLO, CLC, ORA, NOP, SLO, NOP, ORA, ASL_Memory, SLO,  // 1
    JSR, AND, HLT, RLA, BIT, AND, ROL_Memory, RLA, PLP, AND, ROL_Acc, AAC, BIT, AND, ROL_Memory, RLA,  // 2
    BMI, AND, HLT, RLA, NOP, AND, ROL_Memory, RLA, SEC, AND, NOP, RLA, NOP, AND, ROL_Memory, RLA,  // 3
    RTI, EOR, HLT, SRE, NOP, EOR, LSR_Memory, SRE, PHA, EOR, LSR_Acc, ASR, JMP_Abs, EOR, LSR_Memory, SRE,  // 4
    BVC, EOR, HLT, SRE, NOP, EOR, LSR_Memory, SRE, CLI, EOR, NOP, SRE, NOP, EOR, LSR_Memory, SRE,  // 5
    RTS, ADC, HLT, RRA, NOP, ADC, ROR_Memory, RRA, PLA, ADC, ROR_Acc, ARR, JMP_Ind, ADC, ROR_Memory, RRA,  // 6
    BVS, ADC, HLT, RRA, NOP, ADC, ROR_Memory, RRA, SEI, ADC, NOP, RRA, NOP, ADC, ROR_Memory, RRA,  // 7
    NOP, STA, NOP, SAX, STY, STA, STX, SAX, DEY, NOP, TXA, ANE, STY, STA, STX, SAX,  // 8
    BCC, STA, HLT, SHAZ, STY, STA, STX, SAX, TYA, STA, TXS, TAS, SHY, STA, SHX, SHAA,  // 9
    LDY, LDA, LDX, LAX, LDY, LDA, LDX, LAX, TAY, LDA, TAX, ATX, LDY, LDA, LDX, LAX,  // A
    BCS, LDA, HLT, LAX, LDY, LDA, LDX, LAX, CLV, LDA, TSX, LAS, LDY, LDA, LDX, LAX,  // B
    CPY, CPA, NOP, DCP, CPY, CPA, DEC, DCP, INY, CPA, DEX, AXS, CPY, CPA, DEC, DCP,  // C
    BNE, CPA, HLT, DCP, NOP, CPA, DEC, DCP, CLD, CPA, NOP, DCP, NOP, CPA, DEC, DCP,  // D
    CPX, SBC, NOP, ISB, CPX, SBC, INC, ISB, INX, SBC, NOP, SBC, CPX, SBC, INC, ISB,  // E
    BEQ, SBC, HLT, ISB, NOP, SBC, INC, ISB, SED, SBC, NOP, ISB, NOP, SBC, INC, ISB,  // F
};
const Cpu::Mode Cpu::MODES[256] = {
    Imp, IndX, None, IndX, Zero, Zero, Zero, Zero, Imp, Imm, Acc, Imm, Abs, Abs, Abs, Abs,  // 0
    Rel, IndY, None, IndYW, ZeroX, ZeroX, ZeroX, ZeroX, Imp, AbsY, Imp, AbsYW, AbsX, AbsX, AbsXW, AbsXW,  // 1
    Other, IndX, None, IndX, Zero, Zero, Zero, Zero, Imp, Imm, Acc, Imm, Abs, Abs, Abs, Abs,  // 2
    Rel, IndY, None, IndYW, ZeroX, ZeroX, ZeroX, ZeroX, Imp, AbsY, Imp, AbsYW, AbsX, AbsX, AbsXW, AbsXW,  // 3
    Imp, IndX, None, IndX, Zero, Zero, Zero, Zero, Imp, Imm, Acc, Imm, Abs, Abs, Abs, Abs,  // 4
    Rel, IndY, None, IndYW, ZeroX, ZeroX, ZeroX, ZeroX, Imp, AbsY, Imp, AbsYW, AbsX, AbsX, AbsXW, AbsXW,  // 5
    Imp, IndX, None, IndX, Zero, Zero, Zero, Zero, Imp, Imm, Acc, Imm, Ind, Abs, Abs, Abs,  // 6
    Rel, IndY, None, IndYW, ZeroX, ZeroX, ZeroX, ZeroX, Imp, AbsY, Imp, AbsYW, AbsX, AbsX, AbsXW, AbsXW,  // 7
    Imm, IndX, Imm, IndX, Zero, Zero, Zero, Zero, Imp, Imm, Imp, Imm, Abs, Abs, Abs, Abs,  // 8
    Rel, IndYW, None, Other, ZeroX, ZeroX, ZeroY, ZeroY, Imp, AbsYW, Imp, Other, Other, AbsXW, Other, Other,  // 9
    Imm, IndX, Imm, IndX, Zero, Zero, Zero, Zero, Imp, Imm, Imp, Imm, Abs, Abs, Abs, Abs,  // A
    Rel, IndY, None, IndY, ZeroX, ZeroX, ZeroY, ZeroY, Imp, AbsY, Imp, AbsY, AbsX, AbsX, AbsY, AbsY,  // B
    Imm, IndX, Imm, IndX, Zero, Zero, Zero, Zero, Imp, Imm, Imp, Imm, Abs, Abs, Abs, Abs,  // C
    Rel, IndY, None, IndYW, ZeroX, ZeroX, ZeroX, ZeroX, Imp, AbsY, Imp, AbsYW, AbsX, AbsX, AbsXW, AbsXW,  // D
    Imm, IndX, Imm, IndX, Zero, Zero, Zero, Zero, Imp, Imm, Imp, Imm, Abs, Abs, Abs, Abs,  // E
    Rel, IndY, None, IndYW, ZeroX, ZeroX, ZeroX, ZeroX, Imp, AbsY, Imp, AbsYW, AbsX, AbsX, AbsXW, AbsXW,  // F
};

// ----------------------------------------------------------------------------
// Power, reset
// ----------------------------------------------------------------------------

void Cpu::reset() {
    MOS6502::reset();                  // A/X/Y 0, S $FD, P I, PC from $FFFC
    m_needNmi = m_prevNeedNmi = m_prevNmiFlag = false;
    m_runIrq = m_prevRunIrq = false;
    m_needHalt = m_needDummyRead = m_spriteDma = m_dmcRunning = m_abortDmc = false;
    m_hltWarned = false;
    if (!m_sys) return;
    // The CPU takes 8 cycles before it runs the program's first instruction
    m_cycle = (uint64_t)-1;
    m_master = 12;
    for (int i = 0; i < 8; ++i) { startCycle(true); endCycle(true); }
    m_state.cycles = m_cycle + 1;
}

void Cpu::resetButton() {
    m_needHalt = m_needDummyRead = m_spriteDma = m_dmcRunning = m_abortDmc = false;
    m_hltWarned = false;
    m_state.pc = (uint16_t)(m_bus->read8(0xFFFC) | (m_bus->read8(0xFFFD) << 8));
    m_state.p |= FLAG_I;
    m_state.sp = (uint8_t)(m_state.sp - 3);
    m_cycle = (uint64_t)-1;
    m_master = 12;
    for (int i = 0; i < 8; ++i) { startCycle(true); endCycle(true); }
    m_state.cycles = m_cycle + 1;
}

// ----------------------------------------------------------------------------
// Cycles
// ----------------------------------------------------------------------------

void Cpu::startCycle(bool forRead) {
    m_master += forRead ? 5 : 7;
    ++m_cycle;
    m_sys->runPpu(m_master - PPU_OFFSET);
    m_sys->clockCycle();
}

void Cpu::endCycle(bool forRead) {
    m_master += forRead ? 7 : 5;
    m_sys->runPpu(m_master - PPU_OFFSET);

    // "The internal signal goes high during phi1 of the cycle that follows
    // the one where the edge is detected, and stays high until the NMI has
    // been handled"
    m_prevNeedNmi = m_needNmi;
    const bool nmi = m_sys->nmiLine();
    if (!m_prevNmiFlag && nmi) m_needNmi = true;
    m_prevNmiFlag = nmi;

    // "It's really the status of the interrupt lines at the end of the
    // second-to-last cycle that matters": keep the previous cycle's
    m_prevRunIrq = m_runIrq;
    m_runIrq = m_sys->irqLine() && !(m_state.p & FLAG_I);
}

uint8_t Cpu::memRead(uint16_t addr) {
    processPendingDma(addr);
    startCycle(true);
    const uint8_t v = m_sys->read(addr);
    endCycle(true);
    return v;
}

void Cpu::memWrite(uint16_t addr, uint8_t value) {
    startCycle(false);
    m_sys->write(addr, value);
    endCycle(false);
}

// ----------------------------------------------------------------------------
// DMA (NTSC; the RP2A03G/H's behaviour)
// ----------------------------------------------------------------------------

void Cpu::stopDmcDma() {
    if (!m_dmcRunning) return;
    if (m_needHalt) {
        // Before the halt cycle: cancelled outright
        m_dmcRunning = m_needDummyRead = m_needHalt = false;
    } else {
        // Abortable only within its first cycle
        m_abortDmc = true;
    }
}

void Cpu::processPendingDma(uint16_t readAddr) {
    if (!m_needHalt) return;

    uint16_t prevReadAddr = readAddr;
    const bool internalRegs = (readAddr & 0xFFE0) == 0x4000;
    const bool padAddr = readAddr == 0x4016 || readAddr == 0x4017;
    bool skipFirstInputClock = false;
    if (internalRegs && m_dmcRunning && padAddr && (m_sys->dmcAddress() & 0x1F) == (readAddr & 0x1F)) {
        // The DMC reads the same address: /OE stays active, the pads see one read
        skipFirstInputClock = true;
    }
    // The NES (not the original Famicom): only the first dummy read of
    // $4016/$4017 clocks the pads
    const bool nesBehaviour = !m_famicom;
    const bool skipDummyReads = nesBehaviour && padAddr;

    m_needHalt = false;

    // The halt cycle
    startCycle(true);
    if (m_abortDmc && nesBehaviour && padAddr) {
        // aborted, and the CPU reads $4016/$4017 next: the pads see that read only
    } else if (!skipFirstInputClock) {
        m_sys->read(readAddr);
    }
    endCycle(true);

    if (m_abortDmc) {
        m_dmcRunning = false;
        m_abortDmc = false;
        if (!m_spriteDma) {
            m_needDummyRead = false;
            return;
        }
    }

    uint16_t spriteCounter = 0;
    uint8_t spriteAddr = 0;
    uint8_t value = 0;
    auto processCycle = [this]() {
        // OAM DMA cycles count as the DMC's halt and dummy cycles when both run
        if (m_abortDmc) {
            m_dmcRunning = false;
            m_abortDmc = false;
            m_needDummyRead = false;
            m_needHalt = false;
        } else if (m_needHalt) {
            m_needHalt = false;
        } else if (m_needDummyRead) {
            m_needDummyRead = false;
        }
        startCycle(true);
    };

    while (m_dmcRunning || m_spriteDma) {
        const bool getCycle = (m_cycle & 1) == 0;
        if (getCycle) {
            if (m_dmcRunning && !m_needHalt && !m_needDummyRead) {
                // The DMC's read (its halt and dummy cycles are done)
                processCycle();
                value = dmaRead(m_sys->dmcAddress(), prevReadAddr, internalRegs);
                endCycle(true);
                m_dmcRunning = false;
                m_abortDmc = false;
                m_sys->dmcBuffer(value);
            } else if (m_spriteDma) {
                processCycle();
                value = dmaRead((uint16_t)(m_spriteDmaPage * 0x100 + spriteAddr), prevReadAddr, internalRegs);
                endCycle(true);
                ++spriteAddr;
                ++spriteCounter;
            } else {
                // The DMC is waiting for its halt/dummy cycle and OAM DMA isn't running
                processCycle();
                if (!skipDummyReads) m_sys->read(readAddr);
                endCycle(true);
            }
        } else {
            if (m_spriteDma && (spriteCounter & 1)) {
                // OAM DMA's write (after a read)
                processCycle();
                m_sys->write(0x2004, value);
                endCycle(true);
                if (++spriteCounter == 0x200) m_spriteDma = false;
            } else {
                // Alignment, before OAM DMA's first read or the DMC's
                processCycle();
                if (!skipDummyReads) m_sys->read(readAddr);
                endCycle(true);
            }
        }
    }
}

uint8_t Cpu::dmaRead(uint16_t addr, uint16_t& prevReadAddr, bool internalRegs) {
    // When the CPU was halted on a read of $4000-$401F, the DMA unit's reads
    // also reach the 2A03's internal registers: $4015 reads (which clear the
    // frame IRQ), extra pad clocks or none, and bus conflicts
    uint8_t v;
    if (!internalRegs) {
        if (addr >= 0x4000 && addr <= 0x401F) v = m_sys->openBus();    // nothing answers outside
        else v = m_sys->read(addr);
        prevReadAddr = addr;
        return v;
    }
    const uint16_t internal = (uint16_t)(0x4000 | (addr & 0x1F));
    const bool same = internal == addr;
    switch (internal) {
    case 0x4015:
        v = m_sys->read(internal);
        if (!same) m_sys->read(addr);
        break;
    case 0x4016:
    case 0x4017: {
        if (!m_famicom && prevReadAddr == internal) {
            // The same pad register twice in a row: the pad doesn't see it
            v = m_sys->openBus();
        } else {
            v = m_sys->read(internal);
        }
        if (!same) {
            const uint8_t obMask = 0xE0;           // the standard controller's open bus bits
            const uint8_t ext = m_sys->read(addr);
            v = (uint8_t)((ext & obMask) | ((v & ~obMask) & (ext & ~obMask)));
        }
        break;
    }
    default:
        v = m_sys->read(addr);
        break;
    }
    prevReadAddr = internal;
    return v;
}

// ----------------------------------------------------------------------------
// Execution
// ----------------------------------------------------------------------------

int Cpu::step() {
    if (!m_sys) return MOS6502::step();
    const uint64_t before = m_cycle;

    if (m_observer) {
        if (m_observer->needsDisasm()) {
            DisasmEntry entry;
            decodeEntry(m_bus, m_state.pc, &entry);
            if (!m_observer->onStep(this, m_bus, entry)) return 1;    // a breakpoint: not run
        } else {
            m_observer->onStepLite(this, m_state.pc);
        }
    }

    const uint8_t opcode = readByte();
    m_mode = MODES[opcode];
    m_operand = fetchOperand();
    execute(OPS[opcode]);
    if (m_prevRunIrq || m_prevNeedNmi) interrupt();

    m_state.cycles = m_cycle + 1;
    return (int)(m_cycle - before);
}

void Cpu::interrupt() {
    dummyRead();          // the opcode fetch, discarded ($00, BRK, is forced in)
    dummyRead();          // and the next byte (PC isn't incremented)
    pushWord(m_state.pc);
    const bool nmi = m_needNmi;
    if (nmi) m_needNmi = false;
    push((uint8_t)((m_state.p & ~FLAG_B) | FLAG_U));
    m_state.p |= FLAG_I;
    m_state.pc = readWordAt(nmi ? 0xFFFA : 0xFFFE);
}

void Cpu::brk() {
    pushWord((uint16_t)(m_state.pc + 1));
    const uint8_t flags = (uint8_t)(m_state.p | FLAG_B | FLAG_U);
    const bool nmi = m_needNmi;           // an NMI hijacks BRK
    if (nmi) m_needNmi = false;
    push(flags);
    m_state.p |= FLAG_I;
    m_state.pc = readWordAt(nmi ? 0xFFFA : 0xFFFE);
    // The handler's first instruction runs before an NMI (nmi_and_brk)
    m_prevNeedNmi = false;
}

uint16_t Cpu::zeroPageIndirect(uint8_t zp) {
    const uint8_t lo = memRead(zp);
    return (uint16_t)(lo | (memRead((uint8_t)(zp + 1)) << 8));
}

uint16_t Cpu::indexed(uint16_t base, uint8_t index, bool alwaysDummy) {
    const uint16_t addr = (uint16_t)(base + index);
    const bool crossed = (addr & 0xFF00) != (base & 0xFF00);
    // The read of the address before the carry into the high byte (reads
    // only make it when they cross a page)
    if (crossed || alwaysDummy) memRead((uint16_t)(addr - (crossed ? 0x100 : 0)));
    return addr;
}

uint16_t Cpu::fetchOperand() {
    switch (m_mode) {
    case Acc:
    case Imp: dummyRead(); return 0;
    case Imm:
    case Rel:
    case Zero: return readByte();
    case ZeroX: { const uint8_t z = readByte(); memRead(z); return (uint8_t)(z + m_state.x); }
    case ZeroY: { const uint8_t z = readByte(); memRead(z); return (uint8_t)(z + m_state.y); }
    case Ind:
    case Abs: return readWord();
    case IndX: { uint8_t z = readByte(); memRead(z); z = (uint8_t)(z + m_state.x); return zeroPageIndirect(z); }
    case IndY: return indexed(zeroPageIndirect(readByte()), m_state.y, false);
    case IndYW: return indexed(zeroPageIndirect(readByte()), m_state.y, true);
    case AbsX: return indexed(readWord(), m_state.x, false);
    case AbsXW: return indexed(readWord(), m_state.x, true);
    case AbsY: return indexed(readWord(), m_state.y, false);
    case AbsYW: return indexed(readWord(), m_state.y, true);
    default: return 0;
    }
}

uint8_t Cpu::operandValue() {
    return m_mode >= Zero ? memRead(m_operand) : (uint8_t)m_operand;
}

void Cpu::add(uint8_t v) {
    // The 2A03 has no decimal mode
    const uint16_t r = (uint16_t)(m_state.a + v + (m_state.p & FLAG_C));
    m_state.p &= (uint8_t)~(FLAG_C | FLAG_V);
    if (~(m_state.a ^ v) & (m_state.a ^ r) & 0x80) m_state.p |= FLAG_V;
    if (r > 0xFF) m_state.p |= FLAG_C;
    setA((uint8_t)r);
}

void Cpu::compare(uint8_t reg, uint8_t v) {
    m_state.p = (uint8_t)((m_state.p & ~FLAG_C) | (reg >= v ? FLAG_C : 0));
    setNZ((uint8_t)(reg - v));
}

uint8_t Cpu::asl(uint8_t v) {
    m_state.p = (uint8_t)((m_state.p & ~FLAG_C) | (v >> 7));
    v = (uint8_t)(v << 1);
    setNZ(v);
    return v;
}

uint8_t Cpu::lsr(uint8_t v) {
    m_state.p = (uint8_t)((m_state.p & ~FLAG_C) | (v & 1));
    v >>= 1;
    setNZ(v);
    return v;
}

uint8_t Cpu::rol(uint8_t v) {
    const uint8_t c = m_state.p & FLAG_C;
    m_state.p = (uint8_t)((m_state.p & ~FLAG_C) | (v >> 7));
    v = (uint8_t)((v << 1) | c);
    setNZ(v);
    return v;
}

uint8_t Cpu::ror(uint8_t v) {
    const uint8_t c = (m_state.p & FLAG_C) ? 0x80 : 0;
    m_state.p = (uint8_t)((m_state.p & ~FLAG_C) | (v & 1));
    v = (uint8_t)((v >> 1) | c);
    setNZ(v);
    return v;
}

template <typename F>
void Cpu::modify(F f) {
    // Read, write the value back unchanged, then write the result
    const uint16_t addr = m_operand;
    const uint8_t v = memRead(addr);
    memWrite(addr, v);
    memWrite(addr, f(v));
}

void Cpu::branch(bool taken) {
    const int8_t offset = (int8_t)m_operand;
    if (!taken) return;
    // "A taken non-page-crossing branch ignores IRQ/NMI during its last
    // clock, so that next instruction executes before the IRQ"
    if (m_runIrq && !m_prevRunIrq) m_runIrq = false;
    dummyRead();
    const uint16_t target = (uint16_t)(m_state.pc + offset);
    if ((target & 0xFF00) != (m_state.pc & 0xFF00)) dummyRead();
    m_state.pc = target;
}

void Cpu::shx(uint16_t base, uint8_t index, uint8_t value) {
    // SHA/SHX/SHY/TAS (forums.nesdev.org/viewtopic.php?p=297765)
    const bool crossed = (((base + index) & 0xFF00) != (base & 0xFF00));
    const uint64_t c = m_cycle;
    memRead((uint16_t)(base + index - (crossed ? 0x100 : 0)));
    // A DMA during the dummy read: the value isn't ANDed with the high byte
    const bool hadDma = m_cycle - c > 1;
    const uint16_t operand = (uint16_t)(base + index);
    uint8_t hi = (uint8_t)(operand >> 8);
    if (crossed) hi &= value;
    const uint8_t v = hadDma ? value : (uint8_t)(value & ((base >> 8) + 1));
    memWrite((uint16_t)((hi << 8) | (operand & 0xFF)), v);
}

void Cpu::execute(Op op) {
    CPU6502State& s = m_state;
    switch (op) {
    case LDA: setA(operandValue()); break;
    case LDX: setX(operandValue()); break;
    case LDY: setY(operandValue()); break;
    case STA: memWrite(m_operand, s.a); break;
    case STX: memWrite(m_operand, s.x); break;
    case STY: memWrite(m_operand, s.y); break;
    case AND: setA((uint8_t)(s.a & operandValue())); break;
    case EOR: setA((uint8_t)(s.a ^ operandValue())); break;
    case ORA: setA((uint8_t)(s.a | operandValue())); break;
    case ADC: add(operandValue()); break;
    case SBC: add((uint8_t)(operandValue() ^ 0xFF)); break;
    case CPA: compare(s.a, operandValue()); break;
    case CPX: compare(s.x, operandValue()); break;
    case CPY: compare(s.y, operandValue()); break;
    case BIT: {
        const uint8_t v = operandValue();
        s.p = (uint8_t)((s.p & ~(FLAG_Z | FLAG_V | FLAG_N)) | (v & (FLAG_N | FLAG_V)) | ((s.a & v) ? 0 : FLAG_Z));
        break;
    }
    case INC: modify([this](uint8_t v) { v = (uint8_t)(v + 1); setNZ(v); return v; }); break;
    case DEC: modify([this](uint8_t v) { v = (uint8_t)(v - 1); setNZ(v); return v; }); break;
    case ASL_Memory: modify([this](uint8_t v) { return asl(v); }); break;
    case LSR_Memory: modify([this](uint8_t v) { return lsr(v); }); break;
    case ROL_Memory: modify([this](uint8_t v) { return rol(v); }); break;
    case ROR_Memory: modify([this](uint8_t v) { return ror(v); }); break;
    case ASL_Acc: setA(asl(s.a)); break;
    case LSR_Acc: setA(lsr(s.a)); break;
    case ROL_Acc: setA(rol(s.a)); break;
    case ROR_Acc: setA(ror(s.a)); break;
    case INX: setX((uint8_t)(s.x + 1)); break;
    case INY: setY((uint8_t)(s.y + 1)); break;
    case DEX: setX((uint8_t)(s.x - 1)); break;
    case DEY: setY((uint8_t)(s.y - 1)); break;
    case TAX: setX(s.a); break;
    case TAY: setY(s.a); break;
    case TSX: setX(s.sp); break;
    case TXA: setA(s.x); break;
    case TXS: s.sp = s.x; break;
    case TYA: setA(s.y); break;
    case PHA: push(s.a); break;
    case PHP: push((uint8_t)(s.p | FLAG_B | FLAG_U)); break;
    case PLA: dummyRead(); setA(pop()); break;
    case PLP: dummyRead(); setP(pop()); break;
    case CLC: s.p &= (uint8_t)~FLAG_C; break;
    case CLD: s.p &= (uint8_t)~FLAG_D; break;
    case CLI: s.p &= (uint8_t)~FLAG_I; break;
    case CLV: s.p &= (uint8_t)~FLAG_V; break;
    case SEC: s.p |= FLAG_C; break;
    case SED: s.p |= FLAG_D; break;
    case SEI: s.p |= FLAG_I; break;
    case BCC: branch(!(s.p & FLAG_C)); break;
    case BCS: branch(s.p & FLAG_C); break;
    case BEQ: branch(s.p & FLAG_Z); break;
    case BNE: branch(!(s.p & FLAG_Z)); break;
    case BMI: branch(s.p & FLAG_N); break;
    case BPL: branch(!(s.p & FLAG_N)); break;
    case BVC: branch(!(s.p & FLAG_V)); break;
    case BVS: branch(s.p & FLAG_V); break;
    case JMP_Abs: s.pc = m_operand; break;
    case JMP_Ind: {
        // The high byte comes from the same page ($xxFF wraps to $xx00)
        const uint16_t a = m_operand;
        const uint8_t lo = memRead(a);
        const uint8_t hi = memRead((uint16_t)((a & 0xFF00) | ((a + 1) & 0xFF)));
        s.pc = (uint16_t)(lo | (hi << 8));
        break;
    }
    case JSR: {
        const uint8_t lo = readByte();
        dummyRead();
        pushWord(s.pc);
        const uint8_t hi = readByte();
        s.pc = (uint16_t)(lo | (hi << 8));
        break;
    }
    case RTS: {
        dummyRead();
        const uint16_t a = popWord();
        dummyRead();
        s.pc = (uint16_t)(a + 1);
        break;
    }
    case RTI: dummyRead(); setP(pop()); s.pc = popWord(); break;
    case BRK: brk(); break;
    case NOP: operandValue(); break;
    case HLT:
        // The CPU locks up: run this opcode forever, no interrupts
        s.pc = (uint16_t)(s.pc - 1);
        m_prevRunIrq = false;
        m_prevNeedNmi = false;
        if (!m_hltWarned) { m_hltWarned = true; log(SIM_LOG_WARN, "2A03: a KIL opcode locked up the CPU"); }
        break;

    // Unofficial
    case SLO: { const uint8_t v = operandValue(); memWrite(m_operand, v); const uint8_t r = asl(v); setA((uint8_t)(s.a | r)); memWrite(m_operand, r); break; }
    case SRE: { const uint8_t v = operandValue(); memWrite(m_operand, v); const uint8_t r = lsr(v); setA((uint8_t)(s.a ^ r)); memWrite(m_operand, r); break; }
    case RLA: { const uint8_t v = operandValue(); memWrite(m_operand, v); const uint8_t r = rol(v); setA((uint8_t)(s.a & r)); memWrite(m_operand, r); break; }
    case RRA: { const uint8_t v = operandValue(); memWrite(m_operand, v); const uint8_t r = ror(v); add(r); memWrite(m_operand, r); break; }
    case DCP: { uint8_t v = operandValue(); memWrite(m_operand, v); v = (uint8_t)(v - 1); compare(s.a, v); memWrite(m_operand, v); break; }
    case ISB: { uint8_t v = operandValue(); memWrite(m_operand, v); v = (uint8_t)(v + 1); add((uint8_t)(v ^ 0xFF)); memWrite(m_operand, v); break; }
    case SAX: memWrite(m_operand, (uint8_t)(s.a & s.x)); break;
    case LAX: { const uint8_t v = operandValue(); setX(v); setA(v); break; }
    case AAC: setA((uint8_t)(s.a & operandValue())); s.p = (uint8_t)((s.p & ~FLAG_C) | ((s.p & FLAG_N) ? FLAG_C : 0)); break;
    case ASR: {
        setA((uint8_t)(s.a & operandValue()));
        s.p = (uint8_t)((s.p & ~FLAG_C) | (s.a & 1));
        setA((uint8_t)(s.a >> 1));
        break;
    }
    case ARR: {
        setA((uint8_t)(((s.a & operandValue()) >> 1) | ((s.p & FLAG_C) ? 0x80 : 0)));
        s.p &= (uint8_t)~(FLAG_C | FLAG_V);
        if (s.a & 0x40) s.p |= FLAG_C;
        if (((s.p & FLAG_C) ? 1 : 0) ^ ((s.a >> 5) & 1)) s.p |= FLAG_V;
        break;
    }
    case ATX: { const uint8_t v = operandValue(); setA(v); setX(s.a); break; }
    case AXS: {
        const uint8_t m = operandValue();
        const uint8_t ax = (uint8_t)(s.a & s.x);
        s.p = (uint8_t)((s.p & ~FLAG_C) | (ax >= m ? FLAG_C : 0));
        setX((uint8_t)(ax - m));
        break;
    }
    case ANE: setA((uint8_t)((s.a | 0xEE) & s.x & operandValue())); break;
    case LAS: { const uint8_t v = (uint8_t)(operandValue() & s.sp); setA(v); setX(v); s.sp = v; break; }
    case SHY: { const uint16_t b = readWord(); shx(b, s.x, s.y); break; }
    case SHX: { const uint16_t b = readWord(); shx(b, s.y, s.x); break; }
    case SHAA: { const uint16_t b = readWord(); shx(b, s.y, (uint8_t)(s.x & s.a)); break; }
    case SHAZ: { const uint16_t b = zeroPageIndirect(readByte()); shx(b, s.y, (uint8_t)(s.x & s.a)); break; }
    case TAS: { const uint16_t b = readWord(); shx(b, s.y, (uint8_t)(s.x & s.a)); s.sp = (uint8_t)(s.x & s.a); break; }
    }
}

}  // namespace nes
