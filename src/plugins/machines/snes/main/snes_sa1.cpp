// The SA-1 after Mesen2 (Core/SNES/Coprocessors/SA1/Sa1.cpp and its memory
// handlers), GPL-3.0, Sour and the Mesen contributors.

#include "snes_sa1.h"

#include <algorithm>
#include <cstring>

namespace snes {

// The SA-1 CPU's bus: its memory map, its own reset/NMI/IRQ vectors on
// vector pulls, and the extra cycle of BW-RAM accesses.
class Sa1::Bus : public IBus {
public:
    explicit Bus(Sa1* s) : m_s(s) { m_cfg = {24, 8, BusRole::DATA, true, 0xFFFFFF}; }
    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "sa1"; }
    uint8_t read8(uint32_t addr) override {
        addr &= 0xFFFFFF;
        if (m_s->m_cpu && m_s->m_cpu->vectorPull() && (addr & 0xFF0000) == 0) {
            switch (addr) {
            case 0xFFEA: return (uint8_t)m_s->m_sa1NmiVec;
            case 0xFFEB: return (uint8_t)(m_s->m_sa1NmiVec >> 8);
            case 0xFFEE: return (uint8_t)m_s->m_sa1IrqVec;
            case 0xFFEF: return (uint8_t)(m_s->m_sa1IrqVec >> 8);
            case 0xFFFC: return (uint8_t)m_s->m_sa1ResetVec;
            case 0xFFFD: return (uint8_t)(m_s->m_sa1ResetVec >> 8);
            default: break;
            }
        }
        wait(addr);
        return m_s->sa1Read(addr);
    }
    void write8(uint32_t addr, uint8_t v) override { addr &= 0xFFFFFF; wait(addr); m_s->sa1Write(addr, v); }
    uint8_t peek8(uint32_t addr) override {
        addr &= 0xFFFFFF;
        const uint8_t bank = (uint8_t)(addr >> 16);
        const uint16_t a = (uint16_t)addr;
        if (!(bank & 0x40) && (a >= 0x2000 && a < 0x3000)) return 0;     // registers: no side effects
        return m_s->sa1Read(addr);
    }
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

private:
    /** BW-RAM takes two SA-1 cycles. */
    void wait(uint32_t addr) {
        const uint8_t bank = (uint8_t)(addr >> 16);
        const uint16_t a = (uint16_t)addr;
        if ((bank >= 0x40 && bank < 0x70) || (!(bank & 0x40) && a >= 0x6000 && a < 0x8000)) ++m_s->m_extra;
    }
    Sa1* m_s;
    BusConfig m_cfg;
};

Sa1::Sa1(const std::vector<uint8_t>* rom, std::vector<uint8_t>* bwram) : m_rom(rom), m_bwram(bwram) {
    m_bus = std::make_unique<Bus>(this);
    m_cpu = std::make_unique<W65C816>();
    m_cpu->setDataBus(m_bus.get());
    reset();
}

Sa1::~Sa1() = default;

void Sa1::reset() {
    std::memset(m_iram, 0, sizeof m_iram);
    m_sa1ResetVec = m_sa1IrqVec = m_sa1NmiVec = 0;
    m_sa1IrqReq = m_sa1IrqEn = m_sa1NmiReq = m_sa1NmiEn = false;
    m_sa1Wait = m_sa1Reset = m_dmaIrqEn = m_timerIrqEn = false;
    m_sa1Msg = m_cpuMsg = 0;
    m_cpuIrqVec = m_cpuNmiVec = 0;
    m_useCpuIrqVec = m_useCpuNmiVec = m_cpuIrqReq = m_cpuIrqEn = false;
    m_ccIrqFlag = m_ccIrqEn = m_ccDmaActive = false;
    m_ccBpp = 8; m_ccFormat = m_ccWidth = m_ccCounter = 0;
    m_cpuBwBank = m_sa1BwBank = 0;
    m_cpuBwWe = m_sa1BwMode = m_sa1BwWe = m_bw2bpp = false;
    m_bwProtect = 0;
    std::memset(m_brf1, 0, sizeof m_brf1);
    std::memset(m_brf2, 0, sizeof m_brf2);
    m_cpuIramWp = m_sa1IramWp = 0;
    m_dmaSrc = m_dmaDest = 0; m_dmaSize = 0;
    m_dmaEnabled = m_dmaPriority = m_dmaCc = m_dmaCcAuto = false;
    m_dmaDestDev = m_dmaSrcDev = 0;
    m_dmaRunning = m_dmaIrqFlag = false;
    m_mathOp = 0; m_ma = m_mb = 0; m_mathStart = 0; m_mathResult = 0; m_mathOverflow = 0; m_mathPending = false;
    m_vlAutoInc = false; m_vlBits = 16; m_vlBit = 0; m_vlAddr = 0;
    m_openBus = 0;
    m_cycles = 0; m_extra = 0;
    m_c0 = m_cpu ? m_cpu->cycles() : 0;
    // As Mesen: the SA-1 held in reset, BW-RAM protected, the banks in order
    cpuRegWrite(0x2200, 0x20);
    cpuRegWrite(0x2228, 0xFF);
    for (int i = 0; i < 4; ++i) m_banks[i] = (uint8_t)i;
    m_cpu->setIrqLine(false);
    m_cpu->reset();
    m_c0 = m_cpu->cycles();
}

void Sa1::runTo(uint64_t master) {
    const uint64_t target = master / 2;
    while (m_cycles < target) {
        if (m_sa1Wait || m_sa1Reset) { m_cycles = target; break; }
        if (m_dmaRunning) { runDma(); continue; }
        // Waiting (WAI) or stopped: only an S-CPU register write can wake it, between runs
        if (m_cpu->isHalted()) { m_cycles = target; break; }
        m_c0 = m_cpu->cycles();
        m_extra = 0;
        m_cpu->step();
        m_cycles += (m_cpu->cycles() - m_c0) + m_extra;
        m_c0 = m_cpu->cycles();
        m_extra = 0;
    }
}

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------

int32_t Sa1::romOffset(uint8_t bank, uint16_t a) const {
    if (bank >= 0xC0) {
        const int region = (bank >> 4) & 3;
        return (int32_t)((uint32_t)(m_banks[region] & 7) * 0x100000 + (uint32_t)(bank & 0x0F) * 0x10000 + a);
    }
    if ((bank & 0x40) || a < 0x8000) return -1;
    const int region = ((bank & 0x80) ? 2 : 0) + ((bank & 0x20) ? 1 : 0);
    const uint32_t block = (m_banks[region] & 0x80) ? (m_banks[region] & 7) : (uint32_t)region;
    return (int32_t)(block * 0x100000 + (uint32_t)(bank & 0x1F) * 0x8000 + (a & 0x7FFF));
}

uint8_t Sa1::romByte(uint32_t off) const { return m_rom->empty() ? 0 : (*m_rom)[off % m_rom->size()]; }

bool Sa1::bwWritable(uint32_t off) const {
    if (m_cpuBwWe || m_sa1BwWe) return true;
    const uint32_t size = 256u << std::min<uint8_t>(m_bwProtect, 0x0A);
    return (off & 0x3FFFF) >= size;
}

uint8_t Sa1::bitmapRead(uint32_t addr) {
    if (m_bwram->empty()) return 0;
    if (m_bw2bpp) return (uint8_t)((bw(addr >> 2) >> ((addr & 3) * 2)) & 3);
    return (uint8_t)((bw(addr >> 1) >> ((addr & 1) * 4)) & 0x0F);
}

void Sa1::bitmapWrite(uint32_t addr, uint8_t v) {
    if (m_bwram->empty()) return;
    if (m_bw2bpp) {
        const int shift = (addr & 3) * 2;
        const uint32_t off = (addr >> 2) % m_bwram->size();
        bwWrite(off, (uint8_t)((bw(off) & ~(3 << shift)) | ((v & 3) << shift)));
    } else {
        const int shift = (addr & 1) * 4;
        const uint32_t off = (addr >> 1) % m_bwram->size();
        bwWrite(off, (uint8_t)((bw(off) & ~(0x0F << shift)) | ((v & 0x0F) << shift)));
    }
}

uint8_t Sa1::sa1Read(uint32_t addr) {
    const uint8_t bank = (uint8_t)(addr >> 16);
    const uint16_t a = (uint16_t)addr;
    uint8_t v = m_openBus;
    if (!(bank & 0x40)) {
        if (a < 0x1000 || (a >= 0x3000 && a < 0x4000)) v = (a & 0x800) ? 0 : m_iram[a & 0x7FF];
        else if (a >= 0x2000 && a < 0x3000) v = sa1RegRead(a);
        else if (a >= 0x6000 && a < 0x8000) {
            if (!m_bwram->empty()) {
                const uint32_t off = (uint32_t)m_sa1BwBank * 0x2000 | (a & 0x1FFF);
                v = m_sa1BwMode ? bitmapRead(off) : bw(off);
            }
        } else if (a >= 0x8000) v = romByte((uint32_t)romOffset(bank, a));
    } else if (bank < 0x60) {
        if (!m_bwram->empty()) v = bw((uint32_t)(bank - 0x40) << 16 | a);
    } else if (bank < 0x70) {
        v = bitmapRead((uint32_t)(bank - 0x60) << 16 | a);
    } else if (bank >= 0xC0) {
        v = romByte((uint32_t)romOffset(bank, a));
    }
    m_openBus = v;
    return v;
}

void Sa1::sa1Write(uint32_t addr, uint8_t v) {
    const uint8_t bank = (uint8_t)(addr >> 16);
    const uint16_t a = (uint16_t)addr;
    m_openBus = v;
    if (!(bank & 0x40)) {
        if (a < 0x1000 || (a >= 0x3000 && a < 0x4000)) {
            if (!(a & 0x800) && iramWritable(a, m_sa1IramWp)) m_iram[a & 0x7FF] = v;
        } else if (a >= 0x2000 && a < 0x3000) {
            sa1RegWrite(a, v);
        } else if (a >= 0x6000 && a < 0x8000) {
            const uint32_t off = (uint32_t)m_sa1BwBank * 0x2000 | (a & 0x1FFF);
            if (m_sa1BwMode) bitmapWrite(off, v);
            else if (!m_bwram->empty()) bwWrite(off % m_bwram->size(), v);
        }
    } else if (bank < 0x60) {
        if (!m_bwram->empty()) bwWrite(((uint32_t)(bank - 0x40) << 16 | a) % m_bwram->size(), v);
    } else if (bank < 0x70) {
        bitmapWrite((uint32_t)(bank - 0x60) << 16 | a, v);
    }
}

int Sa1::cpuRead(uint8_t bank, uint16_t a, bool peek) {
    if (!(bank & 0x40)) {
        if (a >= 0x2200 && a < 0x2400) {
            if (a == 0x2300) return cpuRegRead(a);
            return -1;
        }
        if (a >= 0x3000 && a < 0x4000) return (a & 0x800) ? 0 : m_iram[a & 0x7FF];
        if (a >= 0x6000 && a < 0x8000) {
            if (m_bwram->empty()) return -1;
            if (m_ccDmaActive && !peek) return charConvType1((uint32_t)bank << 16 | a);
            return bw((uint32_t)m_cpuBwBank * 0x2000 + (a - 0x6000));
        }
        if (a >= 0x8000) {
            // The S-CPU's NMI and IRQ vectors can come from SCNT ($2209)
            if (bank == 0x00) {
                if (m_useCpuNmiVec && (a == 0xFFEA || a == 0xFFEB)) return (uint8_t)(m_cpuNmiVec >> ((a & 1) * 8));
                if (m_useCpuIrqVec && (a == 0xFFEE || a == 0xFFEF)) return (uint8_t)(m_cpuIrqVec >> ((a & 1) * 8));
            }
            return romByte((uint32_t)romOffset(bank, a));
        }
        return -1;
    }
    if (bank < 0x50) {
        if (m_bwram->empty()) return -1;
        if (m_ccDmaActive && !peek) return charConvType1((uint32_t)bank << 16 | a);
        return bw((uint32_t)(bank & 0x0F) << 16 | a);
    }
    if (bank >= 0xC0) return romByte((uint32_t)romOffset(bank, a));
    return -1;
}

bool Sa1::cpuWrite(uint8_t bank, uint16_t a, uint8_t v) {
    if (!(bank & 0x40)) {
        if (a >= 0x2200 && a < 0x2400) { cpuRegWrite(a, v); return true; }
        if (a >= 0x3000 && a < 0x4000) {
            if (!(a & 0x800) && iramWritable(a, m_cpuIramWp)) m_iram[a & 0x7FF] = v;
            return true;
        }
        if (a >= 0x6000 && a < 0x8000) {
            if (!m_bwram->empty()) bwWrite(((uint32_t)m_cpuBwBank * 0x2000 + (a - 0x6000)) % m_bwram->size(), v);
            return true;
        }
        return a >= 0x8000;
    }
    if (bank < 0x50) {
        if (!m_bwram->empty()) bwWrite(((uint32_t)(bank & 0x0F) << 16 | a) % m_bwram->size(), v);
        return true;
    }
    return bank >= 0xC0;
}

bool Sa1::cpuIrq() const { return (m_cpuIrqReq && m_cpuIrqEn) || (m_ccIrqFlag && m_ccIrqEn); }

void Sa1::processInterrupts() {
    m_cpu->setIrqLine((m_sa1IrqReq && m_sa1IrqEn) || (m_dmaIrqFlag && m_dmaIrqEn));
    if (m_sa1NmiReq && m_sa1NmiEn) m_cpu->triggerNmi();
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

void Sa1::cpuRegWrite(uint16_t a, uint8_t v) {
    switch (a) {
    case 0x2200:                                     // CCNT
        if (!(v & 0x20) && m_sa1Reset) {
            // Out of reset: the SA-1 starts at its reset vector, from now
            m_sa1IramWp = 0;
            m_sa1Reset = false;
            m_cpu->reset();
            m_c0 = m_cpu->cycles();
        }
        m_sa1Msg = v & 0x0F;
        m_sa1NmiReq = v & 0x10;
        m_sa1Reset = v & 0x20;
        m_sa1Wait = v & 0x40;
        m_sa1IrqReq = v & 0x80;
        processInterrupts();
        break;
    case 0x2201: m_cpuIrqEn = v & 0x80; m_ccIrqEn = v & 0x20; processInterrupts(); break;    // SIE
    case 0x2202:                                     // SIC
        if (v & 0x80) m_cpuIrqReq = false;
        if (v & 0x20) m_ccIrqFlag = false;
        processInterrupts();
        break;
    case 0x2203: m_sa1ResetVec = (uint16_t)((m_sa1ResetVec & 0xFF00) | v); break;
    case 0x2204: m_sa1ResetVec = (uint16_t)((m_sa1ResetVec & 0x00FF) | v << 8); break;
    case 0x2205: m_sa1NmiVec = (uint16_t)((m_sa1NmiVec & 0xFF00) | v); break;
    case 0x2206: m_sa1NmiVec = (uint16_t)((m_sa1NmiVec & 0x00FF) | v << 8); break;
    case 0x2207: m_sa1IrqVec = (uint16_t)((m_sa1IrqVec & 0xFF00) | v); break;
    case 0x2208: m_sa1IrqVec = (uint16_t)((m_sa1IrqVec & 0x00FF) | v << 8); break;
    case 0x2220: case 0x2221: case 0x2222: case 0x2223: m_banks[a - 0x2220] = v; break;   // CXB-FXB
    case 0x2224: m_cpuBwBank = v & 0x1F; break;      // BMAPS
    case 0x2226: m_cpuBwWe = v & 0x80; break;        // SBWE
    case 0x2228: m_bwProtect = v & 0x0F; break;      // BWPA
    case 0x2229: m_cpuIramWp = v; break;             // SIWP
    case 0x2231: case 0x2232: case 0x2233: case 0x2234: case 0x2235: case 0x2236: case 0x2237:
        sharedRegWrite(a, v);
        break;
    default: break;
    }
}

uint8_t Sa1::cpuRegRead(uint16_t a) {
    if (a == 0x2300)                                 // SFR
        return (uint8_t)(m_cpuMsg | m_useCpuNmiVec << 4 | m_ccIrqFlag << 5 | m_useCpuIrqVec << 6 | m_cpuIrqReq << 7);
    return m_openBus;
}

void Sa1::sharedRegWrite(uint16_t a, uint8_t v) {
    switch (a) {
    case 0x2231:                                     // CDMA
        m_ccFormat = (uint8_t)std::min(v & 3, 2);
        m_ccBpp = m_ccFormat == 0 ? 8 : (m_ccFormat == 1 ? 4 : 2);
        m_ccWidth = (uint8_t)std::min((v & 0x1C) >> 2, 5);
        if (v & 0x80) m_ccDmaActive = false;         // end of character conversion type 1
        break;
    case 0x2232: m_dmaSrc = (m_dmaSrc & 0xFFFF00) | v; break;
    case 0x2233: m_dmaSrc = (m_dmaSrc & 0xFF00FF) | (uint32_t)v << 8; break;
    case 0x2234: m_dmaSrc = (m_dmaSrc & 0x00FFFF) | (uint32_t)v << 16; break;
    case 0x2235: m_dmaDest = (m_dmaDest & 0xFFFF00) | v; break;
    case 0x2236:
        m_dmaDest = (m_dmaDest & 0xFF00FF) | (uint32_t)v << 8;
        if (m_dmaEnabled && !m_dmaCc && m_dmaDestDev == 0) m_dmaRunning = true;          // to I-RAM
        else if (m_dmaCc && m_dmaCcAuto) { m_ccDmaActive = true; m_ccIrqFlag = true; processInterrupts(); }
        break;
    case 0x2237:
        m_dmaDest = (m_dmaDest & 0x00FFFF) | (uint32_t)v << 16;
        if (m_dmaEnabled && !m_dmaCc && m_dmaDestDev == 1) m_dmaRunning = true;          // to BW-RAM
        break;
    default: break;
    }
}

void Sa1::sa1RegWrite(uint16_t a, uint8_t v) {
    switch (a) {
    case 0x2209:                                     // SCNT
        m_cpuMsg = v & 0x0F;
        m_useCpuNmiVec = v & 0x10;
        m_useCpuIrqVec = v & 0x40;
        m_cpuIrqReq = v & 0x80;
        processInterrupts();
        break;
    case 0x220A:                                     // CIE
        m_sa1NmiEn = v & 0x10; m_dmaIrqEn = v & 0x20; m_timerIrqEn = v & 0x40; m_sa1IrqEn = v & 0x80;
        processInterrupts();
        break;
    case 0x220B:                                     // CIC
        if (v & 0x80) m_sa1IrqReq = false;
        if (v & 0x20) m_dmaIrqFlag = false;
        if (v & 0x10) m_sa1NmiReq = false;
        processInterrupts();
        break;
    case 0x220C: m_cpuNmiVec = (uint16_t)((m_cpuNmiVec & 0xFF00) | v); break;
    case 0x220D: m_cpuNmiVec = (uint16_t)((m_cpuNmiVec & 0x00FF) | v << 8); break;
    case 0x220E: m_cpuIrqVec = (uint16_t)((m_cpuIrqVec & 0xFF00) | v); break;
    case 0x220F: m_cpuIrqVec = (uint16_t)((m_cpuIrqVec & 0x00FF) | v << 8); break;
    case 0x2225: m_sa1BwBank = v & 0x7F; m_sa1BwMode = v & 0x80; break;    // BMAP
    case 0x2227: m_sa1BwWe = v & 0x80; break;        // CBWE
    case 0x222A: m_sa1IramWp = v; break;             // CIWP
    case 0x2230:                                     // DCNT
        m_dmaSrcDev = v & 3;
        m_dmaDestDev = (v >> 2) & 1;
        m_dmaCcAuto = v & 0x10;
        m_dmaCc = v & 0x20;
        m_dmaPriority = v & 0x40;
        m_dmaEnabled = v & 0x80;
        if (!m_dmaEnabled) m_ccCounter = 0;
        break;
    case 0x2231: case 0x2232: case 0x2233: case 0x2234: case 0x2235: case 0x2236: case 0x2237:
        sharedRegWrite(a, v);
        break;
    case 0x2238: m_dmaSize = (uint16_t)((m_dmaSize & 0xFF00) | v); break;
    case 0x2239: m_dmaSize = (uint16_t)((m_dmaSize & 0x00FF) | v << 8); break;
    case 0x223F: m_bw2bpp = v & 0x80; break;         // BBF
    case 0x2240: case 0x2241: case 0x2242: case 0x2243: case 0x2244: case 0x2245: case 0x2246: case 0x2247:
        m_brf1[a & 7] = v;
        if (a == 0x2247 && m_dmaEnabled && m_dmaCc && !m_dmaCcAuto) charConvType2();
        break;
    case 0x2248: case 0x2249: case 0x224A: case 0x224B: case 0x224C: case 0x224D: case 0x224E: case 0x224F:
        m_brf2[a & 7] = v;
        if (a == 0x224F && m_dmaEnabled && m_dmaCc && !m_dmaCcAuto) charConvType2();
        break;
    case 0x2250:                                     // MCNT
        processMath();
        m_mathOp = v & 3;
        if (v & 2) m_mathResult = 0;                 // starting a sum clears it
        break;
    case 0x2251: processMath(); m_ma = (uint16_t)((m_ma & 0xFF00) | v); break;
    case 0x2252: processMath(); m_ma = (uint16_t)((m_ma & 0x00FF) | v << 8); break;
    case 0x2253: processMath(); m_mb = (uint16_t)((m_mb & 0xFF00) | v); break;
    case 0x2254:                                     // writing MB's high byte starts the operation
        processMath();
        m_mb = (uint16_t)((m_mb & 0x00FF) | v << 8);
        m_mathStart = now();
        m_mathPending = true;
        break;
    case 0x2258:                                     // VBD
        m_vlAutoInc = v & 0x80;
        m_vlBits = v == 0 ? 16 : (v & 0x0F);
        if (!m_vlAutoInc) incVarLen();
        break;
    case 0x2259: m_vlAddr = (m_vlAddr & 0xFFFF00) | v; break;
    case 0x225A: m_vlAddr = (m_vlAddr & 0xFF00FF) | (uint32_t)v << 8; break;
    case 0x225B: m_vlAddr = (m_vlAddr & 0x00FFFF) | (uint32_t)v << 16; m_vlBit = 0; break;
    default: break;
    }
}

uint8_t Sa1::sa1RegRead(uint16_t a) {
    switch (a) {
    case 0x2301:                                     // CFR
        return (uint8_t)(m_sa1Msg | m_sa1NmiReq << 4 | m_dmaIrqFlag << 5 | m_sa1IrqReq << 7);
    case 0x2306: processMath(); return (uint8_t)m_mathResult;
    case 0x2307: processMath(); return (uint8_t)(m_mathResult >> 8);
    case 0x2308: processMath(); return (uint8_t)(m_mathResult >> 16);
    case 0x2309: processMath(); return (uint8_t)(m_mathResult >> 24);
    case 0x230A: processMath(); return (uint8_t)(m_mathResult >> 32);
    case 0x230B: processMath(); return m_mathOverflow;
    case 0x230C: case 0x230D: {                      // VDP: the bit stream
        const uint32_t addr = m_vlAddr;
        const uint32_t data = sa1Read(addr) | (uint32_t)sa1Read(addr + 1) << 8 | (uint32_t)sa1Read(addr + 2) << 16;
        if (a == 0x230C) return (uint8_t)(data >> m_vlBit);
        const uint8_t v = (uint8_t)(data >> (m_vlBit + 8));
        if (m_vlAutoInc) incVarLen();
        return v;
    }
    default: return m_openBus;
    }
}

void Sa1::incVarLen() {
    m_vlBit = (uint8_t)(m_vlBit + m_vlBits);
    m_vlAddr += m_vlBit >> 3;
    m_vlBit &= 7;
}

// The arithmetic takes 5 SA-1 cycles (6 for a sum); the result appears then
void Sa1::processMath() {
    if (!m_mathPending) return;
    const bool sum = m_mathOp & 2;
    if (now() - m_mathStart < (sum ? 6u : 5u)) return;      // still working
    m_mathPending = false;
    const int32_t product = (int32_t)(int16_t)m_ma * (int16_t)m_mb;
    if (sum) {
        const uint64_t r = m_mathResult + (uint64_t)(int64_t)product;
        m_mathOverflow = (uint8_t)((r >> 33) & 0x80);
        m_mathResult = r & 0xFFFFFFFFFFull;
        m_mb = 0;
    } else if (m_mathOp == 0) {
        m_mathResult = (uint32_t)product;
        m_mb = 0;
    } else {
        if (m_mb == 0) m_mathResult = 0;
        else {
            const int16_t dividend = (int16_t)m_ma;
            uint16_t rem = (uint16_t)(dividend % m_mb);
            if (dividend < 0) rem = (uint16_t)(rem + m_mb);
            const uint16_t q = (uint16_t)((dividend - rem) / m_mb);
            m_mathResult = (uint32_t)rem << 16 | q;
        }
        m_mb = 0;
        m_ma = 0;
    }
}

// ---------------------------------------------------------------------------
// DMA
// ---------------------------------------------------------------------------

void Sa1::runDma() {
    if (m_dmaSize > 0) {
        --m_dmaSize;
        uint8_t v = 0;
        if (m_dmaSrcDev == 0) {                      // ROM
            m_extra = 0;
            v = sa1Read(m_dmaSrc);
            m_extra = 0;
            m_cycles += m_dmaDestDev == 0 ? 1 : 2;
        } else if (m_dmaSrcDev == 1) {               // BW-RAM
            v = m_bwram->empty() ? 0 : bw(m_dmaSrc);
            m_cycles += 2;
        } else {                                     // I-RAM
            v = m_iram[m_dmaSrc & 0x7FF];
            m_cycles += 2;
        }
        if (m_dmaDestDev == 0) m_iram[m_dmaDest & 0x7FF] = v;
        else if (!m_bwram->empty()) bw(m_dmaDest) = v;
        ++m_dmaSrc;
        ++m_dmaDest;
    } else {
        m_cycles += 1;
    }
    if (m_dmaSize == 0) {
        m_dmaRunning = false;
        m_dmaIrqFlag = true;
        processInterrupts();
    }
}

// Type 1: the S-CPU's DMA reads BW-RAM bitmap rows, which come out as tiles (through I-RAM)
uint8_t Sa1::charConvType1(uint32_t addr) {
    const uint8_t mask = (uint8_t)(m_ccBpp * 8 - 1);
    if ((addr & mask) == 0) {
        const uint32_t bwMask = (uint32_t)m_bwram->size() - 1;
        const uint8_t tilesPerLine = (uint8_t)(1 << m_ccWidth);
        const uint32_t bytesPerLine = (uint32_t)(tilesPerLine * 8) >> m_ccFormat;
        const uint32_t tile = ((addr - m_dmaSrc) & bwMask) >> (6 - m_ccFormat);
        const uint32_t tx = tile & (tilesPerLine - 1u), ty = tile >> m_ccWidth;
        uint32_t src = m_dmaSrc + ty * 8 * bytesPerLine + tx * m_ccBpp;
        for (int y = 0; y < 8; ++y) {
            uint64_t data = 0;
            for (int i = 0; i < m_ccBpp; ++i) data |= (uint64_t)(*m_bwram)[(src + (uint32_t)i) & bwMask] << (i * 8);
            src += bytesPerLine;
            uint8_t out[8] = {};
            for (int x = 0; x < 8; ++x)
                for (int i = 0; i < m_ccBpp; ++i) { out[i] |= (uint8_t)((data & 1) << (7 - x)); data >>= 1; }
            for (int i = 0; i < m_ccBpp; ++i) {
                const uint8_t off = (uint8_t)((y << 1) + ((i >> 1) << 4) + (i & 1));
                m_iram[(m_dmaDest + off) & 0x7FF] = out[i];
            }
        }
    }
    return m_iram[(m_dmaDest + (addr & mask)) & 0x7FF];
}

// Type 2: the SA-1 writes a row of eight pixels to the bitmap registers; it lands in I-RAM as a tile row
void Sa1::charConvType2() {
    const uint8_t* regs = (m_ccCounter & 1) ? m_brf2 : m_brf1;
    uint16_t dest = (uint16_t)(m_dmaDest & 0x7FF);
    dest = (uint16_t)(dest & ~((m_ccBpp << 4) - 1));
    dest = (uint16_t)(dest + (m_ccCounter & 7) * 2);
    dest = (uint16_t)(dest + (m_ccCounter & 8) * m_ccBpp);
    for (int i = 0; i < m_ccBpp; ++i) {
        uint8_t v = 0;
        for (int j = 0; j < 8; ++j) v |= (uint8_t)(((regs[j] >> i) & 1) << (7 - j));
        const uint8_t off = (uint8_t)(((i >> 1) << 4) + (i & 1));
        m_iram[(dest + off) & 0x7FF] = v;
    }
    m_ccCounter = (m_ccCounter + 1) & 0x0F;
}

}  // namespace snes
