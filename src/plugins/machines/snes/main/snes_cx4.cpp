// The Cx4 after Mesen2 (Core/SNES/Coprocessors/CX4/Cx4.cpp,
// Cx4.Instructions.cpp), GPL-3.0, Sour and the Mesen contributors.

#include "snes_cx4.h"

#include <cstring>

namespace snes {

namespace {
constexpr int SHIFT[4] = {0, 1, 8, 16};
constexpr double CLOCK_RATIO = 20000000.0 / 21477272.0;
int32_t sx24(uint32_t v) { return (int32_t)(v << 8) >> 8; }
}  // namespace

Cx4::Cx4(const std::vector<uint8_t>* rom, std::vector<uint8_t>* sram) : m_rom(rom), m_sram(sram) { reset(); }

void Cx4::reset() {
    std::memset(m_dataRam, 0, sizeof m_dataRam);
    std::memset(m_prgRam, 0, sizeof m_prgRam);
    m_cycles = 0;
    m_pb = m_p = 0; m_pc = m_sp = 0;
    m_a = 0; std::memset(m_stack, 0, sizeof m_stack);
    m_mult = 0; m_romBuffer = 0; std::memset(m_ramBuffer, 0, sizeof m_ramBuffer);
    m_mdr = m_mar = m_dpr = 0; std::memset(m_regs, 0, sizeof m_regs);
    m_negative = m_zero = m_carry = m_overflow = m_irqFlag = m_irqLine = false;
    m_stopped = true; m_locked = false; m_irqDisabled = false; m_singleRom = true;
    m_romDelay = m_ramDelay = 3;
    m_busEnabled = m_busReading = m_busWriting = false; m_busDelay = 0; m_busAddr = 0;
    m_dmaSrc = m_dmaDest = m_dmaPos = 0; m_dmaLen = 0; m_dmaEnabled = false;
    m_cacheEnabled = false; m_cacheLock[0] = m_cacheLock[1] = false;
    m_cachePage = m_cachePc = 0; m_cacheAddr[0] = m_cacheAddr[1] = 0; m_cacheBase = 0;
    m_cacheBank = m_cachePos = 0;
    m_suspend = false; m_suspendLeft = 0;
    std::memset(m_vectors, 0, sizeof m_vectors);
}

// ---------------------------------------------------------------------------
// The Cx4's bus: ROM (LoROM), battery RAM, its own registers
// ---------------------------------------------------------------------------

Cx4::Kind Cx4::kind(uint32_t addr) const {
    const uint8_t bank = (uint8_t)(addr >> 16), b = bank & 0x7F;
    const uint16_t a = (uint16_t)addr;
    if (a >= 0x8000) return ROM;
    if (b >= 0x70 && b <= 0x7D) return m_sram->empty() ? NONE : SRAM;
    if (!(bank & 0x40) && a >= 0x6000) return REGS;
    return NONE;
}

uint8_t Cx4::busRead(uint32_t addr) {
    const uint8_t b = (uint8_t)((addr >> 16) & 0x7F);
    const uint16_t a = (uint16_t)addr;
    switch (kind(addr)) {
    case ROM: return m_rom->empty() ? 0 : (*m_rom)[((uint32_t)b * 0x8000 + (a & 0x7FFF)) % m_rom->size()];
    case SRAM: return (*m_sram)[((uint32_t)(b - 0x70) * 0x8000 + a) % m_sram->size()];
    case REGS: return read(a);
    default: return 0;
    }
}

void Cx4::busWrite(uint32_t addr, uint8_t v) {
    const uint8_t b = (uint8_t)((addr >> 16) & 0x7F);
    const uint16_t a = (uint16_t)addr;
    switch (kind(addr)) {
    case SRAM: (*m_sram)[((uint32_t)(b - 0x70) * 0x8000 + a) % m_sram->size()] = v; break;
    case REGS: write(a, v); break;
    default: break;
    }
}

uint8_t Cx4::accessDelay(uint32_t addr) const {
    switch (kind(addr)) {
    case ROM: return (uint8_t)(1 + m_romDelay);
    case SRAM: return (uint8_t)(1 + m_ramDelay);
    default: return 1;
    }
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

void Cx4::runTo(uint64_t master) {
    const uint64_t target = (uint64_t)((double)master * CLOCK_RATIO);
    while (m_cycles < target) {
        if (m_locked) step(1);
        else if (m_suspend) {
            step(1);
            if (m_suspendLeft && --m_suspendLeft == 0) m_suspend = false;
        } else if (m_cacheEnabled) processCache(target);
        else if (m_dmaEnabled) processDma(target);
        else if (m_stopped) step(target - m_cycles);
        else if (!processCache(target)) {
            if (!m_cacheEnabled) stop();             // both cache pages locked and neither holds the code
        } else {
            const uint16_t op = m_prgRam[m_cachePage][m_pc];
            ++m_pc;
            // The end of the page: the next page loads before the instruction runs
            if (m_pc == 0) switchCachePage();
            exec(op);
        }
    }
}

void Cx4::step(uint64_t cycles) {
    if (m_busEnabled) {
        if (m_busDelay > cycles) m_busDelay = (uint8_t)(m_busDelay - cycles);
        else {
            m_busEnabled = false;
            m_busDelay = 0;
            if (m_busReading) { m_mdr = busRead(m_busAddr); m_busReading = false; }
            if (m_busWriting) { busWrite(m_busAddr, (uint8_t)m_mdr); m_busWriting = false; }
        }
    }
    m_cycles += cycles;
}

void Cx4::switchCachePage() {
    if (m_cachePage == 1) { stop(); return; }
    m_cachePage = 1;
    if (m_cacheLock[1]) { stop(); return; }
    m_pb = m_p;
    if (!processCache(m_cycles) && !m_cacheEnabled) stop();
}

bool Cx4::processCache(uint64_t target) {
    const uint32_t address = (m_cacheBase + ((uint32_t)m_pb << 9)) & 0xFFFFFF;
    if (m_cachePos == 0) {
        if (m_cacheAddr[m_cachePage] == address) { m_cacheEnabled = false; return true; }
        m_cachePage ^= 1;
        if (m_cacheAddr[m_cachePage] == address) { m_cacheEnabled = false; return true; }
        if (m_cacheLock[m_cachePage]) m_cachePage ^= 1;
        if (m_cacheLock[m_cachePage]) { m_cacheEnabled = false; return false; }
        m_cacheEnabled = true;
    }
    while (m_cachePos < 256) {
        const uint32_t a = address + m_cachePos * 2u;
        const uint8_t lo = busRead(a);
        step(accessDelay(a));
        const uint8_t hi = busRead(a + 1);
        step(accessDelay(a + 1));
        m_prgRam[m_cachePage][m_cachePos] = (uint16_t)(hi << 8 | lo);
        ++m_cachePos;
        if (m_cycles > target) break;
    }
    if (m_cachePos >= 256) {
        m_cacheAddr[m_cachePage] = address;
        m_cachePos = 0;
        m_cacheEnabled = false;
        return true;
    }
    return false;
}

void Cx4::processDma(uint64_t target) {
    while (m_dmaPos < m_dmaLen) {
        const uint32_t src = (m_dmaSrc + m_dmaPos) & 0xFFFFFF, dest = (m_dmaDest + m_dmaPos) & 0xFFFFFF;
        const Kind ks = kind(src), kd = kind(dest);
        if (ks == NONE || kd == NONE || ks == kd || kd == ROM) {
            // An impossible DMA locks the chip until $7F53 is written
            m_locked = true;
            m_dmaPos = 0;
            m_dmaEnabled = false;
            return;
        }
        step(accessDelay(src));
        const uint8_t v = busRead(src);
        step(accessDelay(dest));
        busWrite(dest, v);
        ++m_dmaPos;
        if (m_cycles > target) break;
    }
    if (m_dmaPos >= m_dmaLen) { m_dmaPos = 0; m_dmaEnabled = false; }
}

void Cx4::stop() {
    m_stopped = true;
    if (!m_irqDisabled) { m_irqFlag = true; m_irqLine = true; }
}

// ---------------------------------------------------------------------------
// The SNES side
// ---------------------------------------------------------------------------

uint8_t Cx4::read(uint16_t addr) const {
    addr = (uint16_t)(0x7000 | (addr & 0xFFF));
    if (addr <= 0x7BFF) return m_dataRam[addr & 0xFFF];
    if (addr >= 0x7F60 && addr <= 0x7F7F) return m_vectors[addr & 0x1F];
    if ((addr >= 0x7F80 && addr <= 0x7FAF) || (addr >= 0x7FC0 && addr <= 0x7FEF)) {
        const int r = (addr & 0x3F);
        return (uint8_t)(m_regs[r / 3] >> ((r % 3) * 8));
    }
    if (addr >= 0x7F53 && addr <= 0x7F5F)
        return (uint8_t)(m_suspend | m_irqFlag << 1 | running() << 6 | busy() << 7);
    switch (addr) {
    case 0x7F40: return (uint8_t)m_dmaSrc;
    case 0x7F41: return (uint8_t)(m_dmaSrc >> 8);
    case 0x7F42: return (uint8_t)(m_dmaSrc >> 16);
    case 0x7F43: return (uint8_t)m_dmaLen;
    case 0x7F44: return (uint8_t)(m_dmaLen >> 8);
    case 0x7F45: return (uint8_t)m_dmaDest;
    case 0x7F46: return (uint8_t)(m_dmaDest >> 8);
    case 0x7F47: return (uint8_t)(m_dmaDest >> 16);
    case 0x7F48: return m_cachePage;
    case 0x7F49: return (uint8_t)m_cacheBase;
    case 0x7F4A: return (uint8_t)(m_cacheBase >> 8);
    case 0x7F4B: return (uint8_t)(m_cacheBase >> 16);
    case 0x7F4C: return (uint8_t)(m_cacheLock[0] | m_cacheLock[1] << 1);
    case 0x7F4D: return (uint8_t)m_cacheBank;
    case 0x7F4E: return (uint8_t)(m_cacheBank >> 8);
    case 0x7F4F: return m_cachePc;
    case 0x7F50: return (uint8_t)(m_ramDelay | m_romDelay << 4);
    case 0x7F51: return m_irqDisabled;
    case 0x7F52: return m_singleRom;
    default: return 0;
    }
}

void Cx4::write(uint16_t addr, uint8_t v) {
    addr = (uint16_t)(0x7000 | (addr & 0xFFF));
    if (addr <= 0x7BFF) { m_dataRam[addr & 0xFFF] = v; return; }
    if (addr >= 0x7F60 && addr <= 0x7F7F) { m_vectors[addr & 0x1F] = v; return; }
    if ((addr >= 0x7F80 && addr <= 0x7FAF) || (addr >= 0x7FC0 && addr <= 0x7FEF)) {
        const int r = addr & 0x3F, shift = (r % 3) * 8;
        m_regs[r / 3] = (m_regs[r / 3] & ~(0xFFu << shift)) | (uint32_t)v << shift;
        return;
    }
    if (addr >= 0x7F55 && addr <= 0x7F5C) { m_suspend = true; m_suspendLeft = (uint32_t)(addr - 0x7F55) * 32; return; }
    switch (addr) {
    case 0x7F40: m_dmaSrc = (m_dmaSrc & 0xFFFF00) | v; break;
    case 0x7F41: m_dmaSrc = (m_dmaSrc & 0xFF00FF) | (uint32_t)v << 8; break;
    case 0x7F42: m_dmaSrc = (m_dmaSrc & 0x00FFFF) | (uint32_t)v << 16; break;
    case 0x7F43: m_dmaLen = (uint16_t)((m_dmaLen & 0xFF00) | v); break;
    case 0x7F44: m_dmaLen = (uint16_t)((m_dmaLen & 0x00FF) | v << 8); break;
    case 0x7F45: m_dmaDest = (m_dmaDest & 0xFFFF00) | v; break;
    case 0x7F46: m_dmaDest = (m_dmaDest & 0xFF00FF) | (uint32_t)v << 8; break;
    case 0x7F47:                                     // the DMA starts
        m_dmaDest = (m_dmaDest & 0x00FFFF) | (uint32_t)v << 16;
        if (m_stopped) m_dmaEnabled = true;
        break;
    case 0x7F48:                                     // load a cache page
        m_cachePage = v & 1;
        if (m_stopped) m_cacheEnabled = true;
        break;
    case 0x7F49: m_cacheBase = (m_cacheBase & 0xFFFF00) | v; break;
    case 0x7F4A: m_cacheBase = (m_cacheBase & 0xFF00FF) | (uint32_t)v << 8; break;
    case 0x7F4B: m_cacheBase = (m_cacheBase & 0x00FFFF) | (uint32_t)v << 16; break;
    case 0x7F4C: m_cacheLock[0] = v & 1; m_cacheLock[1] = v & 2; break;
    case 0x7F4D: m_cacheBank = (uint16_t)((m_cacheBank & 0xFF00) | v); break;
    case 0x7F4E: m_cacheBank = (uint16_t)((m_cacheBank & 0x00FF) | (v & 0x7F) << 8); break;
    case 0x7F4F:                                     // the program counter: run
        m_cachePc = v;
        if (m_stopped) { m_stopped = false; m_pb = m_cacheBank; m_pc = m_cachePc; }
        break;
    case 0x7F50: m_ramDelay = v & 7; m_romDelay = (v >> 4) & 7; break;
    case 0x7F51:
        m_irqDisabled = v & 1;
        if (m_irqDisabled) { m_irqFlag = true; m_irqLine = false; }
        break;
    case 0x7F52: m_singleRom = v & 1; break;
    case 0x7F53: m_locked = false; m_stopped = true; break;
    case 0x7F5D: m_suspend = false; break;
    case 0x7F5E: m_irqFlag = false; break;           // the flag only: the IRQ line stays
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Instructions
// ---------------------------------------------------------------------------

uint32_t Cx4::source(uint8_t src) {
    src &= 0x7F;
    if (src >= 0x60) return m_regs[src & 0x0F];
    switch (src) {
    case 0x00: return m_a;
    case 0x01: return (uint32_t)((m_mult >> 24) & 0xFFFFFF);
    case 0x02: return (uint32_t)(m_mult & 0xFFFFFF);
    case 0x03: return m_mdr;
    case 0x08: return m_romBuffer;
    case 0x0C: return (uint32_t)(m_ramBuffer[2] << 16 | m_ramBuffer[1] << 8 | m_ramBuffer[0]);
    case 0x13: return m_mar;
    case 0x1C: return m_dpr;
    case 0x20: return m_pc;
    case 0x28: return m_p;
    case 0x2E: case 0x2F:                            // start a bus read (ROM or RAM timing)
        m_busEnabled = true; m_busReading = true;
        m_busDelay = (uint8_t)(1 + (src == 0x2E ? m_romDelay : m_ramDelay));
        m_busAddr = m_mar;
        return 0;
    case 0x50: return 0x000000; case 0x51: return 0xFFFFFF; case 0x52: return 0x00FF00; case 0x53: return 0xFF0000;
    case 0x54: return 0x00FFFF; case 0x55: return 0xFFFF00; case 0x56: return 0x800000; case 0x57: return 0x7FFFFF;
    case 0x58: return 0x008000; case 0x59: return 0x007FFF; case 0x5A: return 0xFF7FFF; case 0x5B: return 0xFFFF7F;
    case 0x5C: return 0x010000; case 0x5D: return 0xFEFFFF; case 0x5E: return 0x000100; case 0x5F: return 0x00FEFF;
    default: return 0;
    }
}

void Cx4::writeRegister(uint8_t reg, uint32_t v) {
    v &= 0xFFFFFF;
    reg &= 0x7F;
    if (reg >= 0x60) { m_regs[reg & 0x0F] = v; return; }
    switch (reg) {
    case 0x01: m_mult = (m_mult & 0xFFFFFF) | (uint64_t)v << 24; break;
    case 0x02: m_mult = (m_mult & 0xFFFFFF000000ull) | v; break;
    case 0x03: m_mdr = v; break;
    case 0x08: m_romBuffer = v; break;
    case 0x0C: m_ramBuffer[0] = (uint8_t)v; m_ramBuffer[1] = (uint8_t)(v >> 8); m_ramBuffer[2] = (uint8_t)(v >> 16); break;
    case 0x13: m_mar = v; break;
    case 0x1C: m_dpr = v; break;
    case 0x20: m_pc = (uint8_t)v; break;
    case 0x28: m_p = (uint16_t)(v & 0x7FFF); break;
    case 0x2E: case 0x2F:                            // start a bus write
        m_busEnabled = true; m_busWriting = true;
        m_busDelay = (uint8_t)(1 + (reg == 0x2E ? m_romDelay : m_ramDelay));
        m_busAddr = m_mar;
        break;
    default: break;
    }
}

uint32_t Cx4::add(uint32_t a, uint32_t b) {
    const uint32_t r = a + b;
    m_carry = r > 0xFFFFFF;
    m_negative = r & 0x800000;
    m_overflow = ~(a ^ b) & (a ^ r) & 0x800000;
    m_zero = (r & 0xFFFFFF) == 0;
    return r & 0xFFFFFF;
}

uint32_t Cx4::sub(uint32_t a, uint32_t b) {
    const int32_t r = (int32_t)(a - b);
    m_carry = r >= 0;
    m_negative = r & 0x800000;
    m_overflow = ~(a ^ b) & (a ^ (uint32_t)r) & 0x800000;
    m_zero = r == 0;
    return (uint32_t)r & 0xFFFFFF;
}

void Cx4::branch(bool taken, uint8_t far, uint8_t dest) {
    if (!taken) return;
    if (far) m_pb = m_p;
    m_pc = dest;
    step(2);
}

void Cx4::jsr(bool taken, uint8_t far, uint8_t dest) {
    if (!taken) return;
    m_stack[m_sp] = (uint32_t)m_pb << 8 | m_pc;
    m_sp = (m_sp + 1) & 7;
    if (far) m_pb = m_p;
    m_pc = dest;
    step(2);
}

void Cx4::skip(uint8_t flag, uint8_t ifSet) {
    bool f;
    switch (flag) {
    case 1: f = m_carry; break;
    case 2: f = m_zero; break;
    case 3: f = m_negative; break;
    default: f = m_overflow; break;
    }
    if (f == (bool)(ifSet & 1)) {
        ++m_pc;
        if (m_pc == 0) switchCachePage();
        step(1);
    }
}

void Cx4::exec(uint16_t opcode) {
    const uint8_t op = (opcode >> 8) & 0xFC, p1 = (opcode >> 8) & 3, p2 = (uint8_t)opcode;
    const int sh = SHIFT[p1];
    switch (op) {
    case 0x08: branch(true, p1, p2); break;
    case 0x0C: branch(m_zero, p1, p2); break;
    case 0x10: branch(m_carry, p1, p2); break;
    case 0x14: branch(m_negative, p1, p2); break;
    case 0x18: branch(m_overflow, p1, p2); break;
    case 0x1C: if (m_busEnabled) step(m_busDelay); break;                  // WAIT
    case 0x24: skip(p1, p2); break;
    case 0x28: jsr(true, p1, p2); break;
    case 0x2C: jsr(m_zero, p1, p2); break;
    case 0x30: jsr(m_carry, p1, p2); break;
    case 0x34: jsr(m_negative, p1, p2); break;
    case 0x38: jsr(m_overflow, p1, p2); break;
    case 0x3C: {                                                            // RTS
        m_sp = (m_sp - 1) & 7;
        m_pb = (uint16_t)((m_stack[m_sp] >> 8) & 0x7FFF);
        m_pc = (uint8_t)m_stack[m_sp];
        step(2);
        break;
    }
    case 0x40: m_mar = (m_mar + 1) & 0xFFFFFF; break;                       // INC MAR
    case 0x48: sub(source(p2), m_a << sh); break;                           // CMPR
    case 0x4C: sub(p2, m_a << sh); break;
    case 0x50: sub(m_a << sh, source(p2)); break;                           // CMP
    case 0x54: sub(m_a << sh, p2); break;
    case 0x58:                                                              // sign extend
        if (p1 == 1) m_a = (uint32_t)(int8_t)m_a & 0xFFFFFF;
        else if (p1 == 2) m_a = (uint32_t)(int16_t)m_a & 0xFFFFFF;
        if (p1 == 1 || p1 == 2) { m_negative = m_a & 0x800000; m_zero = m_a == 0; }
        break;
    case 0x60:                                                              // LD
        switch (p1) {
        case 0: m_a = source(p2); break;
        case 1: m_mdr = source(p2); break;
        case 2: m_mar = source(p2); break;
        default: m_p = (uint16_t)(source(p2) & 0x7FFF); break;
        }
        break;
    case 0x64:
        switch (p1) {
        case 0: m_a = p2; break;
        case 1: m_mdr = p2; break;
        case 2: m_mar = p2; break;
        default: m_p = p2; break;
        }
        break;
    case 0x68: if (p1 < 3) m_ramBuffer[p1] = m_dataRam[ramAddr(m_a)]; break;                  // RDRAM
    case 0x6C: if (p1 < 3) m_ramBuffer[p1] = m_dataRam[ramAddr(m_dpr + p2)]; break;
    case 0x70: m_romBuffer = DATA_ROM[m_a & 0x3FF]; break;                                  // RDROM
    case 0x74: m_romBuffer = DATA_ROM[(p1 << 8 | p2) & 0x3FF]; break;
    case 0x7C:                                                              // LD P (a byte)
        if (p1 == 0) m_p = (uint16_t)((m_p & 0x7F00) | p2);
        else if (p1 == 1) m_p = (uint16_t)((m_p & 0x00FF) | (p2 & 0x7F) << 8);
        break;
    case 0x80: m_a = add(m_a << sh, source(p2)); break;
    case 0x84: m_a = add(m_a << sh, p2); break;
    case 0x88: m_a = sub(source(p2), m_a << sh); break;                     // SUBR
    case 0x8C: m_a = sub(p2, m_a << sh); break;
    case 0x90: m_a = sub(m_a << sh, source(p2)); break;
    case 0x94: m_a = sub(m_a << sh, p2); break;
    case 0x98: m_mult = (uint64_t)((int64_t)sx24(source(p2)) * sx24(m_a)) & 0xFFFFFFFFFFFFull; break;   // SMUL
    case 0x9C: m_mult = (uint64_t)((int64_t)p2 * sx24(m_a)) & 0xFFFFFFFFFFFFull; break;
    case 0xA0: setA(~(m_a << sh) ^ source(p2)); setZN(); break;            // XNOR
    case 0xA4: setA(~(m_a << sh) ^ p2); setZN(); break;
    case 0xA8: setA((m_a << sh) ^ source(p2)); setZN(); break;
    case 0xAC: setA((m_a << sh) ^ p2); setZN(); break;
    case 0xB0: setA((m_a << sh) & source(p2)); setZN(); break;
    case 0xB4: setA((m_a << sh) & p2); setZN(); break;
    case 0xB8: setA((m_a << sh) | source(p2)); setZN(); break;
    case 0xBC: setA((m_a << sh) | p2); setZN(); break;
    case 0xC0: case 0xC4: case 0xC8: case 0xCC: case 0xD0: case 0xD4: case 0xD8: case 0xDC: {   // shifts
        const uint8_t n = (uint8_t)((op & 4) ? (p2 & 0x1F) : (source(p2) & 0x1F));
        if (n < 24) {
            switch (op & 0xF8) {
            case 0xC0: setA(m_a >> n); break;
            case 0xC8: setA((uint32_t)(sx24(m_a) >> n)); break;
            case 0xD0: setA((m_a >> n) | (m_a << (24 - n))); break;
            default: setA(m_a << n); break;
            }
        }
        setZN();
        break;
    }
    case 0xE0:                                                              // ST
        if (p1 == 0) writeRegister(p2, m_a);
        else if (p1 == 1) writeRegister(p2, m_mdr);
        break;
    case 0xE8: if (p1 < 3) m_dataRam[ramAddr(m_a)] = m_ramBuffer[p1]; break;                 // WRRAM
    case 0xEC: if (p1 < 3) m_dataRam[ramAddr(m_dpr + p2)] = m_ramBuffer[p1]; break;
    case 0xF0: { const uint32_t t = m_a; m_a = m_regs[p2 & 0x0F]; m_regs[p2 & 0x0F] = t; break; }   // SWAP
    case 0xFC: stop(); break;
    default: break;                                                         // NOP and the unknown ones
    }
    step(1);
}

}  // namespace snes
