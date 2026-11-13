// Motorola MC6801 / MC6803, after the MC6801/MC6803 data sheet and the MC6801
// Reference Manual: instructions, flags, cycle counts, interrupts, the
// timer, the SCI and the ports. The undocumented opcodes behave as XRoar's
// mc6801.c found them on hardware.

#include "cpu6801.h"
#include "libdebug/main/execution_observer.h"
#include "libcore/main/intercept.h"
#include "libtoolchain/main/idisasm.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

const RegDescriptor REGS[M6801::R_COUNT] = {
    {"A", RegWidth::R8, REGFLAG_ACCUMULATOR, nullptr, nullptr},
    {"B", RegWidth::R8, 0, nullptr, nullptr},
    {"D", RegWidth::R16, 0, nullptr, nullptr},
    {"X", RegWidth::R16, 0, nullptr, nullptr},
    {"SP", RegWidth::R16, REGFLAG_SP, "S", nullptr},
    {"PC", RegWidth::R16, REGFLAG_PC, nullptr, nullptr},
    {"CC", RegWidth::R8, REGFLAG_STATUS, nullptr, "11HINZVC"},
};

// E cycles per opcode (the data sheet's; the undocumented ones as XRoar counts their bus cycles)
const uint8_t CYCLES[256] = {
    //0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
    2, 2, 2, 2, 3, 3, 2, 2, 3, 3, 2, 2, 2, 2, 2, 2,      // 0
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,      // 1
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,      // 2
    3, 3, 4, 4, 3, 3, 3, 3, 5, 5, 3, 10, 4, 10, 9, 12,   // 3
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,      // 4
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,      // 5
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 3, 6,      // 6
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 3, 6,      // 7
    2, 2, 2, 4, 2, 2, 2, 2, 2, 2, 2, 2, 4, 6, 3, 3,      // 8
    3, 3, 3, 5, 3, 3, 3, 3, 3, 3, 3, 3, 5, 5, 4, 4,      // 9
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 6, 6, 5, 5,      // A
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 6, 6, 5, 5,      // B
    2, 2, 2, 4, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3,      // C
    3, 3, 3, 5, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4,      // D
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,      // E
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,      // F
};

const int SCI_DIV[4] = {16, 128, 1024, 4096};

template <class T> void put(std::vector<uint8_t>& v, const T& x) {
    const auto* p = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), p, p + sizeof(T));
}
template <class T> void get(const uint8_t*& p, T& x) {
    std::memcpy(&x, p, sizeof(T));
    p += sizeof(T);
}

}  // namespace

M6801::M6801(Variant v) : m_variant(v) { reset(); }

// ---------------------------------------------------------------------------
// Registers and state
// ---------------------------------------------------------------------------
const RegDescriptor* M6801::regDescriptor(int idx) const {
    return idx >= 0 && idx < R_COUNT ? &REGS[idx] : nullptr;
}

uint32_t M6801::regRead(int idx) const {
    switch (idx) {
        case R_A: return m_a;
        case R_B: return m_b;
        case R_D: return d();
        case R_X: return m_x;
        case R_SP: return m_sp;
        case R_PC: return m_pc;
        case R_CC: return m_cc;
    }
    return 0;
}

void M6801::regWrite(int idx, uint32_t v) {
    switch (idx) {
        case R_A: m_a = (uint8_t)v; break;
        case R_B: m_b = (uint8_t)v; break;
        case R_D: setD((uint16_t)v); break;
        case R_X: m_x = (uint16_t)v; break;
        case R_SP: m_sp = (uint16_t)v; break;
        case R_PC: setPc(v); break;
        case R_CC: m_cc = (uint8_t)(v | 0xC0); break;
    }
}

void M6801::reset() {
    m_mode = m_modePins;
    m_cc |= CC_I | 0xC0;
    m_wait = m_hcf = m_iDelay = false;
    m_nmiPending = false;
    for (auto& p : m_port) p.ddr = 0;
    m_p3csr = 0;
    m_tcsr = 0;
    m_tcsrRead = 0;
    m_counter = 0;
    m_ocr = 0xFFFF;
    m_icr = 0;
    m_lsbLatch = 0;
    m_ocInhibit = false;
    m_ramc |= 0x40;                         // RAME
    m_rmcr = 0;
    m_trcsr = TR_TDRE;
    m_trRead = 0;
    m_sciDiv = 0;
    m_extClocks = 0;
    m_tx = true;
    m_txBits = 0;
    m_txPreamble = false;
    m_rxBits = 0;
    m_wakeOnes = 0;
    m_rxGenBit = -1;
    m_sub = 0;
    // The vector, read without clocking the timer
    auto raw = [&](uint16_t a) -> uint8_t {
        uint8_t v;
        if (peekInternal(a, v)) return v;
        return m_bus ? m_bus->read8(a) : 0xFF;
    };
    m_pc = (uint16_t)(raw(VEC_RESET) << 8 | raw(VEC_RESET + 1));
    for (int n = 1; n <= 4; ++n) if (port(n).postWrite) port(n).postWrite();
}

size_t M6801::stateSize() const { return 256; }

void M6801::saveState(uint8_t* buf) const {
    std::vector<uint8_t> v;
    put(v, m_a); put(v, m_b); put(v, m_cc); put(v, m_x); put(v, m_sp); put(v, m_pc);
    const uint8_t f = (uint8_t)((m_irq1 ? 1 : 0) | (m_nmiLine ? 2 : 0) | (m_nmiPending ? 4 : 0) | (m_wait ? 8 : 0) |
                                (m_hcf ? 16 : 0) | (m_iDelay ? 32 : 0) | (m_ocInhibit ? 64 : 0) | (m_tin ? 128 : 0));
    put(v, f);
    put(v, m_mode); put(v, m_ramc);
    v.insert(v.end(), m_ram, m_ram + 128);
    for (const auto& p : m_port) { put(v, p.ddr); put(v, p.data); put(v, p.in); }
    put(v, m_p3csr);
    put(v, m_counter); put(v, m_ocr); put(v, m_icr); put(v, m_tcsr); put(v, m_tcsrRead); put(v, m_lsbLatch);
    put(v, m_rmcr); put(v, m_trcsr); put(v, m_rdr); put(v, m_tdr); put(v, m_trRead);
    put(v, (int16_t)m_sciDiv); put(v, (uint8_t)m_tx); put(v, (uint8_t)m_txBits); put(v, (uint8_t)m_txPreamble);
    put(v, (uint8_t)m_preambleBits); put(v, m_tsr); put(v, (uint8_t)m_rxBits); put(v, m_rsr);
    v.resize(256, 0);
    std::memcpy(buf, v.data(), 256);
}

void M6801::loadState(const uint8_t* buf) {
    const uint8_t* p = buf;
    get(p, m_a); get(p, m_b); get(p, m_cc); get(p, m_x); get(p, m_sp); get(p, m_pc);
    uint8_t f = 0;
    get(p, f);
    m_irq1 = f & 1; m_nmiLine = f & 2; m_nmiPending = f & 4; m_wait = f & 8;
    m_hcf = f & 16; m_iDelay = f & 32; m_ocInhibit = f & 64; m_tin = f & 128;
    get(p, m_mode); get(p, m_ramc);
    std::memcpy(m_ram, p, 128);
    p += 128;
    for (auto& q : m_port) { get(p, q.ddr); get(p, q.data); get(p, q.in); }
    get(p, m_p3csr);
    get(p, m_counter); get(p, m_ocr); get(p, m_icr); get(p, m_tcsr); get(p, m_tcsrRead); get(p, m_lsbLatch);
    get(p, m_rmcr); get(p, m_trcsr); get(p, m_rdr); get(p, m_tdr); get(p, m_trRead);
    int16_t div = 0; uint8_t b8 = 0;
    get(p, div); m_sciDiv = div;
    get(p, b8); m_tx = b8;
    get(p, b8); m_txBits = b8;
    get(p, b8); m_txPreamble = b8;
    get(p, b8); m_preambleBits = b8;
    get(p, m_tsr);
    get(p, b8); m_rxBits = b8;
    get(p, m_rsr);
    m_rxQueue.clear();
    m_rxGenBit = -1;
}

// ---------------------------------------------------------------------------
// The address space
// ---------------------------------------------------------------------------
bool M6801::regMapped(uint16_t a) const {
    if (a >= 0x20) return false;
    const bool muxed = m_mode <= 3 || m_mode == 6;          // port 3 is the data bus
    switch (a) {
        case P3DDR: case P3DR: return !muxed;
        case P4DDR: case P4DR: return m_mode > 3;           // port 4 is the address bus in 0-3
        case P3CSR: return !(m_mode <= 3 || m_mode == 5 || m_mode == 6);
        default: return true;
    }
}

bool M6801::ramMapped(uint16_t a) const {
    return a >= 0x80 && a < 0x100 && (m_ramc & 0x40) && m_mode != 3;
}

bool M6801::romMapped(uint16_t a) const {
    return a >= 0xF800 && m_variant == Variant::MC6801 && !m_rom.empty() &&
           (m_mode == 0 || m_mode == 5 || m_mode == 6 || m_mode == 7);
}

bool M6801::isInternal(uint16_t a) const { return regMapped(a) || ramMapped(a) || romMapped(a); }

bool M6801::peekInternal(uint16_t a, uint8_t& v) const {
    if (ramMapped(a)) { v = m_ram[a & 0x7F]; return true; }
    if (romMapped(a)) { v = m_rom[(a - 0xF800) % m_rom.size()]; return true; }
    if (!regMapped(a)) return false;
    switch (a) {
        case P1DDR: case P2DDR: case P3DDR: case P4DDR: v = 0xFF; break;
        case P1DR: v = m_port[0].pins(); break;
        case P2DR: v = (uint8_t)(((port2Out() & m_port[1].ddr) | (m_port[1].in & ~m_port[1].ddr)) & 0x1F) | (uint8_t)(m_mode << 5); break;
        case P3DR: v = m_port[2].pins(); break;
        case P4DR: v = m_port[3].pins(); break;
        case TCSR: v = m_tcsr; break;
        case CRH: v = (uint8_t)(m_counter >> 8); break;
        case CRL: v = m_lsbLatch; break;
        case OCRH: v = (uint8_t)(m_ocr >> 8); break;
        case OCRL: v = (uint8_t)m_ocr; break;
        case ICRH: v = (uint8_t)(m_icr >> 8); break;
        case ICRL: v = (uint8_t)m_icr; break;
        case P3CSR: v = m_p3csr; break;
        case RMCR: v = (uint8_t)(m_rmcr | 0xF0); break;
        case TRCSR: v = m_trcsr; break;
        case RDR: v = m_rdr; break;
        case TDR: v = m_tdr; break;
        case RAMC: v = (uint8_t)(m_ramc | 0x3F); break;
        default: v = 0xFF; break;
    }
    return true;
}

bool M6801::readInternal(uint16_t a, uint8_t& v) {
    if (!isInternal(a)) return false;
    switch (a) {
        case P1DR: if (m_port[0].preRead) m_port[0].preRead(); break;
        case P2DR: if (m_port[1].preRead) m_port[1].preRead(); break;
        case P3DR: if (m_port[2].preRead) m_port[2].preRead(); break;
        case P4DR: if (m_port[3].preRead) m_port[3].preRead(); break;
        default: break;
    }
    peekInternal(a, v);
    if (!regMapped(a)) return true;
    switch (a) {
        case TCSR: m_tcsrRead = m_tcsr & 0xE0; break;      // the first step of clearing a flag
        case CRH:                                          // TOF: TCSR then the counter's MSB
            m_lsbLatch = (uint8_t)m_counter;
            if (m_tcsrRead & TCSR_TOF) { m_tcsr &= (uint8_t)~TCSR_TOF; m_tcsrRead &= (uint8_t)~TCSR_TOF; }
            break;
        case ICRH:                                         // ICF: TCSR then the capture's MSB
            if (m_tcsrRead & TCSR_ICF) { m_tcsr &= (uint8_t)~TCSR_ICF; m_tcsrRead &= (uint8_t)~TCSR_ICF; }
            break;
        case TRCSR: m_trRead = m_trcsr & (TR_RDRF | TR_ORFE | TR_TDRE); break;
        case RDR:                                          // RDRF, ORFE: TRCSR then RDR
            m_trcsr &= (uint8_t)~(m_trRead & (TR_RDRF | TR_ORFE));
            m_trRead &= (uint8_t)~(TR_RDRF | TR_ORFE);
            break;
        default: break;
    }
    return true;
}

bool M6801::writeInternal(uint16_t a, uint8_t v) {
    if (ramMapped(a)) { m_ram[a & 0x7F] = v; return true; }
    if (romMapped(a)) return true;
    if (!regMapped(a)) return false;
    switch (a) {
        case P1DDR: m_port[0].ddr = v; portWritten(1); break;
        case P2DDR: m_port[1].ddr = (uint8_t)(v & 0x1F); portWritten(2); break;
        case P3DDR: m_port[2].ddr = v; portWritten(3); break;
        case P4DDR: m_port[3].ddr = v; portWritten(4); break;
        case P1DR: m_port[0].data = v; portWritten(1); break;
        case P2DR: m_port[1].data = (uint8_t)(v & 0x1F); portWritten(2); break;
        case P3DR: m_port[2].data = v; portWritten(3); break;
        case P4DR: m_port[3].data = v; portWritten(4); break;
        case TCSR: m_tcsr = (uint8_t)((m_tcsr & 0xE0) | (v & 0x1F)); break;
        case CRH: m_counter = 0xFFF8; break;               // a write presets the counter
        case CRL: break;
        case OCRH: case OCRL:                              // OCF: TCSR then a write here
            if (a == OCRH) { m_ocr = (uint16_t)((m_ocr & 0x00FF) | v << 8); m_ocInhibit = true; }
            else m_ocr = (uint16_t)((m_ocr & 0xFF00) | v);
            if (m_tcsrRead & TCSR_OCF) { m_tcsr &= (uint8_t)~TCSR_OCF; m_tcsrRead &= (uint8_t)~TCSR_OCF; }
            break;
        case P3CSR: m_p3csr = (uint8_t)((m_p3csr & 0x80) | (v & 0x58)); break;
        case RMCR:
            if ((m_rmcr ^ v) & 0x0F) { m_sciDiv = 0; m_extClocks = 0; }
            m_rmcr = (uint8_t)(v & 0x0F);
            break;
        case TRCSR:
            if ((v & TR_TE) && !(m_trcsr & TR_TE)) {       // a preamble of ten 1s first
                m_txPreamble = true;
                m_preambleBits = 0;
                m_txBits = 0;
                m_tx = true;
                if (onTxLine) onTxLine(true);
            }
            if ((v & TR_RE) && !(m_trcsr & TR_RE)) m_rxBits = 0;
            if (v & TR_WU) m_wakeOnes = 0;
            m_trcsr = (uint8_t)((m_trcsr & 0xE0) | (v & 0x1F));
            portWritten(2);
            break;
        case TDR:                                          // TDRE: TRCSR then a write here
            if (m_trRead & TR_TDRE) { m_trcsr &= (uint8_t)~TR_TDRE; m_trRead &= (uint8_t)~TR_TDRE; }
            m_tdr = v;
            break;
        case RAMC: m_ramc = (uint8_t)(v & 0xC0); break;
        default: break;
    }
    return true;
}

void M6801::portWritten(int n) {
    if (port(n).postWrite) port(n).postWrite();
}

uint8_t M6801::port2Out() const {
    uint8_t o = (uint8_t)(m_port[1].out() & 0x1F);
    if (m_trcsr & TR_TE) o = (uint8_t)((o & ~0x10) | (m_tx ? 0x10 : 0));
    return o;
}

void M6801::setPort2Input(uint8_t in) {
    const bool old = m_tin;
    m_port[1].in = in;
    m_tin = (m_port[1].ddr & 1) ? m_tin : (in & 1) != 0;
    if (old != m_tin && m_tin == ((m_tcsr & TCSR_IEDG) != 0)) {   // the selected edge captures
        m_icr = m_counter;
        m_tcsr |= TCSR_ICF;
    }
}

// ---------------------------------------------------------------------------
// Bus cycles, the timer and the SCI
// ---------------------------------------------------------------------------
void M6801::tick() {
    ++m_counter;
    if (m_counter == 0xFFFF) m_tcsr |= TCSR_TOF;
    if (!m_ocInhibit && m_counter == m_ocr) {
        m_tcsr |= TCSR_OCF;
        if (m_port[1].ddr & 0x02) {                        // OLVL to P21
            m_port[1].data = (uint8_t)((m_port[1].data & ~0x02) | ((m_tcsr & TCSR_OLVL) ? 0x02 : 0));
            portWritten(2);
        }
    }
    m_ocInhibit = false;
    const int cc = (m_rmcr >> 2) & 3;
    if (cc != 3 && (m_trcsr & (TR_TE | TR_RE))) {
        if (++m_sciDiv >= SCI_DIV[m_rmcr & 3]) {
            m_sciDiv = 0;
            sciBit();
        }
    }
}

int M6801::sciBitCycles() const {
    return ((m_rmcr >> 2) & 3) == 3 ? 0 : SCI_DIV[m_rmcr & 3];
}

void M6801::sciExternalClock() {
    if (((m_rmcr >> 2) & 3) != 3) return;
    if (++m_extClocks >= 8) { m_extClocks = 0; sciBit(); }
}

bool M6801::rxLine() const {
    if (m_rxGenBit >= 0) {
        if (m_rxGenBit == 0) return false;
        if (m_rxGenBit == 9) return true;
        return (m_rxGenByte >> (m_rxGenBit - 1)) & 1;
    }
    return (m_port[1].in & 0x08) != 0;                     // P23
}

// One bit time: the transmitter shifts a bit out, the queued line moves on,
// the receiver samples it
void M6801::sciBit() {
    if (m_trcsr & TR_TE) {
        const bool was = m_tx;
        if (m_txPreamble) {
            m_tx = true;
            if (++m_preambleBits >= 10) m_txPreamble = false;
        } else if (m_txBits == 0) {
            if (!(m_trcsr & TR_TDRE)) {                    // a character waiting: its start bit
                m_tsr = m_tdr;
                m_txByte = m_tdr;
                m_trcsr |= TR_TDRE;
                m_tx = false;
                m_txBits = 1;
            }
        } else if (m_txBits <= 8) {
            m_tx = m_tsr & 1;
            m_tsr >>= 1;
            ++m_txBits;
        } else {                                           // the stop bit
            m_tx = true;
            m_txBits = 0;
            if (onTxByte) onTxByte(m_txByte);
        }
        if (was != m_tx) {
            if (onTxLine) onTxLine(m_tx);
            portWritten(2);
        }
    }
    // The queued characters
    if (m_rxGenBit >= 0 && ++m_rxGenBit > 9) m_rxGenBit = -1;
    if (m_rxGenBit < 0 && !m_rxQueue.empty()) {
        m_rxGenByte = m_rxQueue.front();
        m_rxQueue.pop_front();
        m_rxGenBit = 0;
    }
    if (!(m_trcsr & TR_RE)) return;
    const bool line = rxLine();
    if (m_trcsr & TR_WU) {                                 // asleep until ten 1s
        m_wakeOnes = line ? m_wakeOnes + 1 : 0;
        if (m_wakeOnes >= 10) { m_trcsr &= (uint8_t)~TR_WU; m_wakeOnes = 0; m_rxBits = 0; }
        return;
    }
    if (m_rxBits == 0) {
        if (!line) { m_rxBits = 1; m_rsr = 0; }
    } else if (m_rxBits <= 8) {
        m_rsr = (uint8_t)(m_rsr >> 1 | (line ? 0x80 : 0));
        ++m_rxBits;
    } else {
        if (!line) {                                       // framing error
            m_rdr = m_rsr;
            m_trcsr |= TR_ORFE;
        } else if (m_trcsr & TR_RDRF) {                    // overrun
            m_trcsr |= TR_ORFE;
        } else {
            m_rdr = m_rsr;
            m_trcsr |= TR_RDRF;
        }
        m_rxBits = 0;
    }
}

uint8_t M6801::rd(uint16_t a) {
    ++m_sub;
    tick();
    uint8_t v;
    if (readInternal(a, v)) return v;
    return m_bus ? m_bus->read8(a) : 0xFF;
}

void M6801::wr(uint16_t a, uint8_t v) {
    ++m_sub;
    tick();
    if (writeInternal(a, v)) return;
    if (m_bus) m_bus->write8(a, v);
}

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------
uint8_t M6801::add8(uint8_t a, uint8_t b, int carry) {
    const unsigned r = a + b + carry;
    flag(CC_H, (a ^ b ^ r) & 0x10);
    flag(CC_V, (a ^ r) & (b ^ r) & 0x80);
    flag(CC_C, r & 0x100);
    nz8((uint8_t)r);
    return (uint8_t)r;
}

uint8_t M6801::sub8(uint8_t a, uint8_t b, int borrow) {
    const unsigned r = (unsigned)a - b - borrow;
    flag(CC_V, (a ^ b) & (a ^ r) & 0x80);
    flag(CC_C, r & 0x100);
    nz8((uint8_t)r);
    return (uint8_t)r;
}

uint16_t M6801::add16(uint16_t a, uint16_t b) {
    const uint32_t r = (uint32_t)a + b;
    flag(CC_V, (a ^ r) & (b ^ r) & 0x8000);
    flag(CC_C, r & 0x10000);
    nz16((uint16_t)r);
    return (uint16_t)r;
}

uint16_t M6801::sub16(uint16_t a, uint16_t b) {
    const uint32_t r = (uint32_t)a - b;
    flag(CC_V, (a ^ b) & (a ^ r) & 0x8000);
    flag(CC_C, r & 0x10000);
    nz16((uint16_t)r);
    return (uint16_t)r;
}

// The read-modify-write group by its low nibble; `store` false for TST.
// Shifts and rotates leave V = N xor C. $1, $2, $5 and $B are undocumented.
uint8_t M6801::rmw(int op, uint8_t v, bool& store) {
    store = true;
    uint8_t r = v;
    switch (op) {
        case 0x0: r = sub8(0, v, 0); break;                                   // NEG
        case 0x1: sub8(0, v, 0); break;                                       // NGT: flags as NEG, no change
        case 0x2: r = sub8(0, v, (m_cc & CC_C) ? 1 : 0); break;               // NGC: COM if C, else NEG
        case 0x3: r = (uint8_t)~v; nz8(r); flag(CC_V, false); flag(CC_C, true); break;   // COM
        case 0x4: case 0x5: flag(CC_C, v & 1); r = v >> 1; nz8(r); vFromNC(); break;     // LSR
        case 0x6: r = (uint8_t)(v >> 1 | ((m_cc & CC_C) ? 0x80 : 0)); flag(CC_C, v & 1); nz8(r); vFromNC(); break;  // ROR
        case 0x7: r = (uint8_t)(v >> 1 | (v & 0x80)); flag(CC_C, v & 1); nz8(r); vFromNC(); break;  // ASR
        case 0x8: r = (uint8_t)(v << 1); flag(CC_C, v & 0x80); nz8(r); vFromNC(); break;           // ASL
        case 0x9: r = (uint8_t)(v << 1 | (m_cc & CC_C)); flag(CC_C, v & 0x80); nz8(r); vFromNC(); break;  // ROL
        case 0xA: r = (uint8_t)(v - 1); nz8(r); flag(CC_V, v == 0x80); break;                      // DEC
        case 0xB: r = (uint8_t)(v - 1); nz8(r); flag(CC_V, v == 0x80); flag(CC_C, v != 0); break;  // DC: DEC, C = no borrow
        case 0xC: r = (uint8_t)(v + 1); nz8(r); flag(CC_V, v == 0x7F); break;                      // INC
        case 0xD: nz8(v); flag(CC_V, false); flag(CC_C, false); store = false; break;              // TST (clears C)
        case 0xF: r = 0; m_cc = (uint8_t)((m_cc & ~(CC_N | CC_V | CC_C)) | CC_Z); break;           // CLR
        default: break;
    }
    return r;
}

bool M6801::branchTaken(int cond) const {
    const bool n = m_cc & CC_N, v = m_cc & CC_V, z = m_cc & CC_Z, c = m_cc & CC_C;
    switch (cond & 0xF) {
        case 0x0: return true;               // BRA
        case 0x1: return false;              // BRN
        case 0x2: return !(c || z);          // BHI
        case 0x3: return c || z;             // BLS
        case 0x4: return !c;                 // BCC
        case 0x5: return c;                  // BCS
        case 0x6: return !z;                 // BNE
        case 0x7: return z;                  // BEQ
        case 0x8: return !v;                 // BVC
        case 0x9: return v;                  // BVS
        case 0xA: return !n;                 // BPL
        case 0xB: return n;                  // BMI
        case 0xC: return n == v;             // BGE
        case 0xD: return n != v;             // BLT
        case 0xE: return !z && n == v;       // BGT
        default: return z || n != v;         // BLE
    }
}

// ---------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------
bool M6801::irq2() const {
    if ((m_tcsr & TCSR_ICF) && (m_tcsr & TCSR_EICI)) return true;
    if ((m_tcsr & TCSR_OCF) && (m_tcsr & TCSR_EOCI)) return true;
    if ((m_tcsr & TCSR_TOF) && (m_tcsr & TCSR_ETOI)) return true;
    if ((m_trcsr & TR_RIE) && (m_trcsr & (TR_RDRF | TR_ORFE))) return true;
    if ((m_trcsr & TR_TIE) && (m_trcsr & TR_TDRE)) return true;
    return false;
}

uint16_t M6801::irq2Vector() const {
    if ((m_tcsr & TCSR_ICF) && (m_tcsr & TCSR_EICI)) return VEC_ICF;
    if ((m_tcsr & TCSR_OCF) && (m_tcsr & TCSR_EOCI)) return VEC_OCF;
    if ((m_tcsr & TCSR_TOF) && (m_tcsr & TCSR_ETOI)) return VEC_TOF;
    return VEC_SCI;                          // the encoder's default
}

// The hardware sequence: two dead cycles, PC X A B CC stacked, a dead
// cycle, the vector (12 cycles); out of WAI (already stacked) a cycle and
// the vector
int M6801::takeInterrupt(uint16_t vec, bool stack) {
    if (stack) {
        ++m_sub; tick();
        ++m_sub; tick();
        push16(m_pc);
        push16(m_x);
        push8(m_a);
        push8(m_b);
        push8(m_cc);
    }
    ++m_sub; tick();
    m_cc |= CC_I;
    m_pc = rd16(vec);
    m_wait = false;
    return stack ? 12 : 3;
}

int M6801::interrupt() {
    if (m_hcf) return 0;
    if (m_nmiPending) {
        m_nmiPending = false;
        return takeInterrupt(VEC_NMI, !m_wait);
    }
    if (m_cc & CC_I) return 0;
    if (m_irq1) return takeInterrupt(VEC_IRQ1, !m_wait);
    if (irq2()) return takeInterrupt(irq2Vector(), !m_wait);
    return 0;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------
int M6801::step() {
    if (m_pcHook && !m_wait && !m_hcf && m_pcHook->armed(m_pc) && m_pcHook->fire(this, m_pc)) {
        m_cycles += 2;
        return 2;
    }
    m_sub = 0;
    int c = 0;
    const bool delayed = m_iDelay;           // CLI: one more instruction before an interrupt
    m_iDelay = false;
    if (m_hcf) {
        c = 1;
    } else if (!delayed && (c = interrupt()) != 0) {
        // taken
    } else if (m_wait) {
        c = 1;
    } else {
        if (m_observer) {
            if (m_observer->needsDisasm()) {
                DisasmEntry entry;
                disassembleEntry(m_bus, m_pc, &entry);
                if (!m_observer->onStep(this, m_bus, entry)) return 1;
            } else {
                m_observer->onStepLite(this, m_pc);
            }
        }
        c = exec(fetch());
    }
    while (m_sub < c) { ++m_sub; tick(); }
    m_cycles += (uint64_t)c;
    return c;
}

int M6801::exec(uint8_t op) {
    const int hi = op >> 4, lo = op & 0x0F;
    auto idle = [&]() { ++m_sub; tick(); };
    auto eaIdx = [&]() -> uint16_t { const uint16_t e = (uint16_t)(m_x + fetch()); idle(); return e; };

    // $40-$7F: read-modify-write on A, B, n,X and nnnn; JMP; HCF
    if (hi >= 0x4 && hi <= 0x7) {
        if (lo == 0xE) {
            if (hi <= 0x5) { m_hcf = true; return CYCLES[op]; }          // HCF: the CPU stops
            m_pc = hi == 0x6 ? (uint16_t)(m_x + fetch()) : fetch16();    // JMP
            return CYCLES[op];
        }
        bool store;
        if (hi == 0x4) { m_a = rmw(lo, m_a, store); return 2; }
        if (hi == 0x5) { m_b = rmw(lo, m_b, store); return 2; }
        const uint16_t ea = hi == 0x6 ? eaIdx() : fetch16();
        const uint8_t r = rmw(lo, rd(ea), store);
        if (store) { idle(); wr(ea, r); }
        return 6;
    }
    if (hi == 0x2) {                                                     // branches
        const uint16_t o = (uint16_t)(int16_t)(int8_t)fetch();
        idle();
        if (branchTaken(lo)) m_pc = (uint16_t)(m_pc + o);
        return 3;
    }
    if (hi == 0x0 || hi == 0x1 || hi == 0x3) {
        switch (op) {
            case 0x00: m_b = 0; return 2;                                // CLB (undocumented)
            case 0x01: return 2;                                         // NOP
            case 0x02: m_a = (m_cc & CC_C) ? 0xFF : 0x00; return 2;       // SEXA (undocumented)
            case 0x03: m_a = 0xFF; return 2;                             // SETA (undocumented)
            case 0x04: {                                                 // LSRD
                const uint16_t v = d();
                flag(CC_C, v & 1);
                setD((uint16_t)(v >> 1));
                nz16(d());
                vFromNC();
                return 3;
            }
            case 0x05: {                                                 // ASLD (LSLD)
                const uint16_t v = d();
                flag(CC_C, v & 0x8000);
                setD((uint16_t)(v << 1));
                nz16(d());
                vFromNC();
                return 3;
            }
            case 0x06:                                                   // TAP
                if ((m_cc & CC_I) && !(m_a & CC_I)) m_iDelay = true;
                m_cc = (uint8_t)(m_a | 0xC0);
                return 2;
            case 0x07: m_a = (uint8_t)(m_cc | 0xC0); return 2;            // TPA
            case 0x08: m_x = (uint16_t)(m_x + 1); flag(CC_Z, !m_x); return 3;   // INX
            case 0x09: m_x = (uint16_t)(m_x - 1); flag(CC_Z, !m_x); return 3;   // DEX
            case 0x0A: m_cc &= (uint8_t)~CC_V; return 2;                 // CLV
            case 0x0B: m_cc |= CC_V; return 2;                           // SEV
            case 0x0C: m_cc &= (uint8_t)~CC_C; return 2;                 // CLC
            case 0x0D: m_cc |= CC_C; return 2;                           // SEC
            case 0x0E:                                                   // CLI
                if (m_cc & CC_I) m_iDelay = true;
                m_cc &= (uint8_t)~CC_I;
                return 2;
            case 0x0F: m_cc |= CC_I; return 2;                           // SEI
            case 0x10: m_a = sub8(m_a, m_b, 0); return 2;                // SBA
            case 0x11: sub8(m_a, m_b, 0); return 2;                      // CBA
            case 0x12: m_a = sub8(m_a, m_b, (m_cc & CC_C) ? 1 : 0); return 2;   // SCBA (undocumented)
            case 0x13: m_a = sub8(m_a, m_b, 1); return 2;                // S1BA (undocumented)
            case 0x14: case 0x1C: {                                      // TCAB: B = A - 1 (undocumented)
                const uint8_t r = (uint8_t)(m_a - 1);
                flag(CC_V, m_a == 0x80); nz8(r); m_b = r;
                return 2;
            }
            case 0x15: {                                                 // TCBA: A = B - 1 (undocumented)
                const uint8_t r = (uint8_t)(m_b - 1);
                flag(CC_V, m_b == 0x80); nz8(r); m_a = r;
                return 2;
            }
            case 0x16: case 0x1E: m_b = logic(m_a); return 2;            // TAB
            case 0x17: m_a = logic(m_b); return 2;                       // TBA
            case 0x18: case 0x1A: {                                      // ABA without H and C (undocumented)
                const uint8_t keep = m_cc & (CC_H | CC_C);
                m_a = add8(m_a, m_b, 0);
                m_cc = (uint8_t)((m_cc & ~(CC_H | CC_C)) | keep);
                return 2;
            }
            case 0x19: {                                                 // DAA
                const uint8_t in = m_a;
                unsigned add = 0;
                if ((in & 0x0F) >= 0x0A || (m_cc & CC_H)) add |= 0x06;
                if ((in >= 0x90 && (in & 0x0F) >= 0x0A) || in >= 0xA0 || (m_cc & CC_C)) add |= 0x60;
                const unsigned r = in + add;
                m_a = (uint8_t)r;
                nz8(m_a);
                flag(CC_V, (in ^ r) & (add ^ r) & 0x80);
                if (r & 0x100) m_cc |= CC_C;                             // C set, never cleared
                return 2;
            }
            case 0x1B: m_a = add8(m_a, m_b, 0); return 2;                // ABA
            case 0x1D: {                                                 // TCBA, C = no borrow (undocumented)
                const uint8_t r = (uint8_t)(m_b - 1);
                flag(CC_V, m_b == 0x80); flag(CC_C, m_b != 0); nz8(r); m_a = r;
                return 2;
            }
            case 0x1F: m_a = logic(m_b); m_cc |= CC_C; return 2;         // TBAC (undocumented)
            case 0x30: m_x = (uint16_t)(m_sp + 1); return 3;             // TSX
            case 0x31: m_sp = (uint16_t)(m_sp + 1); return 3;            // INS
            case 0x32: idle(); m_a = pull8(); return 4;                  // PULA
            case 0x33: idle(); m_b = pull8(); return 4;                  // PULB
            case 0x34: m_sp = (uint16_t)(m_sp - 1); return 3;            // DES
            case 0x35: m_sp = (uint16_t)(m_x - 1); return 3;             // TXS
            case 0x36: idle(); push8(m_a); return 3;                     // PSHA
            case 0x37: idle(); push8(m_b); return 3;                     // PSHB
            case 0x38: idle(); m_x = pull16(); return 5;                 // PULX
            case 0x39: idle(); m_pc = pull16(); return 5;                // RTS
            case 0x3A: m_x = (uint16_t)(m_x + m_b); return 3;            // ABX
            case 0x3B:                                                   // RTI
                idle(); idle();
                m_cc = (uint8_t)(pull8() | 0xC0);
                m_b = pull8();
                m_a = pull8();
                m_x = pull16();
                m_pc = pull16();
                return 10;
            case 0x3C: idle(); push16(m_x); return 4;                    // PSHX
            case 0x3D: {                                                 // MUL
                const uint16_t r = (uint16_t)(m_a * m_b);
                setD(r);
                flag(CC_C, r & 0x80);
                return 10;
            }
            case 0x3E:                                                   // WAI
                idle();
                push16(m_pc); push16(m_x); push8(m_a); push8(m_b); push8(m_cc);
                m_wait = true;
                return 9;
            case 0x3F:                                                   // SWI
                idle();
                push16(m_pc); push16(m_x); push8(m_a); push8(m_b); push8(m_cc);
                idle();
                m_cc |= CC_I;
                m_pc = rd16(VEC_SWI);
                return 12;
        }
        return 2;
    }

    // $80-$FF: the accumulator and register group; mode by bits 4-5
    const int mode = (op >> 4) & 3;                                      // 0 imm, 1 dir, 2 idx, 3 ext
    const bool bSide = op & 0x40;
    auto ea = [&]() -> uint16_t {
        if (mode == 1) return fetch();
        if (mode == 2) return eaIdx();
        return fetch16();
    };
    auto read8 = [&]() -> uint8_t { return mode == 0 ? fetch() : rd(ea()); };
    auto read16 = [&]() -> uint16_t { return mode == 0 ? fetch16() : rd16(ea()); };
    uint8_t& acc = bSide ? m_b : m_a;
    const int cyc = CYCLES[op];
    switch (lo) {
        case 0x0: acc = sub8(acc, read8(), 0); return cyc;                         // SUB
        case 0x1: sub8(acc, read8(), 0); return cyc;                               // CMP
        case 0x2: acc = sub8(acc, read8(), (m_cc & CC_C) ? 1 : 0); return cyc;     // SBC
        case 0x3: {                                                                 // SUBD / ADDD
            const uint16_t v = read16();
            setD(bSide ? add16(d(), v) : sub16(d(), v));
            return cyc;
        }
        case 0x4: acc = logic(acc & read8()); return cyc;                          // AND
        case 0x5: logic(acc & read8()); return cyc;                                // BIT
        case 0x6: acc = logic(read8()); return cyc;                                // LDA
        case 0x7:
            if (mode == 0) { fetch(); acc = logic(0xFF); return cyc; }             // $87/$C7 (undocumented): $FF
            { const uint16_t a = ea(); wr(a, acc); logic(acc); return cyc; }       // STA
        case 0x8: acc = logic(acc ^ read8()); return cyc;                          // EOR
        case 0x9: acc = add8(acc, read8(), (m_cc & CC_C) ? 1 : 0); return cyc;     // ADC
        case 0xA: acc = logic(acc | read8()); return cyc;                          // ORA
        case 0xB: acc = add8(acc, read8(), 0); return cyc;                         // ADD
        case 0xC:
            if (!bSide) { sub16(m_x, read16()); return cyc; }                      // CPX (all four flags)
            setD(read16()); nz16(d()); flag(CC_V, false); return cyc;              // LDD
        case 0xD:
            if (!bSide) {
                uint16_t target;
                if (mode == 0) {                                                   // BSR
                    const uint16_t o = (uint16_t)(int16_t)(int8_t)fetch();
                    idle();
                    target = (uint16_t)(m_pc + o);
                } else {
                    target = ea();                                                 // JSR
                }
                idle();
                push16(m_pc);
                m_pc = target;
                return cyc;
            }
            if (mode == 0) { fetch16(); nz16(d()); flag(CC_V, false); return cyc; }   // $CD (undocumented)
            { const uint16_t a = ea(); wr16(a, d()); nz16(d()); flag(CC_V, false); return cyc; }   // STD
        case 0xE: {                                                                // LDS / LDX
            const uint16_t v = read16();
            if (bSide) m_x = v; else m_sp = v;
            nz16(v); flag(CC_V, false);
            return cyc;
        }
        default: {                                                                 // STS / STX
            const uint16_t r = bSide ? m_x : m_sp;
            if (mode == 0) { fetch16(); nz16(r); flag(CC_V, false); return cyc; }  // $8F/$CF (undocumented)
            const uint16_t a = ea();
            wr16(a, r); nz16(r); flag(CC_V, false);
            return cyc;
        }
    }
}

// ---------------------------------------------------------------------------
// Disassembler (Motorola mnemonics)
// ---------------------------------------------------------------------------
namespace {

const char* const INH0[64] = {
    nullptr, "NOP", nullptr, nullptr, "LSRD", "ASLD", "TAP", "TPA", "INX", "DEX", "CLV", "SEV", "CLC", "SEC", "CLI", "SEI",
    "SBA", "CBA", nullptr, nullptr, nullptr, nullptr, "TAB", "TBA", nullptr, "DAA", nullptr, "ABA", nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    "TSX", "INS", "PULA", "PULB", "DES", "TXS", "PSHA", "PSHB", "PULX", "RTS", "ABX", "RTI", "PSHX", "MUL", "WAI", "SWI",
};
const char* const RMW[16] = {"NEG", nullptr, nullptr, "COM", "LSR", nullptr, "ROR", "ASR",
                             "ASL", "ROL", "DEC", nullptr, "INC", "TST", "JMP", "CLR"};
const char* const BR[16] = {"BRA", "BRN", "BHI", "BLS", "BCC", "BCS", "BNE", "BEQ",
                            "BVC", "BVS", "BPL", "BMI", "BGE", "BLT", "BGT", "BLE"};
const char* const ACC[16] = {"SUB", "CMP", "SBC", nullptr, "AND", "BIT", "LDA", "STA",
                             "EOR", "ADC", "ORA", "ADD", nullptr, nullptr, nullptr, nullptr};

std::string hex2(unsigned v) { char b[8]; std::snprintf(b, sizeof b, "$%02X", v & 0xFF); return b; }
std::string hex4(unsigned v) { char b[8]; std::snprintf(b, sizeof b, "$%04X", v & 0xFFFF); return b; }

struct Decoded {
    std::string text;
    int len = 1;
    bool illegal = false;
    uint32_t target = 0;
    bool hasTarget = false;
};

Decoded decode(IBus* bus, uint32_t addr) {
    Decoded r;
    auto byte = [&](int i) -> uint8_t { return bus ? bus->peek8((addr + i) & 0xFFFF) : 0; };
    const uint8_t op = byte(0);
    const int hi = op >> 4, lo = op & 15;
    auto ill = [&]() { r.text = "FCB " + hex2(op); r.illegal = true; r.len = 1; };
    if (hi <= 0x1 || hi == 0x3) {
        if (INH0[op]) r.text = INH0[op];
        else ill();
        return r;
    }
    if (hi == 0x2) {
        const int8_t o = (int8_t)byte(1);
        r.len = 2;
        r.target = (addr + 2 + o) & 0xFFFF;
        r.hasTarget = true;
        r.text = std::string(BR[lo]) + " " + hex4(r.target);
        return r;
    }
    if (hi >= 0x4 && hi <= 0x7) {
        if (!RMW[lo] || (lo == 0xE && hi <= 0x5)) { ill(); return r; }
        if (hi == 0x4 || hi == 0x5) { r.text = std::string(RMW[lo]) + (hi == 0x4 ? "A" : "B"); return r; }
        if (hi == 0x6) { r.len = 2; r.text = std::string(RMW[lo]) + " " + hex2(byte(1)) + ",X"; return r; }
        r.len = 3;
        const uint16_t a = (uint16_t)(byte(1) << 8 | byte(2));
        r.text = std::string(RMW[lo]) + " " + hex4(a);
        if (lo == 0xE) { r.target = a; r.hasTarget = true; }
        return r;
    }
    const int mode = (op >> 4) & 3;
    const bool bSide = op & 0x40;
    std::string mn;
    bool wide = false;
    if (ACC[lo]) {
        mn = std::string(ACC[lo]) + (bSide ? "B" : "A");
        if (lo == 0x7 && mode == 0) { ill(); return r; }
    } else {
        switch (lo) {
            case 0x3: mn = bSide ? "ADDD" : "SUBD"; wide = true; break;
            case 0xC: mn = bSide ? "LDD" : "CPX"; wide = true; break;
            case 0xD:
                if (bSide) { if (mode == 0) { ill(); return r; } mn = "STD"; wide = true; }
                else if (mode == 0) {
                    const int8_t o = (int8_t)byte(1);
                    r.len = 2;
                    r.target = (addr + 2 + o) & 0xFFFF;
                    r.hasTarget = true;
                    r.text = "BSR " + hex4(r.target);
                    return r;
                } else mn = "JSR";
                break;
            case 0xE: mn = bSide ? "LDX" : "LDS"; wide = true; break;
            default:
                if (mode == 0) { ill(); return r; }
                mn = bSide ? "STX" : "STS"; wide = true; break;
        }
    }
    std::string operand;
    switch (mode) {
        case 0:
            if (wide) { operand = "#" + hex4((unsigned)(byte(1) << 8 | byte(2))); r.len = 3; }
            else { operand = "#" + hex2(byte(1)); r.len = 2; }
            break;
        case 1: operand = hex2(byte(1)); r.len = 2; if (mn == "JSR") { r.target = byte(1); r.hasTarget = true; } break;
        case 2: operand = hex2(byte(1)) + ",X"; r.len = 2; break;
        default: {
            const uint16_t a = (uint16_t)(byte(1) << 8 | byte(2));
            operand = hex4(a);
            r.len = 3;
            if (mn == "JSR") { r.target = a; r.hasTarget = true; }
            break;
        }
    }
    r.text = mn + " " + operand;
    return r;
}

}  // namespace

int M6801::disassembleOne(IBus* bus, uint32_t addr, char* buf, int bufsz) {
    const Decoded d = decode(bus, addr);
    std::snprintf(buf, (size_t)bufsz, "%s", d.text.c_str());
    return d.len;
}

int M6801::disassembleEntry(IBus* bus, uint32_t addr, void* entryOut) {
    auto* e = static_cast<DisasmEntry*>(entryOut);
    const Decoded d = decode(bus, addr);
    e->addr = addr;
    e->bytes = d.len;
    const auto sp = d.text.find(' ');
    e->mnemonic = sp == std::string::npos ? d.text : d.text.substr(0, sp);
    e->operands = sp == std::string::npos ? "" : d.text.substr(sp + 1);
    e->complete = d.text;
    e->isCall = isCallAt(bus, addr) > 0;
    e->isReturn = isReturnAt(bus, addr);
    const std::string& m = e->mnemonic;
    e->isBranch = !e->isCall && ((m.size() == 3 && m[0] == 'B' && m != "BIT") || m == "JMP");
    e->isIllegal = d.illegal;
    e->targetAddr = d.hasTarget ? d.target : 0;
    e->stackOp = StackOp::NONE;
    e->rawLen = (uint8_t)std::min(e->bytes, (int)DisasmEntry::MAX_RAW);
    for (int i = 0; i < e->rawLen; ++i) e->raw[i] = bus ? bus->peek8((addr + i) & 0xFFFF) : 0;
    return e->bytes;
}

int M6801::isCallAt(IBus* bus, uint32_t addr) {
    if (!bus) return 0;
    switch (bus->peek8(addr & 0xFFFF)) {
        case 0x8D: return 2;                 // BSR
        case 0x9D: return 2;                 // JSR direct
        case 0xAD: return 2;                 // JSR indexed
        case 0xBD: return 3;                 // JSR extended
        case 0x3F: return 1;                 // SWI
    }
    return 0;
}

bool M6801::isReturnAt(IBus* bus, uint32_t addr) {
    if (!bus) return false;
    const uint8_t op = bus->peek8(addr & 0xFFFF);
    return op == 0x39 || op == 0x3B;         // RTS, RTI
}

bool M6801::isProgramEnd(IBus* bus) { (void)bus; return m_hcf || (m_wait && (m_cc & CC_I)); }

int M6801::writeCallHarness(IBus* bus, uint32_t scratch, uint32_t routineAddr) {
    // JSR routine ; SEI ; WAI (ends waiting with interrupts masked)
    bus->write8(scratch, 0xBD);
    bus->write8(scratch + 1, (routineAddr >> 8) & 0xFF);
    bus->write8(scratch + 2, routineAddr & 0xFF);
    bus->write8(scratch + 3, 0x0F);
    bus->write8(scratch + 4, 0x3E);
    return 5;
}
