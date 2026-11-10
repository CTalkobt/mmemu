// The Super FX (GSU) after Mesen2 (Core/SNES/Coprocessors/GSU/Gsu.cpp,
// Gsu.Instructions.cpp), GPL-3.0, Sour and the Mesen contributors.

#include "snes_gsu.h"

#include <algorithm>
#include <cstring>

namespace snes {

Gsu::Gsu(const std::vector<uint8_t>* rom, uint32_t ramSize) : m_rom(rom) {
    m_ram.assign(ramSize ? ramSize : 0x10000, 0);
    reset();
}

void Gsu::reset() {
    m_cycles = 0;
    std::memset(m_r, 0, sizeof m_r);
    m_sfr = Flags{};
    m_latch = 0;
    m_pbr = m_romBank = m_ramBank = 0;
    m_irqDisabled = m_highSpeed = m_clockSelect = m_backupRam = false;
    m_screenBase = 0; m_plotBpp = 2; m_screenHeight = 0;
    m_ramAccess = m_romAccess = false;
    m_cacheBase = 0;
    m_plotTransparent = m_plotDither = m_colorHigh = m_colorFreeze = m_objMode = false;
    m_colorReg = m_srcReg = m_destReg = 0;
    m_romBuffer = m_romDelay = 0;
    m_programBuffer = 0x01;                          // a NOP first
    m_ramWriteAddr = 0; m_ramWriteValue = 0; m_ramDelay = 0; m_ramAddr = 0;
    m_primary = m_secondary = PixelCache{};
    std::memset(m_cache, 0, sizeof m_cache);
    std::memset(m_cacheValid, 0, sizeof m_cacheValid);
    m_waitRomAccess = m_waitRamAccess = false;
    m_stopped = true;
    m_r15Changed = false;
}

void Gsu::runTo(uint64_t master) {
    while (!m_stopped && m_cycles < master) exec();
    if (master > m_cycles) step(master - m_cycles);
}

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------

uint8_t Gsu::readGsu(uint32_t addr) {
    const uint8_t bank = (uint8_t)((addr >> 16) & 0x7F);
    const uint16_t a = (uint16_t)addr;
    const auto& rom = *m_rom;
    if (bank <= 0x3F) return rom.empty() ? 0 : rom[((uint32_t)bank * 0x8000 + (a & 0x7FFF)) % rom.size()];
    if (bank <= 0x5F) return rom.empty() ? 0 : rom[((uint32_t)(bank - 0x40) << 16 | a) % rom.size()];
    if (bank == 0x70 || bank == 0x71) return m_ram[((uint32_t)(bank & 1) << 16 | a) % m_ram.size()];
    return 0;
}

void Gsu::writeGsu(uint32_t addr, uint8_t v) {
    const uint8_t bank = (uint8_t)((addr >> 16) & 0x7F);
    if (bank == 0x70 || bank == 0x71) m_ram[((uint32_t)(bank & 1) << 16 | (uint16_t)addr) % m_ram.size()] = v;
}

uint8_t Gsu::cpuReadRom(uint32_t offset, uint16_t addr) const {
    if (!m_sfr.g || !m_romAccess) return m_rom->empty() ? 0 : (*m_rom)[offset % m_rom->size()];
    // The GSU has the ROM: the SNES sees its interrupt vectors pointing at RAM
    if (addr & 1) return 0x01;
    switch (addr & 0x0E) {
    case 0x04: return 0x04;
    case 0x0A: return 0x08;
    case 0x0E: return 0x0C;
    default: return 0x00;
    }
}

uint8_t Gsu::cpuReadRam(uint32_t offset) const {
    if (m_sfr.g && m_ramAccess) return 0;
    return m_ram[offset % m_ram.size()];
}

void Gsu::cpuWriteRam(uint32_t offset, uint8_t v) {
    if (m_sfr.g && m_ramAccess) return;
    m_ram[offset % m_ram.size()] = v;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

void Gsu::exec() {
    const uint8_t op = readOpcode();
    const uint8_t n = op & 0x0F;
    switch (op >> 4) {
    case 0x0:
        switch (op) {
        case 0x00: stop(); break;
        case 0x01: resetFlags(); break;              // NOP
        case 0x02: cache(); break;
        case 0x03: lsr(); break;
        case 0x04: rol(); break;
        case 0x05: branch(true); break;
        case 0x06: branch(m_sfr.s == m_sfr.ov); break;   // BGE
        case 0x07: branch(m_sfr.s != m_sfr.ov); break;   // BLT
        case 0x08: branch(!m_sfr.z); break;
        case 0x09: branch(m_sfr.z); break;
        case 0x0A: branch(!m_sfr.s); break;
        case 0x0B: branch(m_sfr.s); break;
        case 0x0C: branch(!m_sfr.cy); break;
        case 0x0D: branch(m_sfr.cy); break;
        case 0x0E: branch(!m_sfr.ov); break;
        default: branch(m_sfr.ov); break;
        }
        break;
    case 0x1: to(n); break;
    case 0x2: with(n); break;
    case 0x3:
        if (n < 0xC) store(n);
        else if (n == 0xC) loop();
        else if (n == 0xD) { m_sfr.b = false; m_sfr.alt1 = true; }
        else if (n == 0xE) { m_sfr.b = false; m_sfr.alt2 = true; }
        else { m_sfr.b = false; m_sfr.alt1 = m_sfr.alt2 = true; }
        break;
    case 0x4:
        if (n < 0xC) load(n);
        else if (n == 0xC) plotRpix();
        else if (n == 0xD) swap();
        else if (n == 0xE) colorCmode();
        else notOp();
        break;
    case 0x5: add(n); break;
    case 0x6: subCmp(n); break;
    case 0x7: if (n == 0) merge(); else andBic(n); break;
    case 0x8: mult(n); break;
    case 0x9:
        if (n == 0) sbk();
        else if (n <= 4) link(n);
        else if (n == 5) sex();
        else if (n == 6) asr();
        else if (n == 7) ror();
        else if (n <= 0xD) jmp(n);
        else if (n == 0xE) lob();
        else fmultLmult();
        break;
    case 0xA: ibtSmsLms(n); break;
    case 0xB: from(n); break;
    case 0xC: if (n == 0) hib(); else orXor(n); break;
    case 0xD: if (n == 0xF) getcRambRomb(); else inc(n); break;
    case 0xE: if (n == 0xF) getb(); else dec(n); break;
    default: iwtLmSm(n); break;
    }
    if (!m_r15Changed) ++m_r[15];
    else m_r15Changed = false;
}

void Gsu::initCache(uint16_t cacheAddr) {
    const uint16_t dest = cacheAddr & 0x01F0;
    if (m_pbr <= 0x5F) { waitRom(); waitForRomAccess(); }
    else { waitRam(); waitForRamAccess(); }
    const uint32_t base = ((uint32_t)m_pbr << 16) + m_cacheBase + dest;
    for (int i = 0; i < 16; ++i) m_cache[dest + i] = readGsu(base + i);
    step(clk(5, 6) * 16);
    m_cacheValid[cacheAddr >> 4] = true;
}

uint8_t Gsu::readOperand() {
    const uint8_t r = m_programBuffer;
    ++m_r[15];
    m_programBuffer = readProgramByte();
    return r;
}

uint8_t Gsu::readOpcode() {
    const uint8_t r = m_programBuffer;
    m_programBuffer = readProgramByte();
    return r;
}

uint8_t Gsu::readProgramByte() {
    const uint16_t cacheAddr = (uint16_t)(m_r[15] - m_cacheBase);
    if (cacheAddr < 512) {
        if (!m_cacheValid[cacheAddr >> 4]) initCache(cacheAddr & 0xFFF0);
        step(clk(1, 2));
        return m_cache[cacheAddr];
    }
    if (m_pbr <= 0x5F) { waitRom(); waitForRomAccess(); }
    else { waitRam(); waitForRamAccess(); }
    step(clk(5, 6));
    return readGsu((uint32_t)m_pbr << 16 | m_r[15]);
}

void Gsu::writeReg16(uint8_t r, uint16_t v) {
    m_r[r] = v;
    if (r == 14) { m_sfr.r = true; m_romDelay = clk(5, 6); }
    else if (r == 15) m_r15Changed = true;
}

void Gsu::resetFlags() {
    m_sfr.b = m_sfr.alt1 = m_sfr.alt2 = false;
    m_srcReg = m_destReg = 0;
}

void Gsu::invalidateCache() { std::memset(m_cacheValid, 0, sizeof m_cacheValid); }

void Gsu::waitRom() { if (m_romDelay) step(m_romDelay); }
void Gsu::waitRam() { if (m_ramDelay) step(m_ramDelay); }
void Gsu::waitForRomAccess() { if (!m_romAccess) { m_waitRomAccess = true; m_stopped = true; } }
void Gsu::waitForRamAccess() { if (!m_ramAccess) { m_waitRamAccess = true; m_stopped = true; } }
void Gsu::updateRunning() { m_stopped = !m_sfr.g || m_waitRamAccess || m_waitRomAccess; }

uint8_t Gsu::readRomBuffer() { waitRom(); return m_romBuffer; }

uint8_t Gsu::readRamBuffer(uint16_t addr) {
    waitRam();
    waitForRamAccess();
    return readGsu(0x700000u | (uint32_t)m_ramBank << 16 | addr);
}

void Gsu::writeRam(uint16_t addr, uint8_t v) {
    waitRam();
    m_ramDelay = clk(5, 6);
    m_ramWriteAddr = addr;
    m_ramWriteValue = v;
}

void Gsu::step(uint64_t cycles) {
    m_cycles += cycles;
    if (m_romDelay) {
        m_romDelay = (uint8_t)(m_romDelay - std::min<uint64_t>(cycles, m_romDelay));
        if (!m_romDelay) {
            waitForRomAccess();
            m_romBuffer = readGsu((uint32_t)m_romBank << 16 | m_r[14]);
            m_sfr.r = false;
        }
    }
    if (m_ramDelay) {
        m_ramDelay = (uint8_t)(m_ramDelay - std::min<uint64_t>(cycles, m_ramDelay));
        if (!m_ramDelay) {
            waitForRamAccess();
            writeGsu(0x700000u | (uint32_t)m_ramBank << 16 | m_ramWriteAddr, m_ramWriteValue);
        }
    }
}

// ---------------------------------------------------------------------------
// Registers ($3000-$3FFF)
// ---------------------------------------------------------------------------

uint8_t Gsu::peekReg(uint16_t addr) const {
    addr &= 0x33FF;
    if (m_sfr.g && addr != 0x3030 && addr != 0x3031 && addr != 0x303B) return 0;   // only SFR and VCR while running
    if (addr < 0x3020) return (addr & 1) ? (uint8_t)(m_r[(addr >> 1) & 15] >> 8) : (uint8_t)m_r[(addr >> 1) & 15];
    switch (addr) {
    case 0x3030: return m_sfr.low();
    case 0x3031: return m_sfr.high();
    case 0x3034: return m_pbr;
    case 0x3036: return m_romBank;
    case 0x303B: return 0x04;                        // VCR: the version
    case 0x303C: return m_ramBank;
    case 0x303E: return (uint8_t)m_cacheBase;
    case 0x303F: return (uint8_t)(m_cacheBase >> 8);
    default: break;
    }
    if (addr >= 0x3100 && addr <= 0x32FF) return m_cache[(m_cacheBase + (addr - 0x3100)) & 0x1FF];
    return 0;
}

uint8_t Gsu::readReg(uint16_t addr) {
    const uint8_t v = peekReg(addr);
    if ((addr & 0x33FF) == 0x3031) m_sfr.irq = false;     // reading SFR's high byte acknowledges the IRQ
    return v;
}

void Gsu::writeReg(uint16_t addr, uint8_t v) {
    addr &= 0x33FF;
    if (m_sfr.g && addr != 0x3030 && addr != 0x303A) return;
    if (addr < 0x3020) {
        if (!(addr & 1)) { m_latch = v; return; }
        const uint8_t r = (addr >> 1) & 15;
        m_r[r] = (uint16_t)(v << 8 | m_latch);
        if (r == 14) { m_sfr.r = true; m_romDelay = clk(5, 6); }
        else if (r == 15) { m_sfr.g = true; updateRunning(); }   // writing R15 starts the GSU
        return;
    }
    switch (addr) {
    case 0x3030: {
        const bool was = m_sfr.g;
        m_sfr.z = v & 0x02; m_sfr.cy = v & 0x04; m_sfr.s = v & 0x08; m_sfr.ov = v & 0x10; m_sfr.g = v & 0x20;
        if (was && !m_sfr.g) { m_cacheBase = 0; invalidateCache(); }
        updateRunning();
        break;
    }
    case 0x3033: m_backupRam = v & 1; break;
    case 0x3034: m_pbr = v & 0x7F; invalidateCache(); break;
    case 0x3037: m_highSpeed = v & 0x20; m_irqDisabled = v & 0x80; break;
    case 0x3038: m_screenBase = v; break;
    case 0x3039: m_clockSelect = v & 1; break;
    case 0x303A: {
        static const uint8_t BPP[4] = {2, 4, 4, 8};
        m_plotBpp = BPP[v & 3];
        m_screenHeight = (uint8_t)(((v & 0x04) >> 2) | ((v & 0x20) >> 4));
        m_ramAccess = v & 0x08;
        m_romAccess = v & 0x10;
        if (m_ramAccess) m_waitRamAccess = false;
        if (m_romAccess) m_waitRomAccess = false;
        updateRunning();
        break;
    }
    default: break;
    }
    if (addr >= 0x3100 && addr <= 0x32FF) {
        const uint16_t c = (uint16_t)((m_cacheBase + (addr - 0x3100)) & 0x1FF);
        m_cache[c] = v;
        if ((c & 0x0F) == 0x0F) m_cacheValid[c >> 4] = true;
    }
}

// ---------------------------------------------------------------------------
// Instructions
// ---------------------------------------------------------------------------

void Gsu::stop() {
    if (!m_irqDisabled) m_sfr.irq = true;
    m_programBuffer = 0x01;                          // a NOP first when restarted
    m_sfr.g = false;
    resetFlags();
    updateRunning();
}

void Gsu::cache() {
    if (m_cacheBase != (m_r[15] & 0xFFF0)) { m_cacheBase = m_r[15] & 0xFFF0; invalidateCache(); }
    resetFlags();
}

void Gsu::branch(bool taken) {
    const int8_t off = (int8_t)readOperand();
    if (taken) writeReg16(15, (uint16_t)(m_r[15] + off));
}

void Gsu::jmp(uint8_t r) {
    if (m_sfr.alt1) {                                // LJMP
        m_pbr = m_r[r] & 0x7F;
        writeReg16(15, src());
        m_cacheBase = m_r[15] & 0xFFF0;
        invalidateCache();
    } else {
        writeReg16(15, m_r[r]);
    }
    resetFlags();
}

void Gsu::to(uint8_t r) {
    if (m_sfr.b) { writeReg16(r, src()); resetFlags(); }    // MOVE
    else m_destReg = r;
}

void Gsu::from(uint8_t r) {
    if (m_sfr.b) {                                   // MOVES
        writeDest(m_r[r]);
        m_sfr.ov = (m_r[r] & 0x80) != 0;
        m_sfr.s = (m_r[r] & 0x8000) != 0;
        m_sfr.z = m_r[r] == 0;
        resetFlags();
    } else {
        m_srcReg = r;
    }
}

void Gsu::with(uint8_t r) { m_srcReg = m_destReg = r; m_sfr.b = true; }

void Gsu::store(uint8_t r) {
    m_ramAddr = m_r[r];
    writeRam(m_ramAddr, (uint8_t)src());
    if (!m_sfr.alt1) writeRam(m_ramAddr ^ 1, (uint8_t)(src() >> 8));
    resetFlags();
}

void Gsu::load(uint8_t r) {
    m_ramAddr = m_r[r];
    uint16_t v = readRamBuffer(m_ramAddr);
    if (!m_sfr.alt1) v |= (uint16_t)(readRamBuffer(m_ramAddr ^ 1) << 8);
    writeDest(v);
    resetFlags();
}

void Gsu::loop() {
    --m_r[12];
    m_sfr.z = m_r[12] == 0;
    m_sfr.s = (m_r[12] & 0x8000) != 0;
    if (!m_sfr.z) writeReg16(15, m_r[13]);
    resetFlags();
}

void Gsu::merge() {
    const uint16_t v = (uint16_t)((m_r[7] & 0xFF00) | (m_r[8] >> 8));
    writeDest(v);
    m_sfr.cy = (v & 0xE0E0) != 0;
    m_sfr.ov = (v & 0xC0C0) != 0;
    m_sfr.s = (v & 0x8080) != 0;
    m_sfr.z = (v & 0xF0F0) != 0;
    resetFlags();
}

void Gsu::swap() {
    const uint16_t v = (uint16_t)((src() >> 8) | (src() << 8));
    writeDest(v);
    setZS(v);
    resetFlags();
}

void Gsu::add(uint8_t r) {
    const uint16_t operand = m_sfr.alt2 ? r : m_r[r];
    const uint16_t s = src();
    uint32_t res = (uint32_t)s + operand;
    if (m_sfr.alt1) res += m_sfr.cy ? 1 : 0;        // ADC
    m_sfr.cy = (res & 0x10000) != 0;
    m_sfr.ov = (~(s ^ operand) & (operand ^ res) & 0x8000) != 0;
    m_sfr.s = (res & 0x8000) != 0;
    m_sfr.z = (res & 0xFFFF) == 0;
    writeDest((uint16_t)res);
    resetFlags();
}

void Gsu::subCmp(uint8_t r) {
    const uint16_t operand = (m_sfr.alt2 && !m_sfr.alt1) ? r : m_r[r];
    const uint16_t s = src();
    int32_t res = (int32_t)s - operand;
    if (!m_sfr.alt2 && m_sfr.alt1) res -= m_sfr.cy ? 0 : 1;   // SBC
    m_sfr.cy = res >= 0;
    m_sfr.ov = ((s ^ operand) & (s ^ res) & 0x8000) != 0;
    m_sfr.s = (res & 0x8000) != 0;
    m_sfr.z = (res & 0xFFFF) == 0;
    if (!m_sfr.alt2 || !m_sfr.alt1) writeDest((uint16_t)res);    // CMP (ALT3) writes nothing
    resetFlags();
}

void Gsu::mult(uint8_t r) {
    const uint16_t operand = m_sfr.alt2 ? r : m_r[r];
    const uint16_t v = m_sfr.alt1 ? (uint16_t)((uint8_t)src() * (uint8_t)operand)       // UMULT
                                  : (uint16_t)((int8_t)src() * (int8_t)operand);
    writeDest(v);
    setZS(v);
    resetFlags();
    step(m_highSpeed ? 1 : 2);
}

void Gsu::fmultLmult() {
    const uint32_t res = (uint32_t)((int16_t)src() * (int16_t)m_r[6]);
    if (m_sfr.alt1) m_r[4] = (uint16_t)res;          // LMULT: the low word in R4
    const uint16_t v = (uint16_t)(res >> 16);
    writeDest(v);
    m_sfr.cy = (res & 0x8000) != 0;
    setZS(v);
    resetFlags();
    step((m_highSpeed ? 3 : 7) * (m_clockSelect ? 1 : 2));
}

void Gsu::andBic(uint8_t r) {
    const uint16_t operand = m_sfr.alt2 ? r : m_r[r];
    const uint16_t v = m_sfr.alt1 ? (uint16_t)(src() & ~operand) : (uint16_t)(src() & operand);
    writeDest(v);
    setZS(v);
    resetFlags();
}

void Gsu::sbk() {
    writeRam(m_ramAddr, (uint8_t)src());
    writeRam(m_ramAddr ^ 1, (uint8_t)(src() >> 8));
    resetFlags();
}

void Gsu::link(uint8_t n) { m_r[11] = (uint16_t)(m_r[15] + n); resetFlags(); }

void Gsu::sex() {
    const uint16_t v = (uint16_t)(int16_t)(int8_t)src();
    writeDest(v);
    setZS(v);
    resetFlags();
}

void Gsu::notOp() { const uint16_t v = (uint16_t)~src(); writeDest(v); setZS(v); resetFlags(); }

void Gsu::lsr() {
    const uint16_t s = src();
    m_sfr.cy = s & 1;
    const uint16_t d = s >> 1;
    writeDest(d);
    setZS(d);
    resetFlags();
}

void Gsu::rol() {
    const uint16_t s = src();
    const uint16_t d = (uint16_t)((s << 1) | (m_sfr.cy ? 1 : 0));
    m_sfr.cy = (s & 0x8000) != 0;
    writeDest(d);
    setZS(d);
    resetFlags();
}

void Gsu::asr() {
    const uint16_t s = src();
    m_sfr.cy = s & 1;
    uint16_t d = (uint16_t)((int16_t)s >> 1);
    if (m_sfr.alt1) d = (uint16_t)(d + ((s + 1) >> 16));     // DIV2
    writeDest(d);
    setZS(d);
    resetFlags();
}

void Gsu::ror() {
    const uint16_t s = src();
    const uint16_t d = (uint16_t)((s >> 1) | (m_sfr.cy ? 0x8000 : 0));
    m_sfr.cy = s & 1;
    writeDest(d);
    setZS(d);
    resetFlags();
}

void Gsu::lob() {
    const uint8_t v = (uint8_t)src();
    writeDest(v);
    m_sfr.z = v == 0; m_sfr.s = (v & 0x80) != 0;
    resetFlags();
}

void Gsu::hib() {
    const uint8_t v = (uint8_t)(src() >> 8);
    writeDest(v);
    m_sfr.z = v == 0; m_sfr.s = (v & 0x80) != 0;
    resetFlags();
}

void Gsu::ibtSmsLms(uint8_t r) {
    if (m_sfr.alt1) {                                // LMS
        m_ramAddr = (uint16_t)(readOperand() << 1);
        const uint8_t lo = readRamBuffer(m_ramAddr);
        const uint8_t hi = readRamBuffer(m_ramAddr | 1);
        writeReg16(r, (uint16_t)(hi << 8 | lo));
    } else if (m_sfr.alt2) {                         // SMS
        m_ramAddr = (uint16_t)(readOperand() << 1);
        writeRam(m_ramAddr, (uint8_t)m_r[r]);
        writeRam(m_ramAddr | 1, (uint8_t)(m_r[r] >> 8));
    } else {                                         // IBT
        writeReg16(r, (uint16_t)(int16_t)(int8_t)readOperand());
    }
    resetFlags();
}

void Gsu::iwtLmSm(uint8_t r) {
    if (m_sfr.alt1) {                                // LM
        m_ramAddr = readOperand();
        m_ramAddr |= (uint16_t)(readOperand() << 8);
        const uint8_t lo = readRamBuffer(m_ramAddr);
        const uint8_t hi = readRamBuffer(m_ramAddr ^ 1);
        writeReg16(r, (uint16_t)(hi << 8 | lo));
    } else if (m_sfr.alt2) {                         // SM
        m_ramAddr = readOperand();
        m_ramAddr |= (uint16_t)(readOperand() << 8);
        writeRam(m_ramAddr, (uint8_t)m_r[r]);
        writeRam(m_ramAddr ^ 1, (uint8_t)(m_r[r] >> 8));
    } else {                                         // IWT
        const uint8_t lo = readOperand();
        const uint8_t hi = readOperand();
        writeReg16(r, (uint16_t)(hi << 8 | lo));
    }
    resetFlags();
}

void Gsu::orXor(uint8_t r) {
    const uint16_t operand = m_sfr.alt2 ? r : m_r[r];
    const uint16_t v = m_sfr.alt1 ? (uint16_t)(src() ^ operand) : (uint16_t)(src() | operand);
    writeDest(v);
    setZS(v);
    resetFlags();
}

void Gsu::inc(uint8_t r) { writeReg16(r, (uint16_t)(m_r[r] + 1)); setZS(m_r[r]); resetFlags(); }
void Gsu::dec(uint8_t r) { writeReg16(r, (uint16_t)(m_r[r] - 1)); setZS(m_r[r]); resetFlags(); }

void Gsu::getcRambRomb() {
    if (!m_sfr.alt2) m_colorReg = color(readRomBuffer());            // GETC
    else if (!m_sfr.alt1) { waitRam(); m_ramBank = src() & 1; }       // RAMB
    else { waitRom(); m_romBank = src() & 0x7F; }                     // ROMB
    resetFlags();
}

void Gsu::getb() {
    if (m_sfr.alt2 && m_sfr.alt1) writeDest((uint16_t)(int16_t)(int8_t)readRomBuffer());     // GETBS
    else if (m_sfr.alt2) writeDest((uint16_t)((src() & 0xFF00) | readRomBuffer()));         // GETBL
    else if (m_sfr.alt1) writeDest((uint16_t)((src() & 0x00FF) | readRomBuffer() << 8));    // GETBH
    else writeDest(readRomBuffer());
    resetFlags();
}

void Gsu::plotRpix() {
    if (m_sfr.alt1) {                                // RPIX
        const uint8_t v = readPixel((uint8_t)m_r[1], (uint8_t)m_r[2]);
        m_sfr.z = v == 0;
        m_sfr.s = false;
        writeDest(v);
    } else {                                         // PLOT
        drawPixel((uint8_t)m_r[1], (uint8_t)m_r[2]);
        ++m_r[1];
    }
    resetFlags();
}

void Gsu::colorCmode() {
    if (m_sfr.alt1) {                                // CMODE
        const uint8_t v = (uint8_t)src();
        m_plotTransparent = v & 0x01;
        m_plotDither = v & 0x02;
        m_colorHigh = v & 0x04;
        m_colorFreeze = v & 0x08;
        m_objMode = v & 0x10;
    } else {
        m_colorReg = color((uint8_t)src());
    }
    resetFlags();
}

// ---------------------------------------------------------------------------
// PLOT
// ---------------------------------------------------------------------------

uint16_t Gsu::tileIndex(uint8_t x, uint8_t y) const {
    switch (m_objMode ? 3 : m_screenHeight) {
    case 1: return (uint16_t)(((x & 0xF8) << 1) + ((x & 0xF8) >> 1) + ((y & 0xF8) >> 3));       // 160 lines
    case 2: return (uint16_t)(((x & 0xF8) << 1) + (x & 0xF8) + ((y & 0xF8) >> 3));              // 192 lines
    case 3: return (uint16_t)(((y & 0x80) << 2) + ((x & 0x80) << 1) + ((y & 0x78) << 1) + ((x & 0x78) >> 3));   // OBJ
    default: return (uint16_t)(((x & 0xF8) << 1) + ((y & 0xF8) >> 3));                          // 128 lines
    }
}

uint32_t Gsu::tileAddress(uint8_t x, uint8_t y) const {
    return (0x700000u | (uint32_t)m_screenBase << 10) + (uint32_t)tileIndex(x, y) * (m_plotBpp << 3) + (uint32_t)((y & 7) * 2);
}

uint8_t Gsu::readPixel(uint8_t x, uint8_t y) {
    writePixelCache(m_secondary);
    writePixelCache(m_primary);
    const uint32_t a = tileAddress(x, y);
    const int bit = (x & 7) ^ 7;
    uint8_t v = 0;
    for (int i = 0; i < m_plotBpp; ++i) {
        const uint8_t off = (uint8_t)(((i >> 1) << 4) + (i & 1));
        v |= (uint8_t)(((readGsu(a + off) >> bit) & 1) << i);
        step(clk(5, 6));
    }
    return v;
}

bool Gsu::transparent() const {
    const uint8_t c = m_colorFreeze ? (m_colorReg & 0x0F) : m_colorReg;
    switch (m_plotBpp) {
    case 2: return (c & 3) == 0;
    case 4: return (c & 0x0F) == 0;
    default: return c == 0;
    }
}

void Gsu::drawPixel(uint8_t x, uint8_t y) {
    if (!m_plotTransparent && transparent()) return;
    uint8_t c = m_colorReg;
    if (m_plotDither && m_plotBpp != 8) {
        if ((x ^ y) & 1) c >>= 4;
        c &= 0x0F;
    }
    if (m_primary.x != (x & 0xF8) || m_primary.y != y) flushPrimary(x, y);
    const int bit = (x & 7) ^ 7;
    m_primary.pixels[bit] = c;
    m_primary.valid |= (uint8_t)(1 << bit);
    if (m_primary.valid == 0xFF) flushPrimary(x, y);
}

void Gsu::flushPrimary(uint8_t x, uint8_t y) {
    writePixelCache(m_secondary);
    m_secondary = m_primary;
    m_primary.valid = 0;
    m_primary.x = x & 0xF8;
    m_primary.y = y;
}

void Gsu::writePixelCache(PixelCache& c) {
    if (!c.valid) return;
    const uint32_t a = tileAddress(c.x, c.y);
    for (int i = 0; i < m_plotBpp; ++i) {
        uint8_t v = 0;
        for (int x = 0; x < 8; ++x) v |= (uint8_t)(((c.pixels[x] >> i) & 1) << x);
        const uint8_t off = (uint8_t)(((i >> 1) << 4) + (i & 1));
        if (c.valid != 0xFF) {
            step(clk(5, 6));
            v = (uint8_t)((v & c.valid) | (readGsu(a + off) & ~c.valid));
        }
        step(clk(5, 6));
        waitForRamAccess();
        writeGsu(a + off, v);
    }
    c.valid = 0;
}

uint8_t Gsu::color(uint8_t v) const {
    if (m_colorHigh) return (uint8_t)((m_colorReg & 0xF0) | (v >> 4));
    if (m_colorFreeze) return (uint8_t)((m_colorReg & 0xF0) | (v & 0x0F));
    return v;
}

}  // namespace snes
