#pragma once

#include "cpu6809.h"
#include <cstdint>

/**
 * Hitachi HD6309 CPU core (#332, the CoCo 3's drop-in 6809 upgrade).
 *
 * A superset of the MC6809: the extra accumulators E and F (W = E:F,
 * Q = D:W), the transfer register V, the zero register, and the mode
 * register MD (bit 0 NM native mode, bit 1 FM FIRQ saves the entire state,
 * bit 6 IL illegal-instruction trap, bit 7 /0 divide-by-zero trap).
 *
 * Every 6309 instruction is implemented: the page-$10/$11 additions (E, F,
 * W, D and Q arithmetic and loads/stores, the inter-register ops ADDR ...
 * CMPR, PSHSW/PULSW/PSHUW/PULUW, TFM in its four forms, interruptible and
 * restartable, the bit ops BAND ... STBT, MULD, DIVD, DIVQ, LDMD, BITMD),
 * SEXW, AIM/OIM/EIM/TIM, the W indexed modes (,W ,W++ ,--W n16,W and their
 * indirect forms), E/F/W accumulator offsets, TFR/EXG with the 6309's
 * register numbering, and the illegal-instruction and divide-by-zero traps
 * through the vector at $FFF0. The seven undefined indexed postbytes
 * ([,-R] and [n16] with R bits other than 00), and a page prefix followed by
 * another page prefix, take the illegal-instruction trap too (hoglet67).
 * DAA has the 6309's own corrections when C is set and A is $80-$99, and in
 * the inter-register ops PC reads one ahead and H is left alone (hoglet67).
 *
 * Timing is counted bus cycle by bus cycle, after XRoar's HD6309 (whose
 * sequences follow David Banks' measurements of real parts): in emulation
 * mode (NM = 0) the counts are the 6809's, in native mode (NM = 1) the
 * reduced 6309 counts, and the native-mode interrupt frames include E and F
 * (two bytes longer, RTI pulls them back). Where XRoar and MAME both differ
 * from Darren Atkinson's "6809 and 6309 Programming Reference" only by an
 * acknowledged guess (AIM/OIM/EIM, TSTE/TSTF) the reference is followed;
 * TIM, ORCC/ANDCC and JSR indexed follow hoglet67's measured table.
 *
 * The interface is M6809's: construct with new HD6309(), set the data bus,
 * step() returns the cycles used.
 */
class HD6309 : public M6809 {
public:
    HD6309();

    /** The 6309 registers after the 6809's (R_A .. R_CC keep their indices). */
    enum Reg6309 { R_E = R_COUNT, R_F, R_W, R_Q, R_V, R_MD, R6309_COUNT };
    static constexpr uint8_t MD_NM = 0x01, MD_FM = 0x02, MD_IL = 0x40, MD_DZ = 0x80;

    const char* isaName()     const override { return "6309"; }
    const char* variantName() const override { return "Hitachi HD6309"; }

    int                  regCount()             const override { return R6309_COUNT; }
    const RegDescriptor* regDescriptor(int idx) const override;
    uint32_t             regRead (int idx)      const override;
    void                 regWrite(int idx, uint32_t val) override;

    int  step() override;
    /** /RESET: MD cleared (emulation mode), DP cleared, I and F set. */
    void reset() override;

    int  disassembleOne  (IBus* bus, uint32_t addr, char* buf, int bufsz) override;
    int  disassembleEntry(IBus* bus, uint32_t addr, void* entryOut) override;

    size_t stateSize() const override { return 32; }
    void   saveState(uint8_t* buf) const override;
    void   loadState(const uint8_t* buf) override;

    uint8_t  e()  const { return m_e; }
    uint8_t  f()  const { return m_f; }
    uint16_t w()  const { return (uint16_t)(m_e << 8 | m_f); }
    uint32_t q()  const { return (uint32_t)d() << 16 | w(); }
    uint16_t v()  const { return m_v; }
    uint8_t  md() const { return m_md; }
    bool nativeMode() const { return m_md & MD_NM; }
    /** True while a TFM is part way through its transfer. */
    bool tfmActive() const { return m_tfmActive; }

private:
    // Bus-cycle-counted memory access; every access and dead cycle adds to m_cyc
    uint8_t  r8(uint16_t a) { ++m_cyc; return rd(a); }
    void     w8(uint16_t a, uint8_t v) { ++m_cyc; wr(a, v); }
    uint16_t r16(uint16_t a) { const uint8_t h = r8(a); return (uint16_t)(h << 8 | r8((uint16_t)(a + 1))); }
    void     w16(uint16_t a, uint16_t v) { w8(a, (uint8_t)(v >> 8)); w8((uint16_t)(a + 1), (uint8_t)v); }
    uint8_t  imm8() { return r8(m_pc++); }
    uint16_t imm16() { const uint8_t h = imm8(); return (uint16_t)(h << 8 | imm8()); }
    void     idle(int n = 1) { m_cyc += n; }
    void     emu(int n = 1) { if (!nativeMode()) m_cyc += n; }      // a 6809 dead cycle native mode drops
    void     psh8(uint16_t& sp, uint8_t v) { w8(--sp, v); }
    void     psh16(uint16_t& sp, uint16_t v) { psh8(sp, (uint8_t)v); psh8(sp, (uint8_t)(v >> 8)); }
    uint8_t  pul8(uint16_t& sp) { return r8(sp++); }
    uint16_t pul16(uint16_t& sp) { const uint8_t h = pul8(sp); return (uint16_t)(h << 8 | pul8(sp)); }
    void     setW(uint16_t v) { m_e = (uint8_t)(v >> 8); m_f = (uint8_t)v; }

    // Effective addresses (mode 1 direct, 2 indexed, 3 extended) and operands (mode 0 immediate)
    uint16_t eaDirect();
    uint16_t eaExtended();
    uint16_t eaIndexed();
    uint16_t ea(int mode);
    uint8_t  operand8(int mode);
    uint16_t operand16(int mode);

    // 16-bit arithmetic the 6809 core lacks
    uint16_t adc16(uint16_t a, uint16_t b, int carry);
    uint16_t sbc16(uint16_t a, uint16_t b, int borrow);
    uint16_t logic16(uint16_t r) { nz16(r); flag(CC_V, false); return r; }
    uint8_t  logic8(uint8_t r) { nz8(r); flag(CC_V, false); return r; }
    uint16_t rmw16(int op, uint16_t v);

    // TFR/EXG and inter-register operands, by the 6309's register codes
    uint16_t tfrRead(int code) const;
    void     tfrWrite(int code, uint16_t v);
    uint16_t reg16(int code) const;                 // TFM pointers: 0 D, 1 X, 2 Y, 3 U, 4 S
    void     setReg16(int code, uint16_t v);

    // Interrupts and traps
    void stackEntire();
    void stackFast();
    void vectorTo(uint8_t mask, int vec);
    void illegal();
    /** An undefined indexed postbyte: the illegal-instruction trap, then the
     *  rest of the instruction abandoned (thrown to step()). */
    [[noreturn]] void illegalPostbyte();
    struct Abandon {};
    bool serviceInterrupt();
    void tfmStep();
    void endTfm();

    void execPage0(uint8_t op);
    void execPage2(uint8_t op);
    void execPage3(uint8_t op);
    void interRegister(uint8_t op);
    void bitOp(uint8_t op);
    void divd(int mode);
    void divq(int mode);

    uint8_t  m_e = 0xFF, m_f = 0xFF, m_md = 0;
    uint16_t m_v = 0xFFFF;
    uint8_t  m_m = 0;                               // the internal "M" latch ADDR DP,r reads (hoglet67)
    int      m_cyc = 0;
    bool     m_tfmActive = false;
    uint8_t  m_tfmOp = 0, m_tfmPost = 0;            // TFM form ($38-$3B) and its register postbyte
};
