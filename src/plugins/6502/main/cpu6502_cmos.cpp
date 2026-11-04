// WDC 65C02 support for the MOS6502 core: the opcode table and the CMOS
// instructions. Reference: x16-emulator src/cpu (65c02.h, instructions.h,
// tables.h) and the WDC W65C02S datasheet.
#include "cpu6502.h"

// ADC in decimal mode on the 65C02: N and Z from the result, V from the sum
// before the high-nibble adjustment, one extra cycle (x16-emulator adc()).
int MOS6502::adcDecimalCmos(uint8_t m) {
    uint16_t a = m_state.a;
    uint16_t lo = (a & 0x0F) + (m & 0x0F) + (m_state.p & FLAG_C);
    uint16_t hi = (a & 0xF0) + (m & 0xF0);
    if (lo > 0x09) { hi += 0x10; lo += 0x06; }
    uint16_t hiOv = hi;
    if (hi > 0x90) hi += 0x60;
    m_state.p &= ~(FLAG_C | FLAG_V | FLAG_Z | FLAG_N);
    if (hi & 0xFF00) m_state.p |= FLAG_C;
    uint8_t result = (uint8_t)((lo & 0x0F) | (hi & 0xF0));
    uint8_t ov = (uint8_t)((lo & 0x0F) | (hiOv & 0xF0));
    if ((ov ^ a) & (ov ^ m) & 0x80) m_state.p |= FLAG_V;
    m_state.a = result;
    updateNZ(result);
    return 1;
}

// SBC in decimal mode on the 65C02 (Bruce Clark, "Decimal Mode", appendix
// for the 65C02): adjust the binary difference by $60 on a borrow out and by
// $06 on a borrow out of the low nibble; C and V as in binary; N and Z from
// the result; one extra cycle.
int MOS6502::sbcDecimalCmos(uint8_t m) {
    int a = m_state.a;
    int c = (m_state.p & FLAG_C) ? 1 : 0;
    int lo = (a & 0x0F) - (m & 0x0F) + c - 1;
    int r = a - m + c - 1;
    uint16_t bin = (uint16_t)(a + (m ^ 0xFF) + c);
    m_state.p &= ~(FLAG_C | FLAG_V | FLAG_Z | FLAG_N);
    if (bin & 0x100) m_state.p |= FLAG_C;
    if ((bin ^ a) & (bin ^ (m ^ 0xFF)) & 0x80) m_state.p |= FLAG_V;
    if (r < 0) r -= 0x60;
    if (lo < 0) r -= 0x06;
    m_state.a = (uint8_t)r;
    updateNZ(m_state.a);
    return 1;
}

int MOS6502::opBRA(uint16_t addr) {
    uint16_t old = m_state.pc;
    m_state.pc = addr;
    return ((old & 0xFF00) != (addr & 0xFF00)) ? 1 : 0;
}

// BBR/BBS: addr is the zero-page byte, m_branchTarget the branch target
int MOS6502::opBBR(uint16_t addr) {
    uint8_t bit = (uint8_t)(1u << ((m_curOp >> 4) & 7));
    if (read(addr) & bit) return 0;
    uint16_t old = m_state.pc;
    m_state.pc = m_branchTarget;
    return ((old & 0xFF00) != (m_branchTarget & 0xFF00)) ? 2 : 1;
}
int MOS6502::opBBS(uint16_t addr) {
    uint8_t bit = (uint8_t)(1u << ((m_curOp >> 4) & 7));
    if (!(read(addr) & bit)) return 0;
    uint16_t old = m_state.pc;
    m_state.pc = m_branchTarget;
    return ((old & 0xFF00) != (m_branchTarget & 0xFF00)) ? 2 : 1;
}
int MOS6502::opRMB(uint16_t addr) {
    write(addr, (uint8_t)(read(addr) & ~(1u << ((m_curOp >> 4) & 7))));
    return 0;
}
int MOS6502::opSMB(uint16_t addr) {
    write(addr, (uint8_t)(read(addr) | (1u << ((m_curOp >> 4) & 7))));
    return 0;
}

int MOS6502::opPHX(uint16_t) { push(m_state.x); return 0; }
int MOS6502::opPHY(uint16_t) { push(m_state.y); return 0; }
int MOS6502::opPLX(uint16_t) { m_state.x = pop(); updateNZ(m_state.x); return 0; }
int MOS6502::opPLY(uint16_t) { m_state.y = pop(); updateNZ(m_state.y); return 0; }
int MOS6502::opSTZ(uint16_t addr) { write(addr, 0); return 0; }

int MOS6502::opTSB(uint16_t addr) {
    uint8_t m = read(addr);
    if (m & m_state.a) m_state.p &= ~FLAG_Z; else m_state.p |= FLAG_Z;
    write(addr, (uint8_t)(m | m_state.a));
    return 0;
}
int MOS6502::opTRB(uint16_t addr) {
    uint8_t m = read(addr);
    if (m & m_state.a) m_state.p &= ~FLAG_Z; else m_state.p |= FLAG_Z;
    write(addr, (uint8_t)(m & ~m_state.a));
    return 0;
}

int MOS6502::opINCA(uint16_t) { m_state.a++; updateNZ(m_state.a); return 0; }
int MOS6502::opDECA(uint16_t) { m_state.a--; updateNZ(m_state.a); return 0; }

// BIT #imm sets only Z
int MOS6502::opBITI(uint16_t addr) {
    if (m_state.a & read(addr)) m_state.p &= ~FLAG_Z; else m_state.p |= FLAG_Z;
    return 0;
}

int MOS6502::opWAI(uint16_t) { m_state.waiting = 1; return 0; }
int MOS6502::opSTP(uint16_t) { m_state.stopped = 1; return 0; }

// Generated from x16-emulator src/cpu/tables.h (addrtable_c02, optable_c02,
// ticktable_c02) by a script; $5C/$DC/$FC are 3-byte NOPs, $DB is STP, and timings
// corrected to recorded WDC 65C02 behaviour (SingleStepTests wdc65c02).
const MOS6502::Opcode MOS6502::s_opcodes65c02[256] = {
    {"BRK", &MOS6502::opBRK, Mode::IMP, 7, false}, // $00
    {"ORA", &MOS6502::opORA, Mode::BPXIND, 6, false}, // $01
    {"NOP", &MOS6502::opNOP, Mode::IMM, 2, false}, // $02
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $03
    {"TSB", &MOS6502::opTSB, Mode::BP, 5, false}, // $04
    {"ORA", &MOS6502::opORA, Mode::BP, 3, false}, // $05
    {"ASL", &MOS6502::opASL, Mode::BP, 5, false}, // $06
    {"RMB0", &MOS6502::opRMB, Mode::BP, 5, false}, // $07
    {"PHP", &MOS6502::opPHP, Mode::IMP, 3, false}, // $08
    {"ORA", &MOS6502::opORA, Mode::IMM, 2, false}, // $09
    {"ASL", &MOS6502::opASL, Mode::ACC, 2, false}, // $0A
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $0B
    {"TSB", &MOS6502::opTSB, Mode::ABS, 6, false}, // $0C
    {"ORA", &MOS6502::opORA, Mode::ABS, 4, false}, // $0D
    {"ASL", &MOS6502::opASL, Mode::ABS, 6, false}, // $0E
    {"BBR0", &MOS6502::opBBR, Mode::BPREL, 5, false}, // $0F
    {"BPL", &MOS6502::opBPL, Mode::REL, 2, false}, // $10
    {"ORA", &MOS6502::opORA, Mode::INDBPY, 5, true}, // $11
    {"ORA", &MOS6502::opORA, Mode::BPIND, 5, false}, // $12
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $13
    {"TRB", &MOS6502::opTRB, Mode::BP, 5, false}, // $14
    {"ORA", &MOS6502::opORA, Mode::BPX, 4, false}, // $15
    {"ASL", &MOS6502::opASL, Mode::BPX, 6, false}, // $16
    {"RMB1", &MOS6502::opRMB, Mode::BP, 5, false}, // $17
    {"CLC", &MOS6502::opCLC, Mode::IMP, 2, false}, // $18
    {"ORA", &MOS6502::opORA, Mode::ABSY, 4, true}, // $19
    {"INC", &MOS6502::opINCA, Mode::ACC, 2, false}, // $1A
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $1B
    {"TRB", &MOS6502::opTRB, Mode::ABS, 6, false}, // $1C
    {"ORA", &MOS6502::opORA, Mode::ABSX, 4, true}, // $1D
    {"ASL", &MOS6502::opASL, Mode::ABSX, 6, true}, // $1E
    {"BBR1", &MOS6502::opBBR, Mode::BPREL, 5, false}, // $1F
    {"JSR", &MOS6502::opJSR, Mode::ABS, 6, false}, // $20
    {"AND", &MOS6502::opAND, Mode::BPXIND, 6, false}, // $21
    {"NOP", &MOS6502::opNOP, Mode::IMM, 2, false}, // $22
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $23
    {"BIT", &MOS6502::opBIT, Mode::BP, 3, false}, // $24
    {"AND", &MOS6502::opAND, Mode::BP, 3, false}, // $25
    {"ROL", &MOS6502::opROL, Mode::BP, 5, false}, // $26
    {"RMB2", &MOS6502::opRMB, Mode::BP, 5, false}, // $27
    {"PLP", &MOS6502::opPLP, Mode::IMP, 4, false}, // $28
    {"AND", &MOS6502::opAND, Mode::IMM, 2, false}, // $29
    {"ROL", &MOS6502::opROL, Mode::ACC, 2, false}, // $2A
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $2B
    {"BIT", &MOS6502::opBIT, Mode::ABS, 4, false}, // $2C
    {"AND", &MOS6502::opAND, Mode::ABS, 4, false}, // $2D
    {"ROL", &MOS6502::opROL, Mode::ABS, 6, false}, // $2E
    {"BBR2", &MOS6502::opBBR, Mode::BPREL, 5, false}, // $2F
    {"BMI", &MOS6502::opBMI, Mode::REL, 2, false}, // $30
    {"AND", &MOS6502::opAND, Mode::INDBPY, 5, true}, // $31
    {"AND", &MOS6502::opAND, Mode::BPIND, 5, false}, // $32
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $33
    {"BIT", &MOS6502::opBIT, Mode::BPX, 4, false}, // $34
    {"AND", &MOS6502::opAND, Mode::BPX, 4, false}, // $35
    {"ROL", &MOS6502::opROL, Mode::BPX, 6, false}, // $36
    {"RMB3", &MOS6502::opRMB, Mode::BP, 5, false}, // $37
    {"SEC", &MOS6502::opSEC, Mode::IMP, 2, false}, // $38
    {"AND", &MOS6502::opAND, Mode::ABSY, 4, true}, // $39
    {"DEC", &MOS6502::opDECA, Mode::ACC, 2, false}, // $3A
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $3B
    {"BIT", &MOS6502::opBIT, Mode::ABSX, 4, true}, // $3C
    {"AND", &MOS6502::opAND, Mode::ABSX, 4, true}, // $3D
    {"ROL", &MOS6502::opROL, Mode::ABSX, 6, true}, // $3E
    {"BBR3", &MOS6502::opBBR, Mode::BPREL, 5, false}, // $3F
    {"RTI", &MOS6502::opRTI, Mode::IMP, 6, false}, // $40
    {"EOR", &MOS6502::opEOR, Mode::BPXIND, 6, false}, // $41
    {"NOP", &MOS6502::opNOP, Mode::IMM, 2, false}, // $42
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $43
    {"NOP", &MOS6502::opNOP, Mode::IMM, 3, false}, // $44
    {"EOR", &MOS6502::opEOR, Mode::BP, 3, false}, // $45
    {"LSR", &MOS6502::opLSR, Mode::BP, 5, false}, // $46
    {"RMB4", &MOS6502::opRMB, Mode::BP, 5, false}, // $47
    {"PHA", &MOS6502::opPHA, Mode::IMP, 3, false}, // $48
    {"EOR", &MOS6502::opEOR, Mode::IMM, 2, false}, // $49
    {"LSR", &MOS6502::opLSR, Mode::ACC, 2, false}, // $4A
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $4B
    {"JMP", &MOS6502::opJMP, Mode::ABS, 3, false}, // $4C
    {"EOR", &MOS6502::opEOR, Mode::ABS, 4, false}, // $4D
    {"LSR", &MOS6502::opLSR, Mode::ABS, 6, false}, // $4E
    {"BBR4", &MOS6502::opBBR, Mode::BPREL, 5, false}, // $4F
    {"BVC", &MOS6502::opBVC, Mode::REL, 2, false}, // $50
    {"EOR", &MOS6502::opEOR, Mode::INDBPY, 5, true}, // $51
    {"EOR", &MOS6502::opEOR, Mode::BPIND, 5, false}, // $52
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $53
    {"NOP", &MOS6502::opNOP, Mode::IMM, 4, false}, // $54
    {"EOR", &MOS6502::opEOR, Mode::BPX, 4, false}, // $55
    {"LSR", &MOS6502::opLSR, Mode::BPX, 6, false}, // $56
    {"RMB5", &MOS6502::opRMB, Mode::BP, 5, false}, // $57
    {"CLI", &MOS6502::opCLI, Mode::IMP, 2, false}, // $58
    {"EOR", &MOS6502::opEOR, Mode::ABSY, 4, true}, // $59
    {"PHY", &MOS6502::opPHY, Mode::IMP, 3, false}, // $5A
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $5B
    {"NOP", &MOS6502::opNOP, Mode::ABS, 4, false}, // $5C
    {"EOR", &MOS6502::opEOR, Mode::ABSX, 4, true}, // $5D
    {"LSR", &MOS6502::opLSR, Mode::ABSX, 6, true}, // $5E
    {"BBR5", &MOS6502::opBBR, Mode::BPREL, 5, false}, // $5F
    {"RTS", &MOS6502::opRTS, Mode::IMP, 6, false}, // $60
    {"ADC", &MOS6502::opADC, Mode::BPXIND, 6, false}, // $61
    {"NOP", &MOS6502::opNOP, Mode::IMM, 2, false}, // $62
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $63
    {"STZ", &MOS6502::opSTZ, Mode::BP, 3, false}, // $64
    {"ADC", &MOS6502::opADC, Mode::BP, 3, false}, // $65
    {"ROR", &MOS6502::opROR, Mode::BP, 5, false}, // $66
    {"RMB6", &MOS6502::opRMB, Mode::BP, 5, false}, // $67
    {"PLA", &MOS6502::opPLA, Mode::IMP, 4, false}, // $68
    {"ADC", &MOS6502::opADC, Mode::IMM, 2, false}, // $69
    {"ROR", &MOS6502::opROR, Mode::ACC, 2, false}, // $6A
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $6B
    {"JMP", &MOS6502::opJMP, Mode::IND, 6, false}, // $6C
    {"ADC", &MOS6502::opADC, Mode::ABS, 4, false}, // $6D
    {"ROR", &MOS6502::opROR, Mode::ABS, 6, false}, // $6E
    {"BBR6", &MOS6502::opBBR, Mode::BPREL, 5, false}, // $6F
    {"BVS", &MOS6502::opBVS, Mode::REL, 2, false}, // $70
    {"ADC", &MOS6502::opADC, Mode::INDBPY, 5, true}, // $71
    {"ADC", &MOS6502::opADC, Mode::BPIND, 5, false}, // $72
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $73
    {"STZ", &MOS6502::opSTZ, Mode::BPX, 4, false}, // $74
    {"ADC", &MOS6502::opADC, Mode::BPX, 4, false}, // $75
    {"ROR", &MOS6502::opROR, Mode::BPX, 6, false}, // $76
    {"RMB7", &MOS6502::opRMB, Mode::BP, 5, false}, // $77
    {"SEI", &MOS6502::opSEI, Mode::IMP, 2, false}, // $78
    {"ADC", &MOS6502::opADC, Mode::ABSY, 4, true}, // $79
    {"PLY", &MOS6502::opPLY, Mode::IMP, 4, false}, // $7A
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $7B
    {"JMP", &MOS6502::opJMP, Mode::ABSXIND, 6, false}, // $7C
    {"ADC", &MOS6502::opADC, Mode::ABSX, 4, true}, // $7D
    {"ROR", &MOS6502::opROR, Mode::ABSX, 6, true}, // $7E
    {"BBR7", &MOS6502::opBBR, Mode::BPREL, 5, false}, // $7F
    {"BRA", &MOS6502::opBRA, Mode::REL, 3, false}, // $80
    {"STA", &MOS6502::opSTA, Mode::BPXIND, 6, false}, // $81
    {"NOP", &MOS6502::opNOP, Mode::IMM, 2, false}, // $82
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $83
    {"STY", &MOS6502::opSTY, Mode::BP, 3, false}, // $84
    {"STA", &MOS6502::opSTA, Mode::BP, 3, false}, // $85
    {"STX", &MOS6502::opSTX, Mode::BP, 3, false}, // $86
    {"SMB0", &MOS6502::opSMB, Mode::BP, 5, false}, // $87
    {"DEY", &MOS6502::opDEY, Mode::IMP, 2, false}, // $88
    {"BIT", &MOS6502::opBITI, Mode::IMM, 2, false}, // $89
    {"TXA", &MOS6502::opTXA, Mode::IMP, 2, false}, // $8A
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $8B
    {"STY", &MOS6502::opSTY, Mode::ABS, 4, false}, // $8C
    {"STA", &MOS6502::opSTA, Mode::ABS, 4, false}, // $8D
    {"STX", &MOS6502::opSTX, Mode::ABS, 4, false}, // $8E
    {"BBS0", &MOS6502::opBBS, Mode::BPREL, 5, false}, // $8F
    {"BCC", &MOS6502::opBCC, Mode::REL, 2, false}, // $90
    {"STA", &MOS6502::opSTA, Mode::INDBPY, 6, false}, // $91
    {"STA", &MOS6502::opSTA, Mode::BPIND, 5, false}, // $92
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $93
    {"STY", &MOS6502::opSTY, Mode::BPX, 4, false}, // $94
    {"STA", &MOS6502::opSTA, Mode::BPX, 4, false}, // $95
    {"STX", &MOS6502::opSTX, Mode::BPY, 4, false}, // $96
    {"SMB1", &MOS6502::opSMB, Mode::BP, 5, false}, // $97
    {"TYA", &MOS6502::opTYA, Mode::IMP, 2, false}, // $98
    {"STA", &MOS6502::opSTA, Mode::ABSY, 5, false}, // $99
    {"TXS", &MOS6502::opTXS, Mode::IMP, 2, false}, // $9A
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $9B
    {"STZ", &MOS6502::opSTZ, Mode::ABS, 4, false}, // $9C
    {"STA", &MOS6502::opSTA, Mode::ABSX, 5, false}, // $9D
    {"STZ", &MOS6502::opSTZ, Mode::ABSX, 5, false}, // $9E
    {"BBS1", &MOS6502::opBBS, Mode::BPREL, 5, false}, // $9F
    {"LDY", &MOS6502::opLDY, Mode::IMM, 2, false}, // $A0
    {"LDA", &MOS6502::opLDA, Mode::BPXIND, 6, false}, // $A1
    {"LDX", &MOS6502::opLDX, Mode::IMM, 2, false}, // $A2
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $A3
    {"LDY", &MOS6502::opLDY, Mode::BP, 3, false}, // $A4
    {"LDA", &MOS6502::opLDA, Mode::BP, 3, false}, // $A5
    {"LDX", &MOS6502::opLDX, Mode::BP, 3, false}, // $A6
    {"SMB2", &MOS6502::opSMB, Mode::BP, 5, false}, // $A7
    {"TAY", &MOS6502::opTAY, Mode::IMP, 2, false}, // $A8
    {"LDA", &MOS6502::opLDA, Mode::IMM, 2, false}, // $A9
    {"TAX", &MOS6502::opTAX, Mode::IMP, 2, false}, // $AA
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $AB
    {"LDY", &MOS6502::opLDY, Mode::ABS, 4, false}, // $AC
    {"LDA", &MOS6502::opLDA, Mode::ABS, 4, false}, // $AD
    {"LDX", &MOS6502::opLDX, Mode::ABS, 4, false}, // $AE
    {"BBS2", &MOS6502::opBBS, Mode::BPREL, 5, false}, // $AF
    {"BCS", &MOS6502::opBCS, Mode::REL, 2, false}, // $B0
    {"LDA", &MOS6502::opLDA, Mode::INDBPY, 5, true}, // $B1
    {"LDA", &MOS6502::opLDA, Mode::BPIND, 5, false}, // $B2
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $B3
    {"LDY", &MOS6502::opLDY, Mode::BPX, 4, false}, // $B4
    {"LDA", &MOS6502::opLDA, Mode::BPX, 4, false}, // $B5
    {"LDX", &MOS6502::opLDX, Mode::BPY, 4, false}, // $B6
    {"SMB3", &MOS6502::opSMB, Mode::BP, 5, false}, // $B7
    {"CLV", &MOS6502::opCLV, Mode::IMP, 2, false}, // $B8
    {"LDA", &MOS6502::opLDA, Mode::ABSY, 4, true}, // $B9
    {"TSX", &MOS6502::opTSX, Mode::IMP, 2, false}, // $BA
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $BB
    {"LDY", &MOS6502::opLDY, Mode::ABSX, 4, true}, // $BC
    {"LDA", &MOS6502::opLDA, Mode::ABSX, 4, true}, // $BD
    {"LDX", &MOS6502::opLDX, Mode::ABSY, 4, true}, // $BE
    {"BBS3", &MOS6502::opBBS, Mode::BPREL, 5, false}, // $BF
    {"CPY", &MOS6502::opCPY, Mode::IMM, 2, false}, // $C0
    {"CMP", &MOS6502::opCMP, Mode::BPXIND, 6, false}, // $C1
    {"NOP", &MOS6502::opNOP, Mode::IMM, 2, false}, // $C2
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $C3
    {"CPY", &MOS6502::opCPY, Mode::BP, 3, false}, // $C4
    {"CMP", &MOS6502::opCMP, Mode::BP, 3, false}, // $C5
    {"DEC", &MOS6502::opDEC, Mode::BP, 5, false}, // $C6
    {"SMB4", &MOS6502::opSMB, Mode::BP, 5, false}, // $C7
    {"INY", &MOS6502::opINY, Mode::IMP, 2, false}, // $C8
    {"CMP", &MOS6502::opCMP, Mode::IMM, 2, false}, // $C9
    {"DEX", &MOS6502::opDEX, Mode::IMP, 2, false}, // $CA
    {"WAI", &MOS6502::opWAI, Mode::IMP, 3, false}, // $CB
    {"CPY", &MOS6502::opCPY, Mode::ABS, 4, false}, // $CC
    {"CMP", &MOS6502::opCMP, Mode::ABS, 4, false}, // $CD
    {"DEC", &MOS6502::opDEC, Mode::ABS, 6, false}, // $CE
    {"BBS4", &MOS6502::opBBS, Mode::BPREL, 5, false}, // $CF
    {"BNE", &MOS6502::opBNE, Mode::REL, 2, false}, // $D0
    {"CMP", &MOS6502::opCMP, Mode::INDBPY, 5, true}, // $D1
    {"CMP", &MOS6502::opCMP, Mode::BPIND, 5, false}, // $D2
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $D3
    {"NOP", &MOS6502::opNOP, Mode::IMM, 4, false}, // $D4
    {"CMP", &MOS6502::opCMP, Mode::BPX, 4, false}, // $D5
    {"DEC", &MOS6502::opDEC, Mode::BPX, 6, false}, // $D6
    {"SMB5", &MOS6502::opSMB, Mode::BP, 5, false}, // $D7
    {"CLD", &MOS6502::opCLD, Mode::IMP, 2, false}, // $D8
    {"CMP", &MOS6502::opCMP, Mode::ABSY, 4, true}, // $D9
    {"PHX", &MOS6502::opPHX, Mode::IMP, 3, false}, // $DA
    {"STP", &MOS6502::opSTP, Mode::IMP, 3, false}, // $DB
    {"NOP", &MOS6502::opNOP, Mode::ABS, 4, false}, // $DC
    {"CMP", &MOS6502::opCMP, Mode::ABSX, 4, true}, // $DD
    {"DEC", &MOS6502::opDEC, Mode::ABSX, 7, false}, // $DE
    {"BBS5", &MOS6502::opBBS, Mode::BPREL, 5, false}, // $DF
    {"CPX", &MOS6502::opCPX, Mode::IMM, 2, false}, // $E0
    {"SBC", &MOS6502::opSBC, Mode::BPXIND, 6, false}, // $E1
    {"NOP", &MOS6502::opNOP, Mode::IMM, 2, false}, // $E2
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $E3
    {"CPX", &MOS6502::opCPX, Mode::BP, 3, false}, // $E4
    {"SBC", &MOS6502::opSBC, Mode::BP, 3, false}, // $E5
    {"INC", &MOS6502::opINC, Mode::BP, 5, false}, // $E6
    {"SMB6", &MOS6502::opSMB, Mode::BP, 5, false}, // $E7
    {"INX", &MOS6502::opINX, Mode::IMP, 2, false}, // $E8
    {"SBC", &MOS6502::opSBC, Mode::IMM, 2, false}, // $E9
    {"NOP", &MOS6502::opNOP, Mode::IMP, 2, false}, // $EA
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $EB
    {"CPX", &MOS6502::opCPX, Mode::ABS, 4, false}, // $EC
    {"SBC", &MOS6502::opSBC, Mode::ABS, 4, false}, // $ED
    {"INC", &MOS6502::opINC, Mode::ABS, 6, false}, // $EE
    {"BBS6", &MOS6502::opBBS, Mode::BPREL, 5, false}, // $EF
    {"BEQ", &MOS6502::opBEQ, Mode::REL, 2, false}, // $F0
    {"SBC", &MOS6502::opSBC, Mode::INDBPY, 5, true}, // $F1
    {"SBC", &MOS6502::opSBC, Mode::BPIND, 5, false}, // $F2
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $F3
    {"NOP", &MOS6502::opNOP, Mode::IMM, 4, false}, // $F4
    {"SBC", &MOS6502::opSBC, Mode::BPX, 4, false}, // $F5
    {"INC", &MOS6502::opINC, Mode::BPX, 6, false}, // $F6
    {"SMB7", &MOS6502::opSMB, Mode::BP, 5, false}, // $F7
    {"SED", &MOS6502::opSED, Mode::IMP, 2, false}, // $F8
    {"SBC", &MOS6502::opSBC, Mode::ABSY, 4, true}, // $F9
    {"PLX", &MOS6502::opPLX, Mode::IMP, 4, false}, // $FA
    {"NOP", &MOS6502::opNOP, Mode::IMP, 1, false}, // $FB
    {"NOP", &MOS6502::opNOP, Mode::ABS, 4, false}, // $FC
    {"SBC", &MOS6502::opSBC, Mode::ABSX, 4, true}, // $FD
    {"INC", &MOS6502::opINC, Mode::ABSX, 7, false}, // $FE
    {"BBS7", &MOS6502::opBBS, Mode::BPREL, 5, false}, // $FF
};
