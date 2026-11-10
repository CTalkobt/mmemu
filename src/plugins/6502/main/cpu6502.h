#pragma once

#include "libcore/main/icore.h"
#include <functional>
#include <vector>

/**
 * 6502 Status Flags.
 */
#define FLAG_C 0x01
#define FLAG_Z 0x02
#define FLAG_I 0x04
#define FLAG_D 0x08
#define FLAG_B 0x10
#define FLAG_U 0x20
#define FLAG_V 0x40
#define FLAG_N 0x80

/**
 * 6502 Internal State.
 */
struct CPU6502State {
    uint8_t  a;
    uint8_t  x;
    uint8_t  y;
    uint8_t  sp;
    uint16_t pc;
    uint8_t  p;
    uint64_t cycles;
    uint8_t  irqLine;
    uint8_t  nmiLine;
    uint8_t  nmiPrev;
    uint8_t  haltLine;
    uint8_t  waiting;   // 65C02 WAI: until an interrupt line is asserted
    uint8_t  stopped;   // 65C02 STP: until reset
};

/**
 * MOS 6502 CPU Implementation.
 */
class MOS6502 : public ICore {
public:
    MOS6502();
    virtual ~MOS6502() {}

    // ISA identification
    const char* isaName()     const override { return "6502"; }
    const char* variantName() const override { return "NMOS 6502"; }
    uint32_t    isaCaps()     const override { return 0; }

    // Register descriptor table
    int                  regCount()             const override;
    const RegDescriptor* regDescriptor(int idx) const override;
    uint32_t             regRead (int idx)      const override;
    void                 regWrite(int idx, uint32_t val) override;

    // Convenience accessors
    uint32_t pc() const override { return m_state.pc; }
    void     setPc(uint32_t addr) override { m_state.pc = (uint16_t)addr; }
    uint32_t sp() const override { return 0x0100 | m_state.sp; }

    // Execution
    int  step()  override;
    uint32_t lastStepWrites() const override { return m_lastWrites; }
    void setAccessSync(std::function<void(int)> sync) override { m_sync = std::move(sync); }
    void reset() override;

    // Interrupts
    void triggerIrq()   override;
    void triggerNmi()   override;
    void triggerReset() override { reset(); }
    void setIrqLine(bool asserted) override { m_state.irqLine = asserted ? 1 : 0; }
    void setNmiLine(bool asserted) override {
        if (asserted && !m_state.nmiLine) m_nmiEdge = true;   // latched every cycle, seen at the poll
        m_state.nmiLine = asserted ? 1 : 0;
    }
    void setHaltLine(bool asserted) override { m_state.haltLine = asserted ? 1 : 0; }

    // Disassembly
    int disassembleOne  (IBus* bus, uint32_t addr, char* buf, int bufsz) override;
    int disassembleEntry(IBus* bus, uint32_t addr, void* entryOut)      override;
    int decodeEntry     (IBus* bus, uint32_t addr, void* entryOut)      override;

    // Snapshot support
    size_t stateSize()             const override { return sizeof(CPU6502State); }
    void   saveState(uint8_t* buf) const override;
    void   loadState(const uint8_t* buf) override;

    // Step-over / step-out heuristics
    int  isCallAt    (IBus* bus, uint32_t addr) override;
    bool isReturnAt  (IBus* bus, uint32_t addr) override;
    bool isProgramEnd(IBus* bus)                override;

    // Helper to write a call harness
    int writeCallHarness(IBus* bus, uint32_t scratch, uint32_t routineAddr) override;

    // Bus attachment
    void setDataBus(IBus* bus) override { m_bus = bus; }
    void setCodeBus(IBus* bus) override { m_bus = bus; }
    void setIoBus  (IBus* bus) override { (void)bus; }

    IBus* getDataBus() const override { return m_bus; }
    IBus* getCodeBus() const override { return m_bus; }
    IBus* getIoBus()   const override { return nullptr; }
    bool isHalted() const override { return m_state.haltLine; }

    uint64_t cycles() const override { return m_state.cycles; }

protected:
    // Visible to variants with their own execution (the NES's cycle-exact 2A03)
    CPU6502State m_state;
    IBus*        m_bus = nullptr;

private:

    // Internal helpers
    uint8_t  read(uint16_t addr);
    void     write(uint16_t addr, uint8_t val);
    void rmwWrite(uint16_t addr, uint8_t old, uint8_t val);
    void     push(uint8_t val);
    uint8_t  pop();
    void     updateNZ(uint8_t val);

    // Opcode execution
    enum class Mode {
        IMP, ACC, IMM, BP, BPX, BPY, ABS, ABSX, ABSY, IND, BPXIND, INDBPY, REL,
        BPIND,      // 65C02 (zp)
        ABSXIND,    // 65C02 JMP (abs,X)
        BPREL       // 65C02 BBR/BBS: zero page, then a relative branch
    };
    using OpcodeFn = int (MOS6502::*)(uint16_t addr);
    struct Opcode {
        const char* mnemonic;
        OpcodeFn    fn;
        Mode        mode;
        int         cycles;
        bool        penalty = false;   // 65C02: +1 cycle on an indexed page crossing (reads)
    };
    static const Opcode s_opcodes[256];
    static const Opcode s_opcodes65c02[256];   // cpu6502_cmos.cpp

    const Opcode* m_ops = s_opcodes;    // the variant's table
    bool     m_cmos = false;            // WDC 65C02 behaviour
    uint8_t  m_curOp = 0;               // opcode being executed (BBR/BBS/RMB/SMB bit)
    // Which cycles of an instruction write (lastStepWrites): per opcode of
    // the variant's table, built when first needed
    enum WriteKind : uint8_t { W_NONE, W_LAST, W_LAST2, W_JSR, W_BRK };
    uint8_t  m_writeKind[256] = {};
    const Opcode* m_writeKindFor = nullptr;
    uint32_t m_lastWrites = 0;
    bool     m_isRmw[256] = {};         // read-modify-write: the read 3 cycles from the end
    void buildWriteKinds();
    void interruptSequence(uint8_t p, uint16_t vector);
    // Access sync (setAccessSync): the cycle of this instruction's data
    // reads and writes (-1 outside step), and the interrupt poll
    std::function<void(int)> m_sync;
    int  m_readCycle = -1;
    int  m_writeCycle = -1;
    int  m_fetchCycle = -1;             // decoding: the cycle of the next fetch (opcode 0, operands on)
    int  m_pollCycle = 0;
    bool m_polled = true;
    bool m_irqPending = false;          // polled: take an IRQ next
    bool m_nmiPending = false;          // polled: an NMI edge
    bool m_nmiEdge = false;             // an NMI edge since the last poll (a pulse shorter than the poll interval counts)
    bool m_replay = false;              // the next read returns m_replayValue (SLO etc. use the RMW result)
    uint8_t m_replayValue = 0;
    void syncTo(int cycle);
    void pollInterrupts();
    uint16_t m_branchTarget = 0;        // BPREL: the branch target

public:
    /**
     * Called as an interrupt (IRQ, NMI, BRK) is about to fetch its vector:
     * the 65C02's VPB pin. The Commander X16 uses it to select ROM bank 0.
     */
    void setVectorPullHook(std::function<void()> fn) { m_onVectorPull = std::move(fn); }

    /**
     * The Ricoh 2A03/2A07 (NES): an NMOS 6502 whose decimal mode is cut off.
     * D can still be set and cleared, but ADC and SBC always add in binary.
     */
    void setDecimalDisabled(bool off) { m_noDecimal = off; }
    bool decimalDisabled() const { return m_noDecimal; }

    /** The "magic" constant of the unstable LXA/XAA ($AB/$8B): $EE by default, $FF on the 2A03. */
    void setUnstableMagic(uint8_t m) { m_magic = m; }

    /**
     * The 6502 polls IRQ in an instruction's last cycle, before CLI, SEI and
     * PLP change I: after CLI one more instruction runs before a pending
     * IRQ, and an IRQ can still be taken right after SEI. Off by default.
     */
    void setIrqPollLatency(bool on) { m_pollLatency = on; }

    /**
     * The RESET line (not power-on): A, X and Y are kept, S drops by 3 (the
     * suppressed pushes), I is set and PC comes from $FFFC.
     */
    void softReset();

private:
    std::function<void()> m_onVectorPull;
    bool m_noDecimal = false;           // 2A03: ADC/SBC ignore D
    uint8_t m_magic = 0xEE;             // LXA/XAA
    bool m_pollLatency = false;
    int m_polledI = -1;                 // I as polled before CLI/SEI/PLP (-1: use P)

protected:
    /** Switch to the WDC 65C02: its opcode table, decimal flags, BRK/IRQ clear D, no JMP ($xxFF) bug. */
    void enableCmos() { m_cmos = true; m_ops = s_opcodes65c02; }

private:

    // Addressing modes
    uint16_t addrBP();
    uint16_t addrBPX();
    uint16_t addrBPY();
    uint16_t addrABS();
    uint16_t addrABSX(bool& pageCrossed);
    uint16_t addrABSY(bool& pageCrossed);
    uint16_t addrIND();
    uint16_t addrBPXIND();
    uint16_t addrINDBPY(bool& pageCrossed);
    bool     pagePenalty(uint8_t op) const;
    uint16_t addrBPIND();
    uint16_t addrABSXIND();

    // 65C02 instructions (cpu6502_cmos.cpp)
    int opBRA(uint16_t addr);
    int opBBR(uint16_t addr);
    int opBBS(uint16_t addr);
    int opRMB(uint16_t addr);
    int opSMB(uint16_t addr);
    int opPHX(uint16_t addr);
    int opPHY(uint16_t addr);
    int opPLX(uint16_t addr);
    int opPLY(uint16_t addr);
    int opSTZ(uint16_t addr);
    int opTRB(uint16_t addr);
    int opTSB(uint16_t addr);
    int opINCA(uint16_t addr);
    int opDECA(uint16_t addr);
    int opBITI(uint16_t addr);
    int opWAI(uint16_t addr);
    int opSTP(uint16_t addr);
    int adcDecimalCmos(uint8_t m);
    int sbcDecimalCmos(uint8_t m);

    // Instructions
    int opADC(uint16_t addr);
    int opAND(uint16_t addr);
    int opASL(uint16_t addr);
    int opBCC(uint16_t addr);
    int opBCS(uint16_t addr);
    int opBEQ(uint16_t addr);
    int opBIT(uint16_t addr);
    int opBMI(uint16_t addr);
    int opBNE(uint16_t addr);
    int opBPL(uint16_t addr);
    int opBRK(uint16_t addr);
    int opBVC(uint16_t addr);
    int opBVS(uint16_t addr);
    int opCLC(uint16_t addr);
    int opCLD(uint16_t addr);
    int opCLI(uint16_t addr);
    int opCLV(uint16_t addr);
    int opCMP(uint16_t addr);
    int opCPX(uint16_t addr);
    int opCPY(uint16_t addr);
    int opDEC(uint16_t addr);
    int opDEX(uint16_t addr);
    int opDEY(uint16_t addr);
    int opEOR(uint16_t addr);
    int opINC(uint16_t addr);
    int opINX(uint16_t addr);
    int opINY(uint16_t addr);
    int opJMP(uint16_t addr);
    int opJSR(uint16_t addr);
    int opLDA(uint16_t addr);
    int opLDX(uint16_t addr);
    int opLDY(uint16_t addr);
    int opLSR(uint16_t addr);
    int opNOP(uint16_t addr);
    int opORA(uint16_t addr);
    int opPHA(uint16_t addr);
    int opPHP(uint16_t addr);
    int opPLA(uint16_t addr);
    int opPLP(uint16_t addr);
    int opROL(uint16_t addr);
    int opROR(uint16_t addr);
    int opRTI(uint16_t addr);
    int opRTS(uint16_t addr);
    int opSBC(uint16_t addr);
    int opSEC(uint16_t addr);
    int opSED(uint16_t addr);
    int opSEI(uint16_t addr);
    int opSTA(uint16_t addr);
    int opSTX(uint16_t addr);
    int opSTY(uint16_t addr);
    int opTAX(uint16_t addr);
    int opTAY(uint16_t addr);
    int opTSX(uint16_t addr);
    int opTXA(uint16_t addr);
    int opTXS(uint16_t addr);
    int opTYA(uint16_t addr);

    // Illegal/Undocumented opcodes
    int opKIL(uint16_t addr);
    int opSLO(uint16_t addr);
    int opRLA(uint16_t addr);
    int opSRE(uint16_t addr);
    int opRRA(uint16_t addr);
    int opSAX(uint16_t addr);
    int opLAX(uint16_t addr);
    int opDCP(uint16_t addr);
    int opISC(uint16_t addr);
    int opANC(uint16_t addr);
    int opALR(uint16_t addr);
    int opARR(uint16_t addr);
    int opXAA(uint16_t addr);
    int opAXS(uint16_t addr);
    int opAHX(uint16_t addr);
    int opTAS(uint16_t addr);
    int opSHY(uint16_t addr);
    int opSHX(uint16_t addr);
    int opLAS(uint16_t addr);
    int opLXA(uint16_t addr);
    void storeAndHigh(uint16_t addr, uint8_t index, uint8_t value);
};
