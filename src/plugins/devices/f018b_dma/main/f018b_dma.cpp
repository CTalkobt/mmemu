#include "f018b_dma.h"
#include "libmem/main/ibus.h"
#include "include/mmemu_plugin_api.h"
#include <cstring>

F018bDmaDevice::F018bDmaDevice(uint32_t base)
    : m_base(base), m_bus(nullptr), m_dmaListAddr(0), m_dmaActive(false),
      m_enhancedMode(false), m_currentJob(0), m_bytesRemaining(0),
      m_srcAccum(0), m_dstAccum(0), m_srcBase(0), m_dstBase(0),
      m_srcStep(0x0100), m_dstStep(0x0100), m_fillByte(0),
      m_currentOp(DMA_COPY), m_backward(false) {
    std::memset(m_regs, 0, sizeof(m_regs));
}

void F018bDmaDevice::getDeviceInfo(DeviceInfo& out) const {
    out.name = m_name;
    out.baseAddr = m_base;
    out.addrMask = addrMask();

    auto addReg = [&](const std::string& rname, uint32_t offset, const std::string& desc) {
        out.registers.push_back({rname, offset, m_regs[offset], desc});
    };

    addReg("ADDRLSBTRIG",  0x00, "DMA list addr LSB; write triggers C65-compat DMA");
    addReg("ADDRMSB",      0x01, "DMA list addr high (bits 15:8)");
    addReg("ADDRBANK",     0x02, "DMA list addr bank (bits 22:16); resets ADDRMB");
    addReg("EN018B",       0x03, "EN018B (bit 0), NOMBWRAP (bit 1)");
    addReg("ADDRMB",       0x04, "DMA upper address (bits 27:20)");
    addReg("ETRIG",        0x05, "Enhanced DMA trigger (flat 28-bit addr)");
    addReg("ETRIGMAPD",    0x06, "Enhanced DMA trigger (MAP'd 16-bit addr)");
    for (uint32_t i = 0x07; i <= 0x0F; ++i) {
        addReg("RSV_" + std::to_string(i), i, "Reserved");
    }

    char buf[32];
    std::snprintf(buf, sizeof(buf), "$%07X", m_dmaListAddr);
    out.state.push_back({"List Address", buf});
    out.state.push_back({"DMA Active", m_dmaActive ? "true" : "false"});
    out.state.push_back({"Enhanced Mode", m_enhancedMode ? "true" : "false"});
    out.state.push_back({"Last Job Count", std::to_string(m_jobs.size())});

    out.dependencies.push_back({"DMA Bus", m_bus ? "connected" : "none"});
}

void F018bDmaDevice::reset() {
    std::memset(m_regs, 0, sizeof(m_regs));
    m_dmaListAddr = 0;
    m_dmaActive = false;
    m_jobs.clear();
    m_currentJob = 0;
    m_bytesRemaining = 0;
}

bool F018bDmaDevice::ioRead(IBus* /*bus*/, uint32_t addr, uint8_t* val) {
    uint32_t offset = addr - m_base;
    if (offset >= 16) return false;
    *val = m_regs[offset];
    return true;
}

bool F018bDmaDevice::ioWrite(IBus* bus, uint32_t addr, uint8_t val) {
    uint32_t offset = addr - m_base;
    if (offset >= 16) return false;

    m_regs[offset] = val;
    m_bus = bus;  // Cache the bus reference for DMA operations

    // $D702: writing ADDRBANK resets ADDRMB ($D704) to zero for compatibility
    // (programs that predate the megabyte register don't set it)
    if (offset == 0x02) {
        m_regs[0x04] = 0x00;
    }

    // $D700: ADDRLSBTRIG — write triggers C65-compatible DMA execution
    if (offset == 0x00) {
        m_enhancedMode = false;
        startDma();
    }
    // $D705: ETRIG — write triggers Enhanced DMA (flat 28-bit address)
    else if (offset == 0x05) {
        m_enhancedMode = true;
        startDma();
    }

    return true;
}

void F018bDmaDevice::tick(uint64_t /*cycles*/) {
    if (!m_dmaActive) return;
    tickOneByte();
}

// ---------------------------------------------------------------------------
// DMA initiation: fetch job list, set up first job
// ---------------------------------------------------------------------------

void F018bDmaDevice::startDma() {
    if (!m_bus) return;

    // Assemble 28-bit DMA list address from registers.
    // For ETRIG ($D705), the low byte comes from $D705 (regs[5]), not $D700.
    uint32_t addr_low = m_enhancedMode ? m_regs[0x05] : m_regs[0x00];
    uint32_t addr_mid = m_regs[0x01];
    uint32_t addr_bank = m_regs[0x02];
    uint32_t addr_upper = m_regs[0x04];

    m_dmaListAddr = (addr_low) | (addr_mid << 8) | ((addr_bank & 0x0F) << 16) | (addr_upper << 20);
    m_dmaListAddr &= 0x0FFFFFFF;

    m_jobs.clear();
    if (!fetchJobList(m_dmaListAddr)) return;

    m_dmaActive = true;
    m_currentJob = 0;
    beginJob(0);
}

// ---------------------------------------------------------------------------
// Begin executing a specific job in the chain
// ---------------------------------------------------------------------------

void F018bDmaDevice::beginJob(size_t jobIdx) {
    if (jobIdx >= m_jobs.size()) {
        m_dmaActive = false;
        return;
    }

    const DmaJob& job = m_jobs[jobIdx];
    m_currentJob = jobIdx;
    m_bytesRemaining = job.count;
    m_currentOp = static_cast<DmaOperation>(job.commandLsb & 0x03);

    // Extend 20-bit addresses to 28-bit using megabyte from enhanced options
    // or falling back to the $D704 register
    uint32_t srcMB = m_enhancedMode && job.srcMB ? (uint32_t)job.srcMB << 20
                                                 : (uint32_t)m_regs[0x04] << 20;
    uint32_t dstMB = m_enhancedMode && job.dstMB ? (uint32_t)job.dstMB << 20
                                                 : (uint32_t)m_regs[0x04] << 20;
    m_srcBase = (job.srcAddr & 0x0FFFFF) | srcMB;
    m_dstBase = (job.dstAddr & 0x0FFFFF) | dstMB;

    m_srcStep = job.srcSkipRate ? job.srcSkipRate : 0x0100;
    m_dstStep = job.dstSkipRate ? job.dstSkipRate : 0x0100;
    m_fillByte = job.srcAddr & 0xFF;  // For fill ops

    // Overlap detection for copy: go backward if dst is within src range
    m_backward = false;
    if (m_currentOp == DMA_COPY && m_bytesRemaining > 0) {
        uint32_t srcEnd = m_srcBase + ((static_cast<uint32_t>(m_bytesRemaining - 1) * m_srcStep) >> 8);
        if (m_dstBase > m_srcBase && m_dstBase <= srcEnd) {
            m_backward = true;
        }
    }

    if (m_backward) {
        m_srcAccum = static_cast<uint32_t>(m_bytesRemaining - 1) * m_srcStep;
        m_dstAccum = static_cast<uint32_t>(m_bytesRemaining - 1) * m_dstStep;
    } else {
        m_srcAccum = 0;
        m_dstAccum = 0;
    }

    if (m_bytesRemaining == 0) {
        // Zero-length job: advance to next
        if (m_currentJob + 1 < m_jobs.size()) {
            beginJob(m_currentJob + 1);
        } else {
            m_dmaActive = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Process one byte of the current DMA operation
// ---------------------------------------------------------------------------

void F018bDmaDevice::tickOneByte() {
    if (!m_bus || m_bytesRemaining == 0) {
        m_dmaActive = false;
        return;
    }

    uint32_t srcAddr = m_srcBase + (m_srcAccum >> 8);
    uint32_t dstAddr = m_dstBase + (m_dstAccum >> 8);

    switch (m_currentOp) {
        case DMA_COPY: {
            uint8_t byte = m_bus->read8(srcAddr);
            m_bus->write8(dstAddr, byte);
            break;
        }
        case DMA_FILL: {
            m_bus->write8(dstAddr, m_fillByte);
            break;
        }
        case DMA_SWAP: {
            uint8_t srcByte = m_bus->read8(srcAddr);
            uint8_t dstByte = m_bus->read8(dstAddr);
            m_bus->write8(srcAddr, dstByte);
            m_bus->write8(dstAddr, srcByte);
            break;
        }
        case DMA_MIX:
            // Mix operation not yet implemented
            break;
    }

    if (m_backward) {
        m_srcAccum -= m_srcStep;
        m_dstAccum -= m_dstStep;
    } else {
        m_srcAccum += m_srcStep;
        m_dstAccum += m_dstStep;
    }

    m_bytesRemaining--;

    if (m_bytesRemaining == 0) {
        // Current job done — advance to next in chain
        if (m_currentJob + 1 < m_jobs.size()) {
            beginJob(m_currentJob + 1);
        } else {
            m_dmaActive = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Job list parsing (unchanged from original)
// ---------------------------------------------------------------------------

void F018bDmaDevice::parseJobOptions(uint32_t& addr, DmaJob& job) {
    if (!m_bus) return;

    while (true) {
        uint8_t option = m_bus->read8(addr);
        addr++;

        if (option == 0x00) break;

        if (option & 0x80) {
            // Options $80-$FF: parameterized (consume one argument byte)
            uint8_t arg = m_bus->read8(addr);
            addr++;

            switch (option) {
                case 0x80: job.srcMB = arg; break;        // Source megabyte
                case 0x81: job.dstMB = arg; break;        // Destination megabyte
                case 0x82: job.srcSkipRate = (job.srcSkipRate & 0xFF00) | arg; break;
                case 0x83: job.srcSkipRate = (job.srcSkipRate & 0x00FF) | (arg << 8); break;
                case 0x84: job.dstSkipRate = (job.dstSkipRate & 0xFF00) | arg; break;
                case 0x85: job.dstSkipRate = (job.dstSkipRate & 0x00FF) | (arg << 8); break;
                case 0x86: /* transparent byte value — not yet used */ break;
                default:   break;
            }
        } else {
            // Options $01-$7F: single-byte flags (no argument)
            switch (option) {
                case 0x06: /* disable transparency */ break;
                case 0x07: /* enable transparency */ break;
                case 0x0A: job.useF018A = true; break;    // Use F018A revision
                case 0x0B: job.useF018A = false; break;   // Use F018B revision
                default:   break;
            }
        }
    }
}

bool F018bDmaDevice::fetchJobList(uint32_t listAddr) {
    if (!m_bus) return false;

    const uint32_t MAX_JOBS = 256;
    uint32_t currentAddr = listAddr;

    bool f018b_default = (m_regs[0x03] & 0x01) != 0;

    for (uint32_t jobIdx = 0; jobIdx < MAX_JOBS; ++jobIdx) {
        DmaJob job = {};
        job.srcSkipRate = 0x0100;
        job.dstSkipRate = 0x0100;

        if (m_enhancedMode) {
            parseJobOptions(currentAddr, job);
        }

        job.commandLsb = m_bus->read8(currentAddr + 0);

        uint8_t count_lo = m_bus->read8(currentAddr + 1);
        uint8_t count_hi = m_bus->read8(currentAddr + 2);
        job.count = count_lo | (count_hi << 8);

        uint8_t src_lo = m_bus->read8(currentAddr + 3);
        uint8_t src_mid = m_bus->read8(currentAddr + 4);
        uint8_t src_bankflags = m_bus->read8(currentAddr + 5);
        job.srcAddr = src_lo | (src_mid << 8) | ((src_bankflags & 0x0F) << 16);
        job.srcFlags = (src_bankflags >> 4) & 0x0F;

        uint8_t dst_lo = m_bus->read8(currentAddr + 6);
        uint8_t dst_mid = m_bus->read8(currentAddr + 7);
        uint8_t dst_bankflags = m_bus->read8(currentAddr + 8);
        job.dstAddr = dst_lo | (dst_mid << 8) | ((dst_bankflags & 0x0F) << 16);
        job.dstFlags = (dst_bankflags >> 4) & 0x0F;

        // Per-job revision: enhanced option $0A/$0B overrides register $D703
        bool f018b = job.useF018A ? false : f018b_default;
        uint32_t jobSize = f018b ? 12 : 11;

        if (f018b) {
            job.commandMsb = m_bus->read8(currentAddr + 9);
            uint8_t mod_lo = m_bus->read8(currentAddr + 10);
            uint8_t mod_hi = m_bus->read8(currentAddr + 11);
            job.modulo = mod_lo | (mod_hi << 8);
        } else {
            job.commandMsb = 0x00;
            uint8_t mod_lo = m_bus->read8(currentAddr + 9);
            uint8_t mod_hi = m_bus->read8(currentAddr + 10);
            job.modulo = mod_lo | (mod_hi << 8);
        }

        m_jobs.push_back(job);

        bool hasChain = (job.commandLsb & 0x04) != 0;
        if (!hasChain) break;

        currentAddr += jobSize;
    }

    return !m_jobs.empty();
}
