// The ST018 after Mesen2 (Core/SNES/Coprocessors/ST018/St018.cpp,
// ArmV3Cpu.cpp), GPL-3.0, Sour and the Mesen contributors. The multiplier
// is zaydlang's (snes_arm_multiply.h).

#include "snes_st018.h"
#include "snes_arm_multiply.h"

#include <cstring>

namespace snes {

namespace {

// The ALU operations of a data-processing instruction
enum Alu : uint8_t { AND, EOR, SUB, RSB, ADD, ADC, SBC, RSC, TST, TEQ, CMP, CMN, ORR, MOV, BIC, MVN };

using Handler = void (St018::*)();

}  // namespace

// The decode table: bits 27-20 and 7-4 of the opcode, built as Mesen's InitArmOpTable (later entries win)
struct St018Table {
    Handler fn[0x1000];
    St018Table(Handler invalid, Handler dp, Handler msr, Handler mrs, Handler branch, Handler single, Handler block,
               Handler mul, Handler mull, Handler swp, Handler swi) {
        for (auto& f : fn) f = invalid;
        for (int i = 0; i <= 0x3FF; ++i) {
            const int op = (i >> 5) & 0x0F;
            const bool s = i & 0x10;
            if (!s && op >= TST && op <= CMN) fn[i] = (i & 0x20) ? msr : mrs;
            else fn[i] = dp;
        }
        for (int i = 0; i <= 0x1FF; ++i) fn[0xA00 | i] = branch;
        for (int i = 0; i <= 0x3FF; ++i) fn[0x400 | i] = single;
        for (int i = 0; i <= 0x1FF; ++i) fn[0x800 | i] = block;
        for (int i = 0; i <= 0x07; ++i) fn[0x009 | (i << 4)] = mul;
        for (int i = 0; i <= 0x07; ++i) fn[0x089 | (i << 4)] = mull;
        for (int i = 0; i <= 0x07; ++i) {
            fn[0x109 | (i & 0x04) << 5 | (i & 0x03) << 4] = swp;
            fn[0x149 | (i & 0x04) << 5 | (i & 0x03) << 4] = swp;
        }
        for (int i = 0; i <= 0xFF; ++i) fn[0xF00 + i] = swi;
    }
};

St018::St018(const std::vector<uint8_t>& fw) {
    m_prg.assign(PRG_SIZE, 0);
    m_data.assign(DATA_SIZE, 0);
    if (fw.size() == PRG_SIZE + DATA_SIZE) {
        std::memcpy(m_prg.data(), fw.data(), PRG_SIZE);
        std::memcpy(m_data.data(), fw.data() + PRG_SIZE, DATA_SIZE);
    }
    powerOn();
}

void St018::powerOn() {
    m_hasDataForSnes = m_hasDataForArm = m_armReset = m_ack = false;
    m_dataSnes = m_dataArm = 0;
    m_cycles = 0;                                   // the console's clock starts again too
    armPowerOn(false);
}

void St018::armPowerOn(bool forReset) {
    // Mesen's ArmV3Cpu::PowerOn: everything cleared, supervisor mode with interrupts off, the pipeline
    // filled; on a reset the fill's cycles aren't counted
    std::memset(m_r, 0, sizeof m_r);
    m_cpsr = Psr{};
    m_fiqSpsr = m_irqSpsr = m_svcSpsr = m_abtSpsr = m_undSpsr = Psr{};
    std::memset(m_userRegs, 0, sizeof m_userRegs); std::memset(m_fiqRegs, 0, sizeof m_fiqRegs);
    std::memset(m_irqRegs, 0, sizeof m_irqRegs); std::memset(m_svcRegs, 0, sizeof m_svcRegs);
    std::memset(m_abtRegs, 0, sizeof m_abtRegs); std::memset(m_undRegs, 0, sizeof m_undRegs);
    m_fetch = m_decode = m_execute = Slot{};
    m_pipeMode = 0;
    m_op = 0;
    m_reload = true;
    m_cpsr.mode = SVC;
    m_cpsr.i = m_cpsr.f = true;
    const uint64_t keep = m_cycles;
    processPipeline();
    if (forReset) m_cycles = keep;
}

void St018::runTo(uint64_t master) {
    if (m_armReset) { m_cycles = master; return; }
    while (m_cycles < master) exec();
}

// The SNES side

uint8_t St018::status() const {
    return (uint8_t)((m_hasDataForSnes ? 0x01 : 0) | (m_ack ? 0x04 : 0) | (m_hasDataForArm ? 0x08 : 0) | (!m_armReset ? 0x80 : 0));
}

int St018::read(uint16_t a, bool peek) {
    switch (a & 0xFF06) {
    case 0x3800:
        if (!peek) m_hasDataForSnes = false;
        return m_dataSnes;
    case 0x3802:
        if (!peek) m_ack = false;
        return -1;
    case 0x3804: return status();
    default: return -1;
    }
}

void St018::write(uint16_t a, uint8_t v) {
    switch (a & 0xFF06) {
    case 0x3802:
        m_dataArm = v;
        m_hasDataForArm = true;
        break;
    case 0x3804: {
        // Releasing reset restarts the ARM; the link keeps its state
        if (m_armReset && !v) armPowerOn(true);
        m_armReset = v != 0;
        break;
    }
    default: break;
    }
}

// The ARM's bus: one master clock an access

uint8_t St018::readByte(uint32_t addr) {
    switch (addr >> 28) {
    case 0x0: return m_prg[addr & (PRG_SIZE - 1)];
    case 0x4:
        switch (addr & 0x3F) {
        case 0x10: m_hasDataForArm = false; return m_dataArm;
        case 0x20: return status();
        default: return 0;
        }
    case 0xA: return m_data[addr & (DATA_SIZE - 1)];
    case 0xE: return m_ram[addr & (RAM_SIZE - 1)];
    default: return 0;
    }
}

void St018::writeByte(uint32_t addr, uint8_t v) {
    switch (addr >> 28) {
    case 0x4:
        switch (addr & 0x3F) {
        case 0x00: m_hasDataForSnes = true; m_dataSnes = v; break;
        case 0x10: m_ack = true; break;
        default: break;
        }
        break;
    case 0xE: m_ram[addr & (RAM_SIZE - 1)] = v; break;
    default: break;
    }
}

uint32_t St018::busRead(uint8_t mode, uint32_t addr) {
    ++m_cycles;
    if (mode & WORD) {
        // Aligned, without the rotation of a misaligned load (as Mesen)
        const uint32_t base = addr & ~3u;
        const uint32_t b0 = readByte(base), b1 = readByte(base | 1), b2 = readByte(base | 2), b3 = readByte(base | 3);
        return b0 | b1 << 8 | b2 << 16 | b3 << 24;
    }
    return readByte(addr);
}

void St018::busWrite(uint8_t mode, uint32_t addr, uint32_t v) {
    ++m_cycles;
    if (mode & WORD) {
        const uint32_t base = addr & ~3u;
        writeByte(base, (uint8_t)v); writeByte(base | 1, (uint8_t)(v >> 8));
        writeByte(base | 2, (uint8_t)(v >> 16)); writeByte(base | 3, (uint8_t)(v >> 24));
    } else {
        writeByte(addr, (uint8_t)v);
    }
}

// The pipeline: R15 is two instructions ahead of the one executing

void St018::reloadPipeline() {
    m_pipeMode = PREFETCH | WORD;
    m_reload = false;
    m_fetch.addr = m_r[15] = m_r[15] & ~3u;
    m_fetch.op = readCode(m_pipeMode, m_fetch.addr);
    m_execute = m_decode;
    m_decode = m_fetch;
    m_fetch.addr = m_r[15] = m_r[15] + 4;
    m_fetch.op = readCode(m_pipeMode, m_fetch.addr);
}

void St018::processPipeline() {
    if (m_reload) reloadPipeline();
    m_execute = m_decode;
    m_decode = m_fetch;
    m_fetch.addr = m_r[15] = m_r[15] + 4;
    m_fetch.op = readCode(m_pipeMode, m_fetch.addr);
}

void St018::exec() {
    static const St018Table table(&St018::opInvalid, &St018::opDataProcessing, &St018::opMsr, &St018::opMrs,
                                  &St018::opBranch, &St018::opSingleTransfer, &St018::opBlockTransfer,
                                  &St018::opMultiply, &St018::opMultiplyLong, &St018::opSwap, &St018::opSwi);
    m_op = m_execute.op;
    if (condition(m_op >> 28)) (this->*table.fn[((m_op & 0x0FF00000) >> 16) | ((m_op & 0xF0) >> 4)])();
    processPipeline();
}

bool St018::condition(uint32_t cc) const {
    const Psr& p = m_cpsr;
    switch (cc) {
    case 0: return p.z;
    case 1: return !p.z;
    case 2: return p.c;
    case 3: return !p.c;
    case 4: return p.n;
    case 5: return !p.n;
    case 6: return p.v;
    case 7: return !p.v;
    case 8: return p.c && !p.z;
    case 9: return !p.c || p.z;
    case 10: return p.n == p.v;
    case 11: return p.n != p.v;
    case 12: return !p.z && p.n == p.v;
    case 13: return p.z || p.n != p.v;
    case 14: return true;
    default: return false;
    }
}

// Modes and banked registers

void St018::switchMode(uint8_t mode) {
    mode |= 0x10;                                   // the high bit of the mode is always set
    if (m_cpsr.mode == mode) return;
    switch (m_cpsr.mode) {
    case FIQ: std::memcpy(m_fiqRegs, &m_r[8], 7 * 4); break;
    case IRQ: std::memcpy(m_userRegs, &m_r[8], 5 * 4); std::memcpy(m_irqRegs, &m_r[13], 2 * 4); break;
    case SVC: std::memcpy(m_userRegs, &m_r[8], 5 * 4); std::memcpy(m_svcRegs, &m_r[13], 2 * 4); break;
    case ABT: std::memcpy(m_userRegs, &m_r[8], 5 * 4); std::memcpy(m_abtRegs, &m_r[13], 2 * 4); break;
    case UND: std::memcpy(m_userRegs, &m_r[8], 5 * 4); std::memcpy(m_undRegs, &m_r[13], 2 * 4); break;
    default: std::memcpy(m_userRegs, &m_r[8], 7 * 4); break;   // user, system and the rest
    }
    m_cpsr.mode = mode;
    if (mode != FIQ) {
        std::memcpy(&m_r[8], m_userRegs, 7 * 4);
        switch (mode) {
        case IRQ: std::memcpy(&m_r[13], m_irqRegs, 2 * 4); break;
        case SVC: std::memcpy(&m_r[13], m_svcRegs, 2 * 4); break;
        case ABT: std::memcpy(&m_r[13], m_abtRegs, 2 * 4); break;
        case UND: std::memcpy(&m_r[13], m_undRegs, 2 * 4); break;
        default: break;
        }
    } else {
        std::memcpy(&m_r[8], m_fiqRegs, 7 * 4);
    }
}

St018::Psr& St018::spsr() {
    switch (m_cpsr.mode) {
    case FIQ: return m_fiqSpsr;
    case IRQ: return m_irqSpsr;
    case SVC: return m_svcSpsr;
    case ABT: return m_abtSpsr;
    case UND: return m_undSpsr;
    default: return m_cpsr;                         // user and system have none
    }
}

void St018::exception(uint8_t mode, uint32_t vector) {
    const Psr old = m_cpsr;
    switchMode(mode);
    spsr() = old;
    m_cpsr.i = true;
    m_r[14] = m_decode.addr;
    m_r[15] = vector;
    m_reload = true;
}

// The ALU and the barrel shifter

uint32_t St018::add(uint32_t a, uint32_t b, bool carry, bool flags) {
    const uint32_t r = a + b + (uint32_t)carry;
    if (flags) {
        const uint32_t ov = ~(a ^ b) & (a ^ r) & 0x80000000u;
        m_cpsr.n = r & 0x80000000u;
        m_cpsr.z = r == 0;
        m_cpsr.v = ov;
        m_cpsr.c = (a ^ b ^ ov ^ r) & 0x80000000u;
    }
    return r;
}

uint32_t St018::logical(uint32_t r, bool carry, bool flags) {
    if (flags) {
        m_cpsr.c = carry;
        m_cpsr.z = r == 0;
        m_cpsr.n = r & 0x80000000u;
    }
    return r;
}

uint32_t St018::shift(uint8_t type, uint32_t v, uint8_t s, bool& carry) {
    switch (type) {
    case 0:                                         // LSL
        if (s) { carry = s < 33 ? (v >> (32 - s)) & 1 : false; v = s < 32 ? v << s : 0; }
        return v;
    case 1:                                         // LSR
        if (s) { carry = s < 33 ? (v >> (s - 1)) & 1 : false; v = s < 32 ? v >> s : 0; }
        return v;
    case 2:                                         // ASR
        if (s) { carry = s < 33 ? (v >> (s - 1)) & 1 : (v >> 31); v = s < 32 ? (uint32_t)((int32_t)v >> s) : (uint32_t)((int32_t)v >> 31); }
        return v;
    default:                                        // ROR (RRX is done by the callers)
        if (s) {
            s &= 0x1F;
            if (s) v = (v << (32 - s)) | (v >> s);
            carry = v >> 31;
        }
        return v;
    }
}

// The instructions

void St018::opInvalid() { exception(UND, 0x04); }
void St018::opSwi() { exception(SVC, 0x08); }

void St018::opBranch() {
    const int32_t offset = ((int32_t)(m_op << 8)) >> 6;    // sign-extended, times 4
    if (m_op & (1u << 24)) m_r[14] = m_r[15] - 4;
    m_r[15] += (uint32_t)offset;
    m_reload = true;
}

void St018::opMsr() {
    const bool imm = m_op & (1u << 25);
    const bool toSpsr = m_op & (1u << 22);
    const uint8_t mask = (m_op >> 16) & 0x0F;
    if (toSpsr && (m_cpsr.mode == USR || m_cpsr.mode == SYS)) return;
    uint32_t v;
    if (imm) {
        v = m_op & 0xFF;
        const uint8_t rot = (m_op >> 8) & 0x0F;
        if (rot) v = (v >> (rot * 2)) | (v << (32 - rot * 2));
    } else {
        v = m_r[m_op & 0x0F];
    }
    Psr& p = toSpsr ? spsr() : m_cpsr;
    if (mask & 0x08) {
        p.n = v & (1u << 31); p.z = v & (1u << 30); p.c = v & (1u << 29); p.v = v & (1u << 28);
    }
    if ((mask & 0x01) && (toSpsr || m_cpsr.mode != USR)) {
        if (!toSpsr) switchMode((uint8_t)(v & 0x1F));
        else p.mode = (uint8_t)(v & 0x1F);
        p.f = v & (1u << 6);
        p.i = v & (1u << 7);
    }
}

void St018::opMrs() {
    const bool fromSpsr = m_op & (1u << 22);
    setR((m_op >> 12) & 0x0F, fromSpsr ? spsr().value() : m_cpsr.value());
}

void St018::opDataProcessing() {
    const bool imm = m_op & (1u << 25);
    const uint8_t rn = (m_op >> 16) & 0x0F, rd = (m_op >> 12) & 0x0F;
    const bool flags = m_op & (1u << 20);
    uint32_t op1 = m_r[rn], op2;
    bool carry = m_cpsr.c;
    if (imm) {
        const uint8_t rot = (m_op >> 8) & 0x0F;
        op2 = m_op & 0xFF;
        if (rot) {
            const uint32_t s = rot * 2u;
            carry = (op2 >> (s - 1)) & 1;
            op2 = (op2 >> s) | (op2 << (32 - s));
        }
    } else {
        const uint8_t type = (m_op >> 5) & 0x03, rm = m_op & 0x0F;
        op2 = m_r[rm];
        const bool byReg = m_op & (1u << 4);
        uint8_t s;
        if (byReg) {
            // A shift by register costs a cycle, and R15 reads 4 further on
            idle();
            const uint8_t rs = (m_op >> 8) & 0x0F;
            s = (uint8_t)(m_r[rs] + (rs == 15 ? 4 : 0));
            if (rm == 15) op2 += 4;
            if (rn == 15) op1 += 4;
        } else {
            s = (m_op >> 7) & 0x1F;
        }
        switch (type) {
        case 0: op2 = shift(0, op2, s, carry); break;
        case 1: op2 = shift(1, op2, (byReg || s) ? s : 32, carry); break;
        case 2: op2 = shift(2, op2, (byReg || s) ? s : 32, carry); break;
        default:
            if (!byReg && s == 0) {                 // RRX
                const bool c = carry;
                carry = op2 & 1;
                op2 = (op2 >> 1) | ((uint32_t)c << 31);
            } else {
                op2 = shift(3, op2, s, carry);
            }
            break;
        }
    }
    switch ((m_op >> 21) & 0x0F) {
    case AND: setR(rd, logical(op1 & op2, carry, flags)); break;
    case EOR: setR(rd, logical(op1 ^ op2, carry, flags)); break;
    case SUB: setR(rd, sub(op1, op2, true, flags)); break;
    case RSB: setR(rd, sub(op2, op1, true, flags)); break;
    case ADD: setR(rd, add(op1, op2, false, flags)); break;
    case ADC: setR(rd, add(op1, op2, m_cpsr.c, flags)); break;
    case SBC: setR(rd, sub(op1, op2, m_cpsr.c, flags)); break;
    case RSC: setR(rd, sub(op2, op1, m_cpsr.c, flags)); break;
    case TST: logical(op1 & op2, carry, true); break;
    case TEQ: logical(op1 ^ op2, carry, true); break;
    case CMP: sub(op1, op2, true, true); break;
    case CMN: add(op1, op2, false, true); break;
    case ORR: setR(rd, logical(op1 | op2, carry, flags)); break;
    case MOV: setR(rd, logical(op2, carry, flags)); break;
    case BIC: setR(rd, logical(op1 & ~op2, carry, flags)); break;
    default: setR(rd, logical(~op2, carry, flags)); break;     // MVN
    }
    if (rd == 15 && flags) {
        // To R15 with S: the mode's SPSR comes back
        const Psr p = spsr();
        switchMode(p.mode);
        m_cpsr = p;
    }
}

void St018::opMultiply() {
    const uint8_t rd = (m_op >> 16) & 0x0F, rn = (m_op >> 12) & 0x0F, rs = (m_op >> 8) & 0x0F, rm = m_op & 0x0F;
    const bool flags = m_op & (1u << 20), acc = m_op & (1u << 21);
    const MultiplicationOutput out = acc ? ArmMultiply::mla(m_r[rm], m_r[rs], m_r[rn]) : ArmMultiply::mul(m_r[rm], m_r[rs]);
    idle(out.CycleCount);
    if (acc) idle();
    const uint32_t r = (uint32_t)out.Output;
    if (rd != 15) setR(rd, r);
    if (flags) { m_cpsr.c = out.Carry; m_cpsr.z = r == 0; m_cpsr.n = r & 0x80000000u; }
}

void St018::opMultiplyLong() {
    const uint8_t rh = (m_op >> 16) & 0x0F, rl = (m_op >> 12) & 0x0F, rs = (m_op >> 8) & 0x0F, rm = m_op & 0x0F;
    const bool flags = m_op & (1u << 20), acc = m_op & (1u << 21), sign = m_op & (1u << 22);
    idle();
    MultiplicationOutput out;
    if (sign) out = acc ? ArmMultiply::smlal(m_r[rl], m_r[rh], m_r[rm], m_r[rs]) : ArmMultiply::smull(m_r[rm], m_r[rs]);
    else out = acc ? ArmMultiply::umlal(m_r[rl], m_r[rh], m_r[rm], m_r[rs]) : ArmMultiply::umull(m_r[rm], m_r[rs]);
    idle(out.CycleCount);
    if (acc) idle();
    const uint64_t r = out.Output;
    if (rl != 15) setR(rl, (uint32_t)r);
    if (rh != 15) setR(rh, (uint32_t)(r >> 32));
    if (flags) { m_cpsr.c = out.Carry; m_cpsr.z = r == 0; m_cpsr.n = r >> 63; }
}

void St018::opSingleTransfer() {
    const bool imm = !(m_op & (1u << 25)), pre = m_op & (1u << 24), up = m_op & (1u << 23);
    const bool byte = m_op & (1u << 22), wb = m_op & (1u << 21), load = m_op & (1u << 20);
    const uint8_t rn = (m_op >> 16) & 0x0F, rd = (m_op >> 12) & 0x0F;
    uint32_t addr = m_r[rn];
    uint32_t offset;
    if (imm) {
        offset = m_op & 0xFFF;
    } else {
        const uint8_t type = (m_op >> 5) & 0x03, s = (m_op >> 7) & 0x1F;
        offset = m_r[m_op & 0x0F];
        bool carry = m_cpsr.c;
        switch (type) {
        case 0: offset = shift(0, offset, s, carry); break;
        case 1: offset = shift(1, offset, s ? s : 32, carry); break;
        case 2: offset = shift(2, offset, s ? s : 32, carry); break;
        default:
            if (s == 0) offset = (offset >> 1) | ((uint32_t)carry << 31);
            else offset = shift(3, offset, s, carry);
            break;
        }
    }
    if (pre) addr += up ? offset : (uint32_t)-offset;
    const uint8_t mode = byte ? BYTE : WORD;
    if (load) {
        setR(rd, read(mode, addr));
        idle();
    } else {
        write(mode, addr, m_r[rd] + (rd == 15 ? 4 : 0));
    }
    if (!pre) addr += up ? offset : (uint32_t)-offset;
    if ((rd != rn || !load) && (wb || !pre)) setR(rn, addr);
}

void St018::opBlockTransfer() {
    const bool pre = m_op & (1u << 24), up = m_op & (1u << 23), forceUser = m_op & (1u << 22);
    const bool wb = m_op & (1u << 21), load = m_op & (1u << 20);
    const uint8_t rn = (m_op >> 16) & 0x0F;
    uint16_t list = (uint16_t)m_op;
    const uint32_t base = m_r[rn] + (rn == 15 ? 4 : 0);
    uint32_t addr = base;
    uint32_t count = 0;
    for (int i = 0; i < 16; ++i) count += (list >> i) & 1;
    if (!list) { count = 16; list = 0x8000; }     // an empty list moves R15 but steps as if all 16
    if (!up) addr -= (count - (pre ? 0 : 1)) * 4;
    else if (pre) addr += 4;
    const uint32_t final = base + (up ? count * 4 : (uint32_t)-(int32_t)(count * 4));
    if (wb && load) setR(rn, final);
    const uint8_t orgMode = m_cpsr.mode;
    if (forceUser && (!load || !(list & 0x8000))) switchMode(USR);
    bool first = true;
    uint8_t mode = WORD;
    for (int i = 0; i < 16; ++i) {
        if (!(list & (1u << i))) continue;
        if (!load) write(mode, addr, m_r[i] + (i == 15 ? 4 : 0));
        if (first && wb) { setR(rn, final); first = false; }
        if (load) setR((uint8_t)i, read(mode | NO_ROTATE, addr));
        mode |= SEQ;
        addr += 4;
    }
    if (load) idle();
    switchMode(orgMode);
    if (forceUser && load && (list & 0x8000)) {
        const Psr p = spsr();
        switchMode(p.mode);
        m_cpsr = p;
    }
}

void St018::opSwap() {
    const bool byte = m_op & (1u << 22);
    const uint8_t rn = (m_op >> 16) & 0x0F, rd = (m_op >> 12) & 0x0F, rm = m_op & 0x0F;
    const uint8_t mode = byte ? BYTE : WORD;
    const uint32_t at = m_r[rn];
    const uint32_t v = read(mode, at);
    idle();
    write(mode, at, rm == 15 ? m_r[rm] + 4 : m_r[rm]);
    setR(rd, v);
}

}  // namespace snes
