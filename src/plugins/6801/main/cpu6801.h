#pragma once

#include "libcore/main/icore.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

/**
 * Motorola MC6801 / MC6803 microcomputer (#333, the TRS-80 MC-10's CPU).
 *
 * After the MC6801/MC6803 data sheet and the MC6801 Reference Manual
 * (Motorola): the 6800 instruction set with the 6801's additions (MUL,
 * ABX, ADDD/SUBD/LDD/STD, PSHX/PULX, LSRD/ASLD, BRN, JSR direct, CPX with
 * all four flags), the data sheet's cycle counts, and the undocumented
 * opcodes as XRoar's mc6801.c documents them (from hardware tests).
 *
 * The on-chip peripherals:
 *   $00/$01 port 1 / port 2 data direction, $02/$03 their data
 *   $04-$07 ports 3 and 4 (internal only in the single-chip modes)
 *   $08 TCSR, $09/$0A the free-running counter, $0B/$0C output compare,
 *   $0D/$0E input capture, $0F port 3 control/status
 *   $10 RMCR, $11 TRCSR, $12 RDR, $13 TDR (the SCI)
 *   $14 RAM control (RAME, STBY PWR)
 *   $80-$FF the 128 bytes of RAM (while RAME is set and the mode has it)
 * The mode (PC2-PC0 on P22-P20 at /RESET) is setModePins(); it reads back
 * in port 2's data register bits 7-5. The MC6803 has no ROM: mode 2 (the
 * MC-10's: expanded multiplexed, internal RAM) or 3 (no internal RAM).
 * The MC6801 has 2 KB of ROM at $F800 in modes 5-7 (setInternalRom).
 *
 * Timing: step() runs one instruction (or interrupt sequence) and returns
 * its E cycles. Each bus cycle the core makes ticks the counter (TOF at
 * $FFFF, OCF when it matches the output compare register, the compare
 * inhibited for the cycle after an OCR write) and the SCI's rate
 * generator; cycles without a bus access tick at the end of the step.
 * cycleInStep() tells a machine which cycle of the instruction an external
 * access is in.
 *
 * Interrupts: /NMI is edge-triggered, /IRQ1 level-sensitive (setIrqLine);
 * IRQ2 is the timer's ICF, OCF and TOF and the SCI, each with its enable.
 * Priority NMI, IRQ1, ICF, OCF, TOF, SCI; vectors $FFFC, $FFF8, $FFF6,
 * $FFF4, $FFF2, $FFF0 (SWI $FFFA, /RESET $FFFE). CLI (or TAP clearing I)
 * lets an interrupt in only after the next instruction.
 */
class M6801 : public ICore {
public:
    enum class Variant { MC6801, MC6803 };
    explicit M6801(Variant v = Variant::MC6803);

    enum Reg { R_A, R_B, R_D, R_X, R_SP, R_PC, R_CC, R_COUNT };
    static constexpr uint8_t CC_C = 0x01, CC_V = 0x02, CC_Z = 0x04, CC_N = 0x08, CC_I = 0x10, CC_H = 0x20;

    // Internal registers (offsets from $0000)
    enum : uint8_t {
        P1DDR = 0x00, P2DDR = 0x01, P1DR = 0x02, P2DR = 0x03, P3DDR = 0x04, P4DDR = 0x05, P3DR = 0x06, P4DR = 0x07,
        TCSR = 0x08, CRH = 0x09, CRL = 0x0A, OCRH = 0x0B, OCRL = 0x0C, ICRH = 0x0D, ICRL = 0x0E, P3CSR = 0x0F,
        RMCR = 0x10, TRCSR = 0x11, RDR = 0x12, TDR = 0x13, RAMC = 0x14,
    };
    // TCSR bits
    static constexpr uint8_t TCSR_OLVL = 0x01, TCSR_IEDG = 0x02, TCSR_ETOI = 0x04, TCSR_EOCI = 0x08,
                             TCSR_EICI = 0x10, TCSR_TOF = 0x20, TCSR_OCF = 0x40, TCSR_ICF = 0x80;
    // TRCSR bits
    static constexpr uint8_t TR_WU = 0x01, TR_TE = 0x02, TR_TIE = 0x04, TR_RE = 0x08, TR_RIE = 0x10,
                             TR_TDRE = 0x20, TR_ORFE = 0x40, TR_RDRF = 0x80;
    // Vectors
    static constexpr uint16_t VEC_SCI = 0xFFF0, VEC_TOF = 0xFFF2, VEC_OCF = 0xFFF4, VEC_ICF = 0xFFF6,
                              VEC_IRQ1 = 0xFFF8, VEC_SWI = 0xFFFA, VEC_NMI = 0xFFFC, VEC_RESET = 0xFFFE;

    const char* isaName()     const override { return "6803"; }
    const char* variantName() const override { return m_variant == Variant::MC6801 ? "Motorola MC6801" : "Motorola MC6803"; }
    uint32_t    isaCaps()     const override { return 0; }
    Variant variant() const { return m_variant; }

    int                  regCount()             const override { return R_COUNT; }
    const RegDescriptor* regDescriptor(int idx) const override;
    uint32_t             regRead (int idx)      const override;
    void                 regWrite(int idx, uint32_t val) override;

    uint32_t pc() const override { return m_pc; }
    void     setPc(uint32_t addr) override { m_pc = (uint16_t)addr; m_wait = false; }
    uint32_t sp() const override { return m_sp; }

    int  step() override;
    /** /RESET: the mode latched from setModePins(), the ports' DDRs and the
     *  timer and SCI as the data sheet gives them, the vector at $FFFE. */
    void reset() override;

    void triggerIrq()   override { m_irq1 = true; }
    void triggerNmi()   override { m_nmiPending = true; }
    void triggerReset() override { reset(); }
    /** /IRQ1 (level-sensitive). */
    void setIrqLine(bool asserted) override { m_irq1 = asserted; }
    /** /NMI: taken on the asserting edge. */
    void setNmiLine(bool asserted) override { if (asserted && !m_nmiLine) m_nmiPending = true; m_nmiLine = asserted; }
    bool isHalted() const override { return m_wait || m_hcf; }

    int  disassembleOne  (IBus* bus, uint32_t addr, char* buf, int bufsz) override;
    int  disassembleEntry(IBus* bus, uint32_t addr, void* entryOut) override;

    int  isCallAt    (IBus* bus, uint32_t addr) override;
    bool isReturnAt  (IBus* bus, uint32_t addr) override;
    bool isProgramEnd(IBus* bus) override;
    int  writeCallHarness(IBus* bus, uint32_t scratch, uint32_t routineAddr) override;

    void setDataBus(IBus* bus) override { m_bus = bus; }
    void setCodeBus(IBus* bus) override { (void)bus; }
    void setIoBus  (IBus* bus) override { (void)bus; }
    IBus* getDataBus() const override { return m_bus; }
    IBus* getCodeBus() const override { return m_bus; }
    IBus* getIoBus()   const override { return nullptr; }
    uint64_t cycles() const override { return m_cycles; }

    size_t stateSize() const override;
    void   saveState(uint8_t* buf) const override;
    void   loadState(const uint8_t* buf) override;

    // --- Configuration ------------------------------------------------------
    /** PC2-PC0 as P22-P20 present them at /RESET (0-7; the MC-10: 2). */
    void setModePins(uint8_t mode) { m_modePins = mode & 7; }
    /** The mode latched at the last /RESET. */
    uint8_t mode() const { return m_mode; }
    /** The MC6801's mask ROM (2 KB at $F800 in modes 5, 6 and 7). */
    void setInternalRom(const std::vector<uint8_t>& rom) { m_rom = rom; }

    // --- The address space as the chip decodes it -----------------------------
    /** True when `addr` is the chip's own (a register or the RAM) in this mode. */
    bool isInternal(uint16_t addr) const;
    /** An internal location without side effects (false if `addr` is external). */
    bool peekInternal(uint16_t addr, uint8_t& v) const;
    /** An internal location as the CPU reads it (flags cleared, port 2's preRead). */
    bool readInternal(uint16_t addr, uint8_t& v);
    /** An internal location as the CPU writes it. */
    bool writeInternal(uint16_t addr, uint8_t v);
    /** The 128 bytes of RAM. */
    const uint8_t* internalRam() const { return m_ram; }

    // --- Ports ----------------------------------------------------------------
    /** An I/O port (1-4): what the outside drives on its input pins, a
     *  callback before the CPU reads its data register and after it writes
     *  the data or direction register. */
    struct Port {
        uint8_t ddr = 0, data = 0;
        uint8_t in = 0xFF;                   ///< levels driven on the pins from outside
        std::function<void()> preRead;
        std::function<void()> postWrite;
        /** The pins' levels: outputs from `data`, inputs from `in`. */
        uint8_t pins() const { return (uint8_t)((data & ddr) | (in & ~ddr)); }
        /** Pins driven by the chip: the data bits, 1 where an input floats. */
        uint8_t out() const { return (uint8_t)((data & ddr) | ~ddr); }
    };
    Port& port(int n) { return m_port[(n - 1) & 3]; }
    const Port& port(int n) const { return m_port[(n - 1) & 3]; }
    /** Port 2's pins as the chip drives them: P24 the SCI's TX while TE, the
     *  data register's bits elsewhere (1 where an input floats). */
    uint8_t port2Out() const;
    /** Port 2's input pins changed (P20 is the timer's Tin: an IEDG edge
     *  captures the counter). */
    void setPort2Input(uint8_t in);

    // --- Timer and SCI ----------------------------------------------------------
    uint16_t counter() const { return m_counter; }
    uint8_t tcsr() const { return m_tcsr; }
    uint16_t outputCompare() const { return m_ocr; }
    uint16_t inputCapture() const { return m_icr; }
    uint8_t trcsr() const { return m_trcsr; }
    uint8_t rmcr() const { return m_rmcr; }
    /** The SCI's TX line (P24). */
    bool txLine() const { return m_tx; }
    /** The SCI's bit time in E cycles (0 with the external clock). */
    int sciBitCycles() const;
    /** The TX line changed (P24, while TE). */
    std::function<void(bool level)> onTxLine;
    /** A character left the transmitter (at its stop bit). */
    std::function<void(uint8_t byte)> onTxByte;
    /** Queue a character to arrive on RX (P23) at the SCI's bit rate. */
    void receiveByte(uint8_t v) { m_rxQueue.push_back(v); }
    size_t rxQueued() const { return m_rxQueue.size() + (m_rxGenBit >= 0 ? 1 : 0); }
    /** One external clock edge for the SCI (RMCR CC = 11: 8 per bit). */
    void sciExternalClock();

    // --- State --------------------------------------------------------------------
    uint8_t a() const { return m_a; }
    uint8_t b() const { return m_b; }
    uint8_t cc() const { return m_cc; }
    uint16_t d() const { return (uint16_t)(m_a << 8 | m_b); }
    uint16_t x() const { return m_x; }
    /** Which bus cycle of the current step the core is in (1 = the first). */
    int cycleInStep() const { return m_sub; }
    /** Stopped by an HCF opcode ($4E, $5E): only /RESET restarts it. */
    bool halted() const { return m_hcf; }

private:
    // Bus cycles
    void tick();                       // one E cycle of the timer and SCI
    uint8_t rd(uint16_t a);
    void wr(uint16_t a, uint8_t v);
    uint16_t rd16(uint16_t a) { uint8_t h = rd(a); return (uint16_t)(h << 8 | rd((uint16_t)(a + 1))); }
    void wr16(uint16_t a, uint16_t v) { wr(a, (uint8_t)(v >> 8)); wr((uint16_t)(a + 1), (uint8_t)v); }
    uint8_t fetch() { return rd(m_pc++); }
    uint16_t fetch16() { uint16_t v = rd16(m_pc); m_pc = (uint16_t)(m_pc + 2); return v; }
    void push8(uint8_t v) { wr(m_sp, v); m_sp = (uint16_t)(m_sp - 1); }
    void push16(uint16_t v) { push8((uint8_t)v); push8((uint8_t)(v >> 8)); }
    uint8_t pull8() { m_sp = (uint16_t)(m_sp + 1); return rd(m_sp); }
    uint16_t pull16() { uint8_t h = pull8(); return (uint16_t)(h << 8 | pull8()); }
    void setD(uint16_t v) { m_a = (uint8_t)(v >> 8); m_b = (uint8_t)v; }

    // Flags
    void nz8(uint8_t v) { m_cc = (uint8_t)((m_cc & ~(CC_N | CC_Z)) | (v & 0x80 ? CC_N : 0) | (v ? 0 : CC_Z)); }
    void nz16(uint16_t v) { m_cc = (uint8_t)((m_cc & ~(CC_N | CC_Z)) | (v & 0x8000 ? CC_N : 0) | (v ? 0 : CC_Z)); }
    void flag(uint8_t f, bool on) { if (on) m_cc |= f; else m_cc &= (uint8_t)~f; }
    void vFromNC() { flag(CC_V, ((m_cc & CC_N) != 0) != ((m_cc & CC_C) != 0)); }

    // Arithmetic
    uint8_t add8(uint8_t a, uint8_t b, int carry);
    uint8_t sub8(uint8_t a, uint8_t b, int borrow);
    uint16_t add16(uint16_t a, uint16_t b);
    uint16_t sub16(uint16_t a, uint16_t b);
    uint8_t rmw(int op, uint8_t v, bool& store);
    uint8_t logic(uint8_t r) { nz8(r); flag(CC_V, false); return r; }
    bool branchTaken(int cond) const;

    int exec(uint8_t op);
    int interrupt();                   // 0 if none taken
    int takeInterrupt(uint16_t vec, bool stack);
    bool irq2() const;
    uint16_t irq2Vector() const;

    // Peripherals
    void sciBit();
    void portWritten(int n);
    bool romMapped(uint16_t a) const;
    bool ramMapped(uint16_t a) const;
    bool regMapped(uint16_t a) const;

    Variant m_variant;
    IBus* m_bus = nullptr;
    uint8_t m_a = 0, m_b = 0, m_cc = 0xC0 | CC_I;
    uint16_t m_x = 0, m_sp = 0, m_pc = 0;
    bool m_irq1 = false, m_nmiLine = false, m_nmiPending = false;
    bool m_wait = false, m_hcf = false;
    bool m_iDelay = false;             // CLI: interrupts wait one more instruction
    uint64_t m_cycles = 0;
    int m_sub = 0;                     // bus cycles so far in this step

    // Mode, RAM, ROM
    uint8_t m_modePins = 2, m_mode = 2;
    uint8_t m_ramc = 0xC0;
    uint8_t m_ram[128] = {};
    std::vector<uint8_t> m_rom;

    // Ports
    Port m_port[4];
    uint8_t m_p3csr = 0;
    bool m_tin = true;                 // P20's last level, for input capture edges

    // Timer
    uint16_t m_counter = 0, m_ocr = 0xFFFF, m_icr = 0;
    uint8_t m_tcsr = 0;
    uint8_t m_tcsrRead = 0;            // flags seen set by a TCSR read (the first step of clearing them)
    uint8_t m_lsbLatch = 0;            // counter LSB latched by reading the MSB
    bool m_ocInhibit = false;

    // SCI
    uint8_t m_rmcr = 0, m_trcsr = TR_TDRE, m_rdr = 0, m_tdr = 0;
    uint8_t m_trRead = 0;              // TRCSR flags seen set by a read
    int m_sciDiv = 0;                  // E cycles toward the next bit time
    int m_extClocks = 0;
    bool m_tx = true;
    int m_txBits = 0;                  // 0 idle / preamble state, 1-9 shifting
    bool m_txPreamble = false;
    int m_preambleBits = 0;
    uint16_t m_tsr = 0;
    uint8_t m_txByte = 0;
    int m_rxBits = 0;
    uint8_t m_rsr = 0;
    int m_wakeOnes = 0;
    // The queued characters as a line (receiveByte)
    std::deque<uint8_t> m_rxQueue;
    int m_rxGenBit = -1;               // -1 idle, 0 start, 1-8 data, 9 stop
    uint8_t m_rxGenByte = 0;
    bool rxLine() const;
};
