#pragma once

#include "cpu6502.h"

/**
 * WDC 65C02 (Commander X16 and other CMOS 6502 machines).
 *
 * The MOS6502 core with the 65C02 opcode table (cpu6502_cmos.cpp): STZ,
 * TRB/TSB, BRA, PHX/PHY/PLX/PLY, INC/DEC A, BIT #/zp,X/abs,X, (zp) and
 * JMP (abs,X) modes, BBR/BBS/RMB/SMB, WAI and STP; undefined opcodes are
 * NOPs of their documented length. Differences from the NMOS part: ADC/SBC
 * set N, Z and V in decimal mode and take a cycle more; BRK and interrupts
 * clear D; JMP ($xxFF) reads its high byte from the next page.
 */
class MOS65C02 : public MOS6502 {
public:
    MOS65C02() { enableCmos(); reset(); }
    const char* isaName()     const override { return "65C02"; }
    const char* variantName() const override { return "WDC 65C02"; }
};
