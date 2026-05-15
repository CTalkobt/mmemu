#include "cpu6502.h"
#include "libtoolchain/main/idisasm.h"
#include "libdebug/main/execution_observer.h"
#include "mmemu_plugin_api.h"
#include <cstdio>
#include <cstring>

static const RegDescriptor s_regDescriptors[] = {
    { "A",      RegWidth::R8,  REGFLAG_ACCUMULATOR, nullptr, nullptr    },
    { "X",      RegWidth::R8,  0,                   nullptr, nullptr    },
    { "Y",      RegWidth::R8,  0,                   nullptr, nullptr    },
    { "SP",     RegWidth::R8,  REGFLAG_SP,          nullptr, nullptr    },
    { "PC",     RegWidth::R16, REGFLAG_PC,          nullptr, nullptr    },
    { "P",      RegWidth::R8,  REGFLAG_STATUS,      "F",     "NV-BDIZC" },
    { "cycles_lo", RegWidth::R32, REGFLAG_INTERNAL,  nullptr, nullptr   },
    { "cycles_hi", RegWidth::R32, REGFLAG_INTERNAL,  nullptr, nullptr   }
};

MOS6502::MOS6502() {
    MOS6502::reset();
}

int MOS6502::regCount() const {
    return sizeof(s_regDescriptors) / sizeof(RegDescriptor);
}

const RegDescriptor* MOS6502::regDescriptor(int idx) const {
    if (idx >= 0 && idx < regCount()) return &s_regDescriptors[idx];
    return nullptr;
}

uint32_t MOS6502::regRead(int idx) const {
    switch (idx) {
        case 0: return m_state.a;
        case 1: return m_state.x;
        case 2: return m_state.y;
        case 3: return m_state.sp;
        case 4: return m_state.pc;
        case 5: return m_state.p;
        case 6: return (uint32_t)(m_state.cycles & 0xFFFFFFFFu);
        case 7: return (uint32_t)(m_state.cycles >> 32);
        default: return 0;
    }
}

void MOS6502::regWrite(int idx, uint32_t val) {
    switch (idx) {
        case 0: m_state.a = (uint8_t)val; break;
        case 1: m_state.x = (uint8_t)val; break;
        case 2: m_state.y = (uint8_t)val; break;
        case 3: m_state.sp = (uint8_t)val; break;
        case 4: m_state.pc = (uint16_t)val; break;
        case 5: m_state.p = (uint8_t)val; break;
        case 6: m_state.cycles = (m_state.cycles & 0xFFFFFFFF00000000ull) | val; break;
        case 7: m_state.cycles = ((uint64_t)val << 32) | (m_state.cycles & 0xFFFFFFFFu); break;
    }
}

void MOS6502::reset() {
    m_state.a = 0;
    m_state.x = 0;
    m_state.y = 0;
    m_state.sp = 0xFD;
    m_state.p = FLAG_U | FLAG_I;
    m_state.cycles = 0;
    m_state.irqLine = 0;
    m_state.nmiLine = 0;
    m_state.nmiPrev = 0;
    m_state.haltLine = 0;

    if (m_bus) {
        uint8_t lo = m_bus->read8(0xFFFC);
        uint8_t hi = m_bus->read8(0xFFFD);
        m_state.pc = (uint16_t)lo | ((uint16_t)hi << 8);
    } else {
        m_state.pc = 0;
    }
}

void MOS6502::triggerIrq() {
    // Treat triggerIrq as a toggle or momentary assertion.
    // For PET we use setIrqLine(true/false) which is cleaner.
    m_state.irqLine = 1;
}

void MOS6502::triggerNmi() {
    // NMIs are edge-triggered (falling edge).
    m_state.nmiLine = 1; 
    m_state.nmiPrev = 0; 
}

void MOS6502::saveState(uint8_t* buf) const {
    std::memcpy(buf, &m_state, sizeof(CPU6502State));
}

void MOS6502::loadState(const uint8_t* buf) {
    std::memcpy(&m_state, buf, sizeof(CPU6502State));
}

uint8_t MOS6502::read(uint16_t addr) {
    return m_bus ? m_bus->read8(addr) : 0;
}

void MOS6502::write(uint16_t addr, uint8_t val) {
    if (m_bus) {
        m_bus->write8(addr, val);
    }
}

void MOS6502::push(uint8_t val) {
    write(0x0100 | m_state.sp--, val);
}

uint8_t MOS6502::pop() {
    return read(0x0100 | ++m_state.sp);
}

void MOS6502::updateNZ(uint8_t val) {
    m_state.p &= ~(FLAG_N | FLAG_Z);
    if (val == 0) m_state.p |= FLAG_Z;
    if (val & 0x80) m_state.p |= FLAG_N;
}

int MOS6502::disassembleOne(IBus* bus, uint32_t addr, char* buf, int bufsz) {
    uint8_t op = bus->peek8(addr);
    const Opcode& info = s_opcodes[op];
    
    int bytes = 1;
    char operands[32] = "";

    switch (info.mode) {
        case Mode::IMP:    break;
        case Mode::ACC:    std::snprintf(operands, sizeof(operands), "A"); break;
        case Mode::IMM:    std::snprintf(operands, sizeof(operands), "#$%02X", bus->peek8(addr+1)); bytes=2; break;
        case Mode::BP:     std::snprintf(operands, sizeof(operands), "$%02X", bus->peek8(addr+1)); bytes=2; break;
        case Mode::BPX:    std::snprintf(operands, sizeof(operands), "$%02X,X", bus->peek8(addr+1)); bytes=2; break;
        case Mode::BPY:    std::snprintf(operands, sizeof(operands), "$%02X,Y", bus->peek8(addr+1)); bytes=2; break;
        case Mode::REL: {
            int8_t rel = (int8_t)bus->peek8(addr+1);
            std::snprintf(operands, sizeof(operands), "$%04X", (uint16_t)(addr + 2 + rel));
            bytes=2;
            break;
        }
        case Mode::ABS: {
            uint16_t target = bus->peek8(addr+1) | (bus->peek8(addr+2) << 8);
            std::snprintf(operands, sizeof(operands), "$%04X", target);
            bytes=3;
            break;
        }
        case Mode::ABSX: {
            uint16_t target = bus->peek8(addr+1) | (bus->peek8(addr+2) << 8);
            std::snprintf(operands, sizeof(operands), "$%04X,X", target);
            bytes=3;
            break;
        }
        case Mode::ABSY: {
            uint16_t target = bus->peek8(addr+1) | (bus->peek8(addr+2) << 8);
            std::snprintf(operands, sizeof(operands), "$%04X,Y", target);
            bytes=3;
            break;
        }
        case Mode::IND: {
            uint16_t target = bus->peek8(addr+1) | (bus->peek8(addr+2) << 8);
            std::snprintf(operands, sizeof(operands), "($%04X)", target);
            bytes=3;
            break;
        }
        case Mode::BPXIND: std::snprintf(operands, sizeof(operands), "($%02X,X)", bus->peek8(addr+1)); bytes=2; break;
        case Mode::INDBPY: std::snprintf(operands, sizeof(operands), "($%02X),Y", bus->peek8(addr+1)); bytes=2; break;
    }

    std::snprintf(buf, bufsz, "%-4s %s", info.mnemonic, operands);
    return bytes;
}

int MOS6502::disassembleEntry(IBus* bus, uint32_t addr, void* entryOut) {
    DisasmEntry* entry = (DisasmEntry*)entryOut;
    uint8_t op = bus->peek8(addr);
    const Opcode& info = s_opcodes[op];

    entry->addr = addr;
    entry->mnemonic = info.mnemonic;
    
    char buf[64];
    entry->bytes = disassembleOne(bus, addr, buf, sizeof(buf));
    
    // Split buf into mnemonic and operands for entry
    char mnem[16];
    char ops[48];
    if (std::sscanf(buf, "%s %[^\n]", mnem, ops) == 2) {
        entry->operands = ops;
    } else {
        entry->operands = "";
    }
    entry->complete = buf;

    entry->isCall   = (op == 0x20); // JSR
    entry->isReturn = (op == 0x60 || op == 0x40); // RTS or RTI
    entry->isBranch = (info.mode == Mode::REL);

    // Mark unofficial/illegal opcodes.  All handlers that exist exclusively
    // for undocumented opcodes are listed here.  opNOP and opSBC each serve
    // one official opcode ($EA and $E9 respectively), so we special-case them.
    auto fn = info.fn;
    entry->isIllegal =
        fn == &MOS6502::opKIL || fn == &MOS6502::opSLO || fn == &MOS6502::opRLA ||
        fn == &MOS6502::opSRE || fn == &MOS6502::opRRA || fn == &MOS6502::opSAX ||
        fn == &MOS6502::opLAX || fn == &MOS6502::opDCP || fn == &MOS6502::opISC ||
        fn == &MOS6502::opANC || fn == &MOS6502::opALR || fn == &MOS6502::opARR ||
        fn == &MOS6502::opXAA || fn == &MOS6502::opAHX || fn == &MOS6502::opTAS ||
        fn == &MOS6502::opSHY || fn == &MOS6502::opSHX || fn == &MOS6502::opLAS ||
        fn == &MOS6502::opAXS ||
        (fn == &MOS6502::opNOP && op != 0xEA) ||   // $EA is the only official NOP
        (fn == &MOS6502::opSBC && op == 0xEB);      // $EB is the unofficial SBC
    
    if (entry->isCall || entry->isBranch || op == 0x4C || op == 0x6C) {
        // Calculate targetAddr
        if (info.mode == Mode::REL) {
            int8_t rel = (int8_t)bus->peek8(addr + 1);
            entry->targetAddr = (uint16_t)(addr + 2 + rel);
        } else if (info.mode == Mode::ABS || info.mode == Mode::IND) {
            entry->targetAddr = bus->peek8(addr+1) | (bus->peek8(addr+2) << 8);
        } else {
            entry->targetAddr = 0;
        }
    } else {
        entry->targetAddr = 0;
    }

    return entry->bytes;
}

int MOS6502::isCallAt(IBus* bus, uint32_t addr) {
    return (bus->peek8(addr) == 0x20) ? 3 : 0;
}

bool MOS6502::isReturnAt(IBus* bus, uint32_t addr) {
    uint8_t op = bus->peek8(addr);
    return (op == 0x60 || op == 0x40);
}

bool MOS6502::isProgramEnd(IBus* bus) {
    // Check for RTS/RTI on empty stack (SP at initial value)
    if (m_state.sp == 0xFF) {
        uint8_t op = bus->peek8(m_state.pc);
        if (op == 0x60 || op == 0x40) return true; // RTS, RTI
    }

    if (bus->peek8(m_state.pc) == 0x4C) {
        uint8_t lo = bus->peek8(m_state.pc + 1);
        uint8_t hi = bus->peek8(m_state.pc + 2);
        uint16_t target = (uint16_t)lo | ((uint16_t)hi << 8);
        return target == m_state.pc;
    }
    return false;
}

int MOS6502::writeCallHarness(IBus* bus, uint32_t scratch, uint32_t routineAddr) {
    bus->write8(scratch, 0x20); // JSR
    bus->write8(scratch + 1, routineAddr & 0xFF);
    bus->write8(scratch + 2, (routineAddr >> 8) & 0xFF);
    bus->write8(scratch + 3, 0x00); // BRK (or some halt)
    return 4;
}

// Addressing modes
uint16_t MOS6502::addrBP() { return read(m_state.pc++); }
uint16_t MOS6502::addrBPX() { return (read(m_state.pc++) + m_state.x) & 0xFF; }
uint16_t MOS6502::addrBPY() { return (read(m_state.pc++) + m_state.y) & 0xFF; }
uint16_t MOS6502::addrABS() {
    uint16_t lo = read(m_state.pc++);
    uint16_t hi = read(m_state.pc++);
    return lo | (hi << 8);
}
uint16_t MOS6502::addrABSX(bool& pageCrossed) {
    uint16_t base = addrABS();
    uint16_t addr = base + m_state.x;
    pageCrossed = (base & 0xFF00) != (addr & 0xFF00);
    return addr;
}
uint16_t MOS6502::addrABSY(bool& pageCrossed) {
    uint16_t base = addrABS();
    uint16_t addr = base + m_state.y;
    pageCrossed = (base & 0xFF00) != (addr & 0xFF00);
    return addr;
}
uint16_t MOS6502::addrIND() {
    uint16_t ptr = addrABS();
    uint16_t lo = read(ptr);
    uint16_t hi = read((ptr & 0xFF00) | ((ptr + 1) & 0x00FF));
    return lo | (hi << 8);
}
uint16_t MOS6502::addrBPXIND() {
    uint8_t ptr = (read(m_state.pc++) + m_state.x) & 0xFF;
    uint16_t lo = read(ptr);
    uint16_t hi = read((ptr + 1) & 0xFF);
    return lo | (hi << 8);
}
uint16_t MOS6502::addrINDBPY(bool& pageCrossed) {
    uint8_t ptr = read(m_state.pc++);
    uint16_t lo = read(ptr);
    uint16_t hi = read((ptr + 1) & 0xFF);
    uint16_t base = lo | (hi << 8);
    uint16_t addr = base + m_state.y;
    pageCrossed = (base & 0xFF00) != (addr & 0xFF00);
    return addr;
}

// Opcode Implementations
int MOS6502::opADC(uint16_t addr) {
    uint8_t m = read(addr);
    uint16_t sum = (uint16_t)m_state.a + (uint16_t)m + (uint16_t)(m_state.p & FLAG_C);
    m_state.p &= ~(FLAG_C | FLAG_V | FLAG_Z | FLAG_N);
    if (sum > 0xFF) m_state.p |= FLAG_C;
    if (~((uint16_t)m_state.a ^ (uint16_t)m) & ((uint16_t)m_state.a ^ sum) & 0x0080) m_state.p |= FLAG_V;
    m_state.a = (uint8_t)sum;
    updateNZ(m_state.a);
    return 0;
}

int MOS6502::opAND(uint16_t addr) {
    m_state.a &= read(addr);
    updateNZ(m_state.a);
    return 0;
}

int MOS6502::opASL(uint16_t addr) {
    uint8_t m;
    if (addr == 0xFFFF) { // Flag for Accumulator mode in this implementation
        m = m_state.a;
        m_state.p &= ~FLAG_C;
        if (m & 0x80) m_state.p |= FLAG_C;
        m_state.a = m << 1;
        updateNZ(m_state.a);
    } else {
        m = read(addr);
        m_state.p &= ~FLAG_C;
        if (m & 0x80) m_state.p |= FLAG_C;
        m <<= 1;
        write(addr, m);
        updateNZ(m);
    }
    return 0;
}

int MOS6502::opBCC(uint16_t addr) {
    if (!(m_state.p & FLAG_C)) {
        uint16_t old_pc = m_state.pc;
        m_state.pc = addr;
        int c = 1;
        if ((old_pc & 0xFF00) != (m_state.pc & 0xFF00)) c++;
        return c;
    }
    return 0;
}

int MOS6502::opBCS(uint16_t addr) {
    if (m_state.p & FLAG_C) {
        uint16_t old_pc = m_state.pc;
        m_state.pc = addr;
        int c = 1;
        if ((old_pc & 0xFF00) != (m_state.pc & 0xFF00)) c++;
        return c;
    }
    return 0;
}

int MOS6502::opBEQ(uint16_t addr) {
    if (m_state.p & FLAG_Z) {
        uint16_t old_pc = m_state.pc;
        m_state.pc = addr;
        int c = 1;
        if ((old_pc & 0xFF00) != (m_state.pc & 0xFF00)) c++;
        return c;
    }
    return 0;
}

int MOS6502::opBIT(uint16_t addr) {
    uint8_t m = read(addr);
    m_state.p &= ~(FLAG_Z | FLAG_V | FLAG_N);
    if (!(m_state.a & m)) m_state.p |= FLAG_Z;
    if (m & 0x40) m_state.p |= FLAG_V;
    if (m & 0x80) m_state.p |= FLAG_N;
    return 0;
}

int MOS6502::opBMI(uint16_t addr) {
    if (m_state.p & FLAG_N) {
        uint16_t old_pc = m_state.pc;
        m_state.pc = addr;
        int c = 1;
        if ((old_pc & 0xFF00) != (m_state.pc & 0xFF00)) c++;
        return c;
    }
    return 0;
}

int MOS6502::opBNE(uint16_t addr) {
    if (!(m_state.p & FLAG_Z)) {
        uint16_t old_pc = m_state.pc;
        m_state.pc = addr;
        int c = 1;
        if ((old_pc & 0xFF00) != (m_state.pc & 0xFF00)) c++;
        return c;
    }
    return 0;
}

int MOS6502::opBPL(uint16_t addr) {
    if (!(m_state.p & FLAG_N)) {
        uint16_t old_pc = m_state.pc;
        m_state.pc = addr;
        int c = 1;
        if ((old_pc & 0xFF00) != (m_state.pc & 0xFF00)) c++;
        return c;
    }
    return 0;
}

int MOS6502::opBRK(uint16_t addr) {
    (void)addr;
    m_state.pc++;
    push((uint8_t)(m_state.pc >> 8));
    push((uint8_t)(m_state.pc & 0xFF));
    // BRK pushes P with bit 4 (B) and bit 5 (U) set.
    push(m_state.p | FLAG_B | FLAG_U);
    m_state.p |= FLAG_I;
    uint8_t lo = read(0xFFFE);
    uint8_t hi = read(0xFFFF);
    m_state.pc = lo | (hi << 8);
    return 0;
}

int MOS6502::opBVC(uint16_t addr) {
    if (!(m_state.p & FLAG_V)) {
        uint16_t old_pc = m_state.pc;
        m_state.pc = addr;
        int c = 1;
        if ((old_pc & 0xFF00) != (m_state.pc & 0xFF00)) c++;
        return c;
    }
    return 0;
}

int MOS6502::opBVS(uint16_t addr) {
    if (m_state.p & FLAG_V) {
        uint16_t old_pc = m_state.pc;
        m_state.pc = addr;
        int c = 1;
        if ((old_pc & 0xFF00) != (m_state.pc & 0xFF00)) c++;
        return c;
    }
    return 0;
}

int MOS6502::opCLC(uint16_t addr) { (void)addr; m_state.p &= ~FLAG_C; return 0; }
int MOS6502::opCLD(uint16_t addr) { (void)addr; m_state.p &= ~FLAG_D; return 0; }
int MOS6502::opCLI(uint16_t addr) { (void)addr; m_state.p &= ~FLAG_I; return 0; }
int MOS6502::opCLV(uint16_t addr) { (void)addr; m_state.p &= ~FLAG_V; return 0; }

int MOS6502::opCMP(uint16_t addr) {
    uint8_t m = read(addr);
    uint16_t res = (uint16_t)m_state.a - (uint16_t)m;
    m_state.p &= ~FLAG_C;
    if (m_state.a >= m) m_state.p |= FLAG_C;
    updateNZ((uint8_t)res);
    return 0;
}

int MOS6502::opCPX(uint16_t addr) {
    uint8_t m = read(addr);
    uint16_t res = (uint16_t)m_state.x - (uint16_t)m;
    m_state.p &= ~FLAG_C;
    if (m_state.x >= m) m_state.p |= FLAG_C;
    updateNZ((uint8_t)res);
    return 0;
}

int MOS6502::opCPY(uint16_t addr) {
    uint8_t m = read(addr);
    uint16_t res = (uint16_t)m_state.y - (uint16_t)m;
    m_state.p &= ~FLAG_C;
    if (m_state.y >= m) m_state.p |= FLAG_C;
    updateNZ((uint8_t)res);
    return 0;
}

int MOS6502::opDEC(uint16_t addr) {
    uint8_t m = read(addr) - 1;
    write(addr, m);
    updateNZ(m);
    return 0;
}

int MOS6502::opDEX(uint16_t addr) { (void)addr; m_state.x--; updateNZ(m_state.x); return 0; }
int MOS6502::opDEY(uint16_t addr) { (void)addr; m_state.y--; updateNZ(m_state.y); return 0; }

int MOS6502::opEOR(uint16_t addr) {
    m_state.a ^= read(addr);
    updateNZ(m_state.a);
    return 0;
}

int MOS6502::opINC(uint16_t addr) {
    uint8_t m = read(addr) + 1;
    write(addr, m);
    updateNZ(m);
    return 0;
}

int MOS6502::opINX(uint16_t addr) { (void)addr; m_state.x++; updateNZ(m_state.x); return 0; }
int MOS6502::opINY(uint16_t addr) { (void)addr; m_state.y++; updateNZ(m_state.y); return 0; }

int MOS6502::opJMP(uint16_t addr) {
    m_state.pc = addr;
    return 0;
}

int MOS6502::opJSR(uint16_t addr) {
    uint16_t ret = m_state.pc - 1;
    push((uint8_t)(ret >> 8));
    push((uint8_t)(ret & 0xFF));
    m_state.pc = addr;
    return 0;
}

int MOS6502::opLDA(uint16_t addr) { m_state.a = read(addr); updateNZ(m_state.a); return 0; }
int MOS6502::opLDX(uint16_t addr) { m_state.x = read(addr); updateNZ(m_state.x); return 0; }
int MOS6502::opLDY(uint16_t addr) { m_state.y = read(addr); updateNZ(m_state.y); return 0; }

int MOS6502::opLSR(uint16_t addr) {
    uint8_t m;
    if (addr == 0xFFFF) {
        m = m_state.a;
        m_state.p &= ~FLAG_C;
        if (m & 0x01) m_state.p |= FLAG_C;
        m_state.a = m >> 1;
        updateNZ(m_state.a);
    } else {
        m = read(addr);
        m_state.p &= ~FLAG_C;
        if (m & 0x01) m_state.p |= FLAG_C;
        m >>= 1;
        write(addr, m);
        updateNZ(m);
    }
    return 0;
}

int MOS6502::opNOP(uint16_t addr) { (void)addr; return 0; }

int MOS6502::opORA(uint16_t addr) {
    m_state.a |= read(addr);
    updateNZ(m_state.a);
    return 0;
}

int MOS6502::opPHA(uint16_t addr) { (void)addr; push(m_state.a); return 0; }
int MOS6502::opPHP(uint16_t addr) { (void)addr; push(m_state.p | FLAG_B); return 0; }
int MOS6502::opPLA(uint16_t addr) { (void)addr; m_state.a = pop(); updateNZ(m_state.a); return 0; }
int MOS6502::opPLP(uint16_t addr) { (void)addr; m_state.p = (pop() & ~FLAG_B) | FLAG_U; return 0; }

int MOS6502::opROL(uint16_t addr) {
    uint8_t m;
    uint8_t c = (m_state.p & FLAG_C) ? 1 : 0;
    if (addr == 0xFFFF) {
        m = m_state.a;
        m_state.p &= ~FLAG_C;
        if (m & 0x80) m_state.p |= FLAG_C;
        m_state.a = (m << 1) | c;
        updateNZ(m_state.a);
    } else {
        m = read(addr);
        m_state.p &= ~FLAG_C;
        if (m & 0x80) m_state.p |= FLAG_C;
        m = (m << 1) | c;
        write(addr, m);
        updateNZ(m);
    }
    return 0;
}

int MOS6502::opROR(uint16_t addr) {
    uint8_t m;
    uint8_t c = (m_state.p & FLAG_C) ? 0x80 : 0;
    if (addr == 0xFFFF) {
        m = m_state.a;
        m_state.p &= ~FLAG_C;
        if (m & 0x01) m_state.p |= FLAG_C;
        m_state.a = (m >> 1) | c;
        updateNZ(m_state.a);
    } else {
        m = read(addr);
        m_state.p &= ~FLAG_C;
        if (m & 0x01) m_state.p |= FLAG_C;
        m = (m >> 1) | c;
        write(addr, m);
        updateNZ(m);
    }
    return 0;
}

int MOS6502::opRTI(uint16_t addr) {
    (void)addr;
    m_state.p = (pop() & ~FLAG_B) | FLAG_U;
    uint8_t lo = pop();
    uint8_t hi = pop();
    m_state.pc = lo | (hi << 8);
    return 0;
}

int MOS6502::opRTS(uint16_t addr) {
    (void)addr;
    uint8_t lo = pop();
    uint8_t hi = pop();
    m_state.pc = (lo | (hi << 8)) + 1;
    return 0;
}

int MOS6502::opSBC(uint16_t addr) {
    uint8_t m = read(addr) ^ 0xFF;
    uint16_t sum = (uint16_t)m_state.a + (uint16_t)m + (uint16_t)(m_state.p & FLAG_C);
    m_state.p &= ~(FLAG_C | FLAG_V | FLAG_Z | FLAG_N);
    if (sum > 0xFF) m_state.p |= FLAG_C;
    if (~((uint16_t)m_state.a ^ (uint16_t)m) & ((uint16_t)m_state.a ^ sum) & 0x0080) m_state.p |= FLAG_V;
    m_state.a = (uint8_t)sum;
    updateNZ(m_state.a);
    return 0;
}

int MOS6502::opSEC(uint16_t addr) { (void)addr; m_state.p |= FLAG_C; return 0; }
int MOS6502::opSED(uint16_t addr) { (void)addr; m_state.p |= FLAG_D; return 0; }
int MOS6502::opSEI(uint16_t addr) { (void)addr; m_state.p |= FLAG_I; return 0; }

int MOS6502::opSTA(uint16_t addr) {
    write(addr, m_state.a);
    return 0;
}
int MOS6502::opSTX(uint16_t addr) { write(addr, m_state.x); return 0; }
int MOS6502::opSTY(uint16_t addr) { write(addr, m_state.y); return 0; }

int MOS6502::opTAX(uint16_t addr) { (void)addr; m_state.x = m_state.a; updateNZ(m_state.x); return 0; }
int MOS6502::opTAY(uint16_t addr) { (void)addr; m_state.y = m_state.a; updateNZ(m_state.y); return 0; }
int MOS6502::opTSX(uint16_t addr) { (void)addr; m_state.x = m_state.sp; updateNZ(m_state.x); return 0; }
int MOS6502::opTXA(uint16_t addr) { (void)addr; m_state.a = m_state.x; updateNZ(m_state.a); return 0; }
int MOS6502::opTXS(uint16_t addr) { (void)addr; m_state.sp = m_state.x; return 0; }
int MOS6502::opTYA(uint16_t addr) { (void)addr; m_state.a = m_state.y; updateNZ(m_state.a); return 0; }

// Illegal Opcodes
int MOS6502::opKIL(uint16_t addr) { (void)addr; return 0; }
int MOS6502::opSLO(uint16_t addr) { opASL(addr); return opORA(addr); }
int MOS6502::opRLA(uint16_t addr) { opROL(addr); return opAND(addr); }
int MOS6502::opSRE(uint16_t addr) { opLSR(addr); return opEOR(addr); }
int MOS6502::opRRA(uint16_t addr) { opROR(addr); return opADC(addr); }
int MOS6502::opSAX(uint16_t addr) { write(addr, m_state.a & m_state.x); return 0; }
int MOS6502::opLAX(uint16_t addr) { m_state.a = m_state.x = read(addr); updateNZ(m_state.a); return 0; }
int MOS6502::opDCP(uint16_t addr) { write(addr, read(addr) - 1); return opCMP(addr); }
int MOS6502::opISC(uint16_t addr) { write(addr, read(addr) + 1); return opSBC(addr); }
int MOS6502::opANC(uint16_t addr) { (void)addr; m_state.a &= read(m_state.pc++); updateNZ(m_state.a); if (m_state.p & FLAG_N) m_state.p |= FLAG_C; else m_state.p &= ~FLAG_C; return 0; }
int MOS6502::opALR(uint16_t addr) { (void)addr; m_state.a &= read(m_state.pc++); return opLSR(0xFFFF); }
int MOS6502::opARR(uint16_t addr) { (void)addr; m_state.a &= read(m_state.pc++); return opROR(0xFFFF); }
int MOS6502::opXAA(uint16_t addr) { (void)addr; m_state.a = m_state.x & read(m_state.pc++); updateNZ(m_state.a); return 0; }
int MOS6502::opAXS(uint16_t addr) { (void)addr; uint8_t m = read(m_state.pc++); uint8_t res = (m_state.a & m_state.x) - m; m_state.p &= ~FLAG_C; if ((m_state.a & m_state.x) >= m) m_state.p |= FLAG_C; m_state.x = res; updateNZ(m_state.x); return 0; }
int MOS6502::opAHX(uint16_t addr) { (void)addr; m_state.pc += 2; return 0; }
int MOS6502::opTAS(uint16_t addr) { (void)addr; m_state.pc += 2; return 0; }
int MOS6502::opSHY(uint16_t addr) { (void)addr; m_state.pc += 2; return 0; }
int MOS6502::opSHX(uint16_t addr) { (void)addr; m_state.pc += 2; return 0; }
int MOS6502::opLAS(uint16_t addr) { (void)addr; m_state.a = m_state.x = m_state.sp &= read(addrABS()); updateNZ(m_state.a); return 0; }

// Opcode Table
const MOS6502::Opcode MOS6502::s_opcodes[256] = {
    {"BRK", &MOS6502::opBRK, Mode::IMP, 7}, {"ORA", &MOS6502::opORA, Mode::BPXIND, 6}, {"KIL", &MOS6502::opKIL, Mode::IMP, 2}, {"SLO", &MOS6502::opSLO, Mode::BPXIND, 8},
    {"NOP", &MOS6502::opNOP, Mode::BP, 3}, {"ORA", &MOS6502::opORA, Mode::BP, 3}, {"ASL", &MOS6502::opASL, Mode::BP, 5}, {"SLO", &MOS6502::opSLO, Mode::BP, 5},
    {"PHP", &MOS6502::opPHP, Mode::IMP, 3}, {"ORA", &MOS6502::opORA, Mode::IMM, 2}, {"ASL", &MOS6502::opASL, Mode::ACC, 2}, {"ANC", &MOS6502::opANC, Mode::IMM, 2},
    {"NOP", &MOS6502::opNOP, Mode::ABS, 4}, {"ORA", &MOS6502::opORA, Mode::ABS, 4}, {"ASL", &MOS6502::opASL, Mode::ABS, 6}, {"SLO", &MOS6502::opSLO, Mode::ABS, 6},
    {"BPL", &MOS6502::opBPL, Mode::REL, 2}, {"ORA", &MOS6502::opORA, Mode::INDBPY, 5}, {"KIL", &MOS6502::opKIL, Mode::IMP, 2}, {"SLO", &MOS6502::opSLO, Mode::INDBPY, 8},
    {"NOP", &MOS6502::opNOP, Mode::BPX, 4}, {"ORA", &MOS6502::opORA, Mode::BPX, 4}, {"ASL", &MOS6502::opASL, Mode::BPX, 6}, {"SLO", &MOS6502::opSLO, Mode::BPX, 6},
    {"CLC", &MOS6502::opCLC, Mode::IMP, 2}, {"ORA", &MOS6502::opORA, Mode::ABSY, 4}, {"NOP", &MOS6502::opNOP, Mode::IMP, 2}, {"SLO", &MOS6502::opSLO, Mode::ABSY, 7},
    {"NOP", &MOS6502::opNOP, Mode::ABSX, 4}, {"ORA", &MOS6502::opORA, Mode::ABSX, 4}, {"ASL", &MOS6502::opASL, Mode::ABSX, 7}, {"SLO", &MOS6502::opSLO, Mode::ABSX, 7},
    {"JSR", &MOS6502::opJSR, Mode::ABS, 6}, {"AND", &MOS6502::opAND, Mode::BPXIND, 6}, {"KIL", &MOS6502::opKIL, Mode::IMP, 2}, {"RLA", &MOS6502::opRLA, Mode::BPXIND, 8},
    {"BIT", &MOS6502::opBIT, Mode::BP, 3}, {"AND", &MOS6502::opAND, Mode::BP, 3}, {"ROL", &MOS6502::opROL, Mode::BP, 5}, {"RLA", &MOS6502::opRLA, Mode::BP, 5},
    {"PLP", &MOS6502::opPLP, Mode::IMP, 4}, {"AND", &MOS6502::opAND, Mode::IMM, 2}, {"ROL", &MOS6502::opROL, Mode::ACC, 2}, {"ANC", &MOS6502::opANC, Mode::IMM, 2},
    {"BIT", &MOS6502::opBIT, Mode::ABS, 4}, {"AND", &MOS6502::opAND, Mode::ABS, 4}, {"ROL", &MOS6502::opROL, Mode::ABS, 6}, {"RLA", &MOS6502::opRLA, Mode::ABS, 6},
    {"BMI", &MOS6502::opBMI, Mode::REL, 2}, {"AND", &MOS6502::opAND, Mode::INDBPY, 5}, {"KIL", &MOS6502::opKIL, Mode::IMP, 2}, {"RLA", &MOS6502::opRLA, Mode::INDBPY, 8},
    {"NOP", &MOS6502::opNOP, Mode::BPX, 4}, {"AND", &MOS6502::opAND, Mode::BPX, 4}, {"ROL", &MOS6502::opROL, Mode::BPX, 6}, {"RLA", &MOS6502::opRLA, Mode::BPX, 6},
    {"SEC", &MOS6502::opSEC, Mode::IMP, 2}, {"AND", &MOS6502::opAND, Mode::ABSY, 4}, {"NOP", &MOS6502::opNOP, Mode::IMP, 2}, {"RLA", &MOS6502::opRLA, Mode::ABSY, 7},
    {"NOP", &MOS6502::opNOP, Mode::ABSX, 4}, {"AND", &MOS6502::opAND, Mode::ABSX, 4}, {"ROL", &MOS6502::opROL, Mode::ABSX, 7}, {"RLA", &MOS6502::opRLA, Mode::ABSX, 7},
    {"RTI", &MOS6502::opRTI, Mode::IMP, 6}, {"EOR", &MOS6502::opEOR, Mode::BPXIND, 6}, {"KIL", &MOS6502::opKIL, Mode::IMP, 2}, {"SRE", &MOS6502::opSRE, Mode::BPXIND, 8},
    {"NOP", &MOS6502::opNOP, Mode::BP, 3}, {"EOR", &MOS6502::opEOR, Mode::BP, 3}, {"LSR", &MOS6502::opLSR, Mode::BP, 5}, {"SRE", &MOS6502::opSRE, Mode::BP, 5},
    {"PHA", &MOS6502::opPHA, Mode::IMP, 3}, {"EOR", &MOS6502::opEOR, Mode::IMM, 2}, {"LSR", &MOS6502::opLSR, Mode::ACC, 2}, {"ALR", &MOS6502::opALR, Mode::IMM, 2},
    {"JMP", &MOS6502::opJMP, Mode::ABS, 3}, {"EOR", &MOS6502::opEOR, Mode::ABS, 4}, {"LSR", &MOS6502::opLSR, Mode::ABS, 6}, {"SRE", &MOS6502::opSRE, Mode::ABS, 6},
    {"BVC", &MOS6502::opBVC, Mode::REL, 2}, {"EOR", &MOS6502::opEOR, Mode::INDBPY, 5}, {"KIL", &MOS6502::opKIL, Mode::IMP, 2}, {"SRE", &MOS6502::opSRE, Mode::INDBPY, 8},
    {"NOP", &MOS6502::opNOP, Mode::BPX, 4}, {"EOR", &MOS6502::opEOR, Mode::BPX, 4}, {"LSR", &MOS6502::opLSR, Mode::BPX, 6}, {"SRE", &MOS6502::opSRE, Mode::BPX, 6},
    {"CLI", &MOS6502::opCLI, Mode::IMP, 2}, {"EOR", &MOS6502::opEOR, Mode::ABSY, 4}, {"NOP", &MOS6502::opNOP, Mode::IMP, 2}, {"SRE", &MOS6502::opSRE, Mode::ABSY, 7},
    {"NOP", &MOS6502::opNOP, Mode::ABSX, 4}, {"EOR", &MOS6502::opEOR, Mode::ABSX, 4}, {"LSR", &MOS6502::opLSR, Mode::ABSX, 7}, {"SRE", &MOS6502::opSRE, Mode::ABSX, 7},
    {"RTS", &MOS6502::opRTS, Mode::IMP, 6}, {"ADC", &MOS6502::opADC, Mode::BPXIND, 6}, {"KIL", &MOS6502::opKIL, Mode::IMP, 2}, {"RRA", &MOS6502::opRRA, Mode::BPXIND, 8},
    {"NOP", &MOS6502::opNOP, Mode::BP, 3}, {"ADC", &MOS6502::opADC, Mode::BP, 3}, {"ROR", &MOS6502::opROR, Mode::BP, 5}, {"RRA", &MOS6502::opRRA, Mode::BP, 5},
    {"PLA", &MOS6502::opPLA, Mode::IMP, 4}, {"ADC", &MOS6502::opADC, Mode::IMM, 2}, {"ROR", &MOS6502::opROR, Mode::ACC, 2}, {"ARR", &MOS6502::opARR, Mode::IMM, 2},
    {"JMP", &MOS6502::opJMP, Mode::IND, 5}, {"ADC", &MOS6502::opADC, Mode::ABS, 4}, {"ROR", &MOS6502::opROR, Mode::ABS, 6}, {"RRA", &MOS6502::opRRA, Mode::ABS, 6},
    {"BVS", &MOS6502::opBVS, Mode::REL, 2}, {"ADC", &MOS6502::opADC, Mode::INDBPY, 5}, {"KIL", &MOS6502::opKIL, Mode::IMP, 2}, {"RRA", &MOS6502::opRRA, Mode::INDBPY, 8},
    {"NOP", &MOS6502::opNOP, Mode::BPX, 4}, {"ADC", &MOS6502::opADC, Mode::BPX, 4}, {"ROR", &MOS6502::opROR, Mode::BPX, 6}, {"RRA", &MOS6502::opRRA, Mode::BPX, 6},
    {"SEI", &MOS6502::opSEI, Mode::IMP, 2}, {"ADC", &MOS6502::opADC, Mode::ABSY, 4}, {"NOP", &MOS6502::opNOP, Mode::IMP, 2}, {"RRA", &MOS6502::opRRA, Mode::ABSY, 7},
    {"NOP", &MOS6502::opNOP, Mode::ABSX, 4}, {"ADC", &MOS6502::opADC, Mode::ABSX, 4}, {"ROR", &MOS6502::opROR, Mode::ABSX, 7}, {"RRA", &MOS6502::opRRA, Mode::ABSX, 7},
    {"NOP", &MOS6502::opNOP, Mode::IMM, 2}, {"STA", &MOS6502::opSTA, Mode::BPXIND, 6}, {"NOP", &MOS6502::opNOP, Mode::IMM, 2}, {"SAX", &MOS6502::opSAX, Mode::BPXIND, 6},
    {"STY", &MOS6502::opSTY, Mode::BP, 3}, {"STA", &MOS6502::opSTA, Mode::BP, 3}, {"STX", &MOS6502::opSTX, Mode::BP, 3}, {"SAX", &MOS6502::opSAX, Mode::BP, 3},
    {"DEY", &MOS6502::opDEY, Mode::IMP, 2}, {"NOP", &MOS6502::opNOP, Mode::IMM, 2}, {"TXA", &MOS6502::opTXA, Mode::IMP, 2}, {"XAA", &MOS6502::opXAA, Mode::IMM, 2},
    {"STY", &MOS6502::opSTY, Mode::ABS, 4}, {"STA", &MOS6502::opSTA, Mode::ABS, 4}, {"STX", &MOS6502::opSTX, Mode::ABS, 4}, {"SAX", &MOS6502::opSAX, Mode::ABS, 4},
    {"BCC", &MOS6502::opBCC, Mode::REL, 2}, {"STA", &MOS6502::opSTA, Mode::INDBPY, 6}, {"KIL", &MOS6502::opKIL, Mode::IMP, 2}, {"AHX", &MOS6502::opAHX, Mode::INDBPY, 6},
    {"STY", &MOS6502::opSTY, Mode::BPX, 4}, {"STA", &MOS6502::opSTA, Mode::BPX, 4}, {"STX", &MOS6502::opSTX, Mode::BPY, 4}, {"SAX", &MOS6502::opSAX, Mode::BPY, 4},
    {"TYA", &MOS6502::opTYA, Mode::IMP, 2}, {"STA", &MOS6502::opSTA, Mode::ABSY, 5}, {"TXS", &MOS6502::opTXS, Mode::IMP, 2}, {"TAS", &MOS6502::opTAS, Mode::ABSY, 5},
    {"SHY", &MOS6502::opSHY, Mode::ABSX, 5}, {"STA", &MOS6502::opSTA, Mode::ABSX, 5}, {"SHX", &MOS6502::opSHX, Mode::ABSY, 5}, {"AHX", &MOS6502::opAHX, Mode::ABSY, 5},
    {"LDY", &MOS6502::opLDY, Mode::IMM, 2}, {"LDA", &MOS6502::opLDA, Mode::BPXIND, 6}, {"LDX", &MOS6502::opLDX, Mode::IMM, 2}, {"LAX", &MOS6502::opLAX, Mode::BPXIND, 6},
    {"LDY", &MOS6502::opLDY, Mode::BP, 3}, {"LDA", &MOS6502::opLDA, Mode::BP, 3}, {"LDX", &MOS6502::opLDX, Mode::BP, 3}, {"LAX", &MOS6502::opLAX, Mode::BP, 3},
    {"TAY", &MOS6502::opTAY, Mode::IMP, 2}, {"LDA", &MOS6502::opLDA, Mode::IMM, 2}, {"TAX", &MOS6502::opTAX, Mode::IMP, 2}, {"LAX", &MOS6502::opLAX, Mode::IMM, 2},
    {"LDY", &MOS6502::opLDY, Mode::ABS, 4}, {"LDA", &MOS6502::opLDA, Mode::ABS, 4}, {"LDX", &MOS6502::opLDX, Mode::ABS, 4}, {"LAX", &MOS6502::opLAX, Mode::ABS, 4},
    {"BCS", &MOS6502::opBCS, Mode::REL, 2}, {"LDA", &MOS6502::opLDA, Mode::INDBPY, 5}, {"KIL", &MOS6502::opKIL, Mode::IMP, 2}, {"LAX", &MOS6502::opLAX, Mode::INDBPY, 5},
    {"LDY", &MOS6502::opLDY, Mode::BPX, 4}, {"LDA", &MOS6502::opLDA, Mode::BPX, 4}, {"LDX", &MOS6502::opLDX, Mode::BPY, 4}, {"LAX", &MOS6502::opLAX, Mode::BPY, 4},
    {"CLV", &MOS6502::opCLV, Mode::IMP, 2}, {"LDA", &MOS6502::opLDA, Mode::ABSY, 4}, {"TSX", &MOS6502::opTSX, Mode::IMP, 2}, {"LAS", &MOS6502::opLAS, Mode::ABSY, 4},
    {"LDY", &MOS6502::opLDY, Mode::ABSX, 4}, {"LDA", &MOS6502::opLDA, Mode::ABSX, 4}, {"LDX", &MOS6502::opLDX, Mode::ABSY, 4}, {"LAX", &MOS6502::opLAX, Mode::ABSY, 4},
    {"CPY", &MOS6502::opCPY, Mode::IMM, 2}, {"CMP", &MOS6502::opCMP, Mode::BPXIND, 6}, {"NOP", &MOS6502::opNOP, Mode::IMM, 2}, {"DCP", &MOS6502::opDCP, Mode::BPXIND, 8},
    {"CPY", &MOS6502::opCPY, Mode::BP, 3}, {"CMP", &MOS6502::opCMP, Mode::BP, 3}, {"DEC", &MOS6502::opDEC, Mode::BP, 5}, {"DCP", &MOS6502::opDCP, Mode::BP, 5},
    {"INY", &MOS6502::opINY, Mode::IMP, 2}, {"CMP", &MOS6502::opCMP, Mode::IMM, 2}, {"DEX", &MOS6502::opDEX, Mode::IMP, 2}, {"AXS", &MOS6502::opAXS, Mode::IMM, 2},
    {"CPY", &MOS6502::opCPY, Mode::ABS, 4}, {"CMP", &MOS6502::opCMP, Mode::ABS, 4}, {"DEC", &MOS6502::opDEC, Mode::ABS, 6}, {"DCP", &MOS6502::opDCP, Mode::ABS, 6},
    {"BNE", &MOS6502::opBNE, Mode::REL, 2}, {"CMP", &MOS6502::opCMP, Mode::INDBPY, 5}, {"KIL", &MOS6502::opKIL, Mode::IMP, 2}, {"DCP", &MOS6502::opDCP, Mode::INDBPY, 8},
    {"NOP", &MOS6502::opNOP, Mode::BPX, 4}, {"CMP", &MOS6502::opCMP, Mode::BPX, 4}, {"DEC", &MOS6502::opDEC, Mode::BPX, 6}, {"DCP", &MOS6502::opDCP, Mode::BPX, 6},
    {"CLD", &MOS6502::opCLD, Mode::IMP, 2}, {"CMP", &MOS6502::opCMP, Mode::ABSY, 4}, {"NOP", &MOS6502::opNOP, Mode::IMP, 2}, {"DCP", &MOS6502::opDCP, Mode::ABSY, 7},
    {"NOP", &MOS6502::opNOP, Mode::ABSX, 4}, {"CMP", &MOS6502::opCMP, Mode::ABSX, 4}, {"DEC", &MOS6502::opDEC, Mode::ABSX, 7}, {"DCP", &MOS6502::opDCP, Mode::ABSX, 7},
    {"CPX", &MOS6502::opCPX, Mode::IMM, 2}, {"SBC", &MOS6502::opSBC, Mode::BPXIND, 6}, {"NOP", &MOS6502::opNOP, Mode::IMM, 2}, {"ISC", &MOS6502::opISC, Mode::BPXIND, 8},
    {"CPX", &MOS6502::opCPX, Mode::BP, 3}, {"SBC", &MOS6502::opSBC, Mode::BP, 3}, {"INC", &MOS6502::opINC, Mode::BP, 5}, {"ISC", &MOS6502::opISC, Mode::BP, 5},
    {"INX", &MOS6502::opINX, Mode::IMP, 2}, {"SBC", &MOS6502::opSBC, Mode::IMM, 2}, {"NOP", &MOS6502::opNOP, Mode::IMP, 2}, {"SBC", &MOS6502::opSBC, Mode::IMM, 2},
    {"CPX", &MOS6502::opCPX, Mode::ABS, 4}, {"SBC", &MOS6502::opSBC, Mode::ABS, 4}, {"INC", &MOS6502::opINC, Mode::ABS, 6}, {"ISC", &MOS6502::opISC, Mode::ABS, 6},
    {"BEQ", &MOS6502::opBEQ, Mode::REL, 2}, {"SBC", &MOS6502::opSBC, Mode::INDBPY, 5}, {"KIL", &MOS6502::opKIL, Mode::IMP, 2}, {"ISC", &MOS6502::opISC, Mode::INDBPY, 8},
    {"NOP", &MOS6502::opNOP, Mode::BPX, 4}, {"SBC", &MOS6502::opSBC, Mode::BPX, 4}, {"INC", &MOS6502::opINC, Mode::BPX, 6}, {"ISC", &MOS6502::opISC, Mode::BPX, 6},
    {"SED", &MOS6502::opSED, Mode::IMP, 2}, {"SBC", &MOS6502::opSBC, Mode::ABSY, 4}, {"NOP", &MOS6502::opNOP, Mode::IMP, 2}, {"ISC", &MOS6502::opISC, Mode::ABSY, 7},
    {"NOP", &MOS6502::opNOP, Mode::ABSX, 4}, {"SBC", &MOS6502::opSBC, Mode::ABSX, 4}, {"INC", &MOS6502::opINC, Mode::ABSX, 7}, {"ISC", &MOS6502::opISC, Mode::ABSX, 7}
};

int MOS6502::step() {
    // If CPU is halted (e.g. by ANTIC DMA or WSYNC), it consumes 1 cycle and does nothing.
    if (m_state.haltLine) {
        m_state.cycles++;
        return 1;
    }

    // NMI handling (falling edge)
    if (m_state.nmiLine && !m_state.nmiPrev) {
        log(SIM_LOG_DEBUG, "6502: Taking NMI");
        m_state.nmiPrev = 1;
        push((uint8_t)(m_state.pc >> 8));
        push((uint8_t)(m_state.pc & 0xFF));
        // IRQ/NMI pushes P with B=0 and U=1
        push((m_state.p & ~FLAG_B) | FLAG_U);
        m_state.p |= FLAG_I;
        uint8_t lo = read(0xFFFA);
        uint8_t hi = read(0xFFFB);
        m_state.pc = (uint16_t)lo | ((uint16_t)hi << 8);
        m_state.cycles += 7;
        return 7;
    }
    m_state.nmiPrev = m_state.nmiLine;

    // IRQ handling (level-sensitive)
    if (m_state.irqLine && !(m_state.p & FLAG_I)) {
        log(SIM_LOG_DEBUG, "6502: Taking IRQ");
        push((uint8_t)(m_state.pc >> 8));
        push((uint8_t)(m_state.pc & 0xFF));
        // IRQ/NMI pushes P with B=0 and U=1
        push((m_state.p & ~FLAG_B) | FLAG_U);
        m_state.p |= FLAG_I;
        uint8_t lo = read(0xFFFE);
        uint8_t hi = read(0xFFFF);
        m_state.pc = (uint16_t)lo | ((uint16_t)hi << 8);
        m_state.cycles += 7;
        return 7;
    }

    uint16_t pc = m_state.pc;
    uint8_t op = read(m_state.pc++);
    const Opcode& info = s_opcodes[op];
    
    int extraCycles = 0;
    bool pageCrossed = false;
    uint16_t addr = 0;

    switch (info.mode) {
        case Mode::IMP:    addr = 0; break;
        case Mode::ACC:    addr = 0xFFFF; break;
        case Mode::IMM:    addr = m_state.pc++; break;
        case Mode::BP:     addr = addrBP(); break;
        case Mode::BPX:    addr = addrBPX(); break;
        case Mode::BPY:    addr = addrBPY(); break;
        case Mode::ABS:    addr = addrABS(); break;
        case Mode::ABSX:   addr = addrABSX(pageCrossed); if (pageCrossed) extraCycles++; break;
        case Mode::ABSY:   addr = addrABSY(pageCrossed); if (pageCrossed) extraCycles++; break;
        case Mode::IND:    addr = addrIND(); break;
        case Mode::BPXIND: addr = addrBPXIND(); break;
        case Mode::INDBPY: addr = addrINDBPY(pageCrossed); if (pageCrossed) extraCycles++; break;
        case Mode::REL: {
            int8_t rel = (int8_t)read(m_state.pc++);
            addr = m_state.pc + rel;
            break;
        }
    }

    if (m_observer) {
        if (m_observer->needsDisasm()) {
            uint16_t pcAfterDecode = m_state.pc;
            DisasmEntry entry;
            disassembleEntry(m_bus, pc, &entry);
            m_state.pc = pc;  // expose pre-fetch PC to observer (cpu->pc() == breakpoint addr)
            bool cont = m_observer->onStep(this, m_bus, entry);
            if (!cont) {
                // Breakpoint fired: leave m_state.pc = pc so next step re-executes here.
                return 1;
            }
            m_state.pc = pcAfterDecode;  // restore for execution
        } else {
            m_observer->onStepLite(this, pc);
        }
    }

    if (info.fn) {
        extraCycles += (this->*(info.fn))(addr);
    }

    int total = info.cycles + extraCycles;
    m_state.cycles += total;
    return total;
}
