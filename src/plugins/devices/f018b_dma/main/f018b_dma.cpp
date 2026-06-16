#include "f018b_dma.h"
#include "libmem/main/ibus.h"
#include "include/mmemu_plugin_api.h"
#include <cstring>
#include <iomanip>
#include <iostream>

F018bDmaDevice::F018bDmaDevice(uint32_t base)
    : m_base(base), m_bus(nullptr), m_dmaListAddr(0), m_dmaActive(false),
      m_enhancedMode(false), m_hasChain(false), m_bytesRemaining(0),
      m_srcAccum(0), m_dstAccum(0), m_srcBase(0), m_dstBase(0),
      m_srcStep(0x0100), m_dstStep(0x0100), m_fillByte(0),
      m_currentOp(DMA_COPY), m_srcDir(false), m_dstDir(false),
      m_inheritSrcMB(0), m_inheritSrcMBset(false),
      m_inheritDstMB(0), m_inheritDstMBset(false) {
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

    out.dependencies.push_back({"DMA Bus", m_bus ? "connected" : "none"});
}

void F018bDmaDevice::reset() {
    std::memset(m_regs, 0, sizeof(m_regs));
    m_dmaListAddr = 0;
    m_dmaActive = false;
    m_hasChain = false;
    m_bytesRemaining = 0;
    m_inheritSrcMB = 0; m_inheritSrcMBset = false;
    m_inheritDstMB = 0; m_inheritDstMBset = false;
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
    if (!m_bus) m_bus = bus;  // Use explicit DMA bus if set, fall back to I/O bus

    // $D702: ADDRBANK clears ADDRMB's upper 5 bits, and updates the lower bits of ADDRMB
    //  to match ADDRBANK's upper 3 bits, since on HW they share a register.
    if (offset == 0x02) {
        m_regs[0x04] = (val & 0x70) >> 4;
    }

    // $D704: ADDRMB should also set the upper 3 bits of ADDRBANK, since on HW they share a register.
    if (offset == 0x04) {
        m_regs[0x02] = (m_regs[0x02] & 0x8F) | ((val & 0x07) << 4);
    }

    // $D700: ADDRLSBTRIG — write triggers C65-compatible DMA execution, clears ADDRMB.7-3
    if (offset == 0x00) {
        m_enhancedMode = false;
        m_regs[0x04] &= 0x07;
        m_regs[0x05] = val; // update ETRIG's value to match
        startDma();
    }

    // $D705: ETRIG — write triggers Enhanced DMA (flat 28-bit address)
    else if (offset == 0x05) {
        m_enhancedMode = true;
        m_regs[0x00] = val; // update ADDRLSBTRIG's value to match
        startDma();
    }

    return true;
}

void F018bDmaDevice::tick(uint64_t /*cycles*/) {
    if (!m_dmaActive) return;
    tickOneByte();
}

// ---------------------------------------------------------------------------
// DMA initiation: assemble list address, read first job
// ---------------------------------------------------------------------------

void F018bDmaDevice::startDma() {
    if (!m_bus) return;

    // Assemble 28-bit DMA list address from registers.
    uint32_t addr_low = m_enhancedMode ? m_regs[0x05] : m_regs[0x00];
    uint32_t addr_mid = m_regs[0x01];
    uint32_t addr_bank = m_regs[0x02] & 0x7F; // exclude I/O flag
    uint32_t addr_upper = m_regs[0x04] >> 3; // drop lower 3 bits since they overlap

    m_dmaListAddr = (addr_low) | (addr_mid << 8) | (addr_bank << 16) | (addr_upper << 23);
    m_dmaListAddr &= 0x0FFFFFFF;

    // Reset inherited options for a new DMA trigger
    m_inheritSrcMB = 0; m_inheritSrcMBset = false;
    m_inheritDstMB = 0; m_inheritDstMBset = false;

    // Read and begin the first job
    if (!fetchAndBeginNextJob()) {
        m_dmaActive = false;
        return;
    }

    m_dmaActive = true;
}

// ---------------------------------------------------------------------------
// Read one job from m_dmaListAddr, advance the pointer, set up execution
// ---------------------------------------------------------------------------

bool F018bDmaDevice::fetchAndBeginNextJob() {
    if (!m_bus) return false;

    uint32_t addr = m_dmaListAddr;
    bool f018b_default = (m_regs[0x03] & 0x01) != 0;

    DmaJob job = {};
    job.srcSkipRate = 0x0100;
    job.dstSkipRate = 0x0100;
    // Inherit megabyte settings from previous jobs in the chain
    job.srcMB = m_inheritSrcMB; job.srcMBset = m_inheritSrcMBset;
    job.dstMB = m_inheritDstMB; job.dstMBset = m_inheritDstMBset;

    if (m_enhancedMode) {
        parseJobOptions(addr, job);
    }

    // Update inherited values for next chained job
    if (job.srcMBset) { m_inheritSrcMB = job.srcMB; m_inheritSrcMBset = true; }
    if (job.dstMBset) { m_inheritDstMB = job.dstMB; m_inheritDstMBset = true; }

    job.commandLsb = m_bus->read8(addr + 0);

    uint8_t count_lo = m_bus->read8(addr + 1);
    uint8_t count_hi = m_bus->read8(addr + 2);
    job.count = count_lo | (count_hi << 8);

    uint8_t src_lo = m_bus->read8(addr + 3);
    uint8_t src_mid = m_bus->read8(addr + 4);
    uint8_t src_bankflags = m_bus->read8(addr + 5);
    job.srcAddr = src_lo | (src_mid << 8) | ((src_bankflags & 0x0F) << 16);
    job.srcFlags = (src_bankflags >> 4) & 0x0F;

    uint8_t dst_lo = m_bus->read8(addr + 6);
    uint8_t dst_mid = m_bus->read8(addr + 7);
    uint8_t dst_bankflags = m_bus->read8(addr + 8);
    job.dstAddr = dst_lo | (dst_mid << 8) | ((dst_bankflags & 0x0F) << 16);
    job.dstFlags = (dst_bankflags >> 4) & 0x0F;

    // Per-job revision: enhanced option $0A/$0B overrides register $D703
    bool f018b = job.useF018A ? false : f018b_default;
    uint32_t jobSize = f018b ? 12 : 11;

    if (f018b) {
        job.commandMsb = m_bus->read8(addr + 9);
        uint8_t mod_lo = m_bus->read8(addr + 10);
        uint8_t mod_hi = m_bus->read8(addr + 11);
        job.modulo = mod_lo | (mod_hi << 8);
    } else {
        job.commandMsb = 0x00;
        uint8_t mod_lo = m_bus->read8(addr + 9);
        uint8_t mod_hi = m_bus->read8(addr + 10);
        job.modulo = mod_lo | (mod_hi << 8);
    }

    addr += jobSize;

    // Advance list address past this job
    m_dmaListAddr = addr & 0x0FFFFFFF;
    syncListAddrToRegs();

    // Store chain bit for when this job finishes
    m_hasChain = (job.commandLsb & 0x04) != 0;

    // Set up execution state from job
    m_bytesRemaining = job.count;
    m_currentOp = static_cast<DmaOperation>(job.commandLsb & 0x03);

    if (m_currentOp == DMA_MIX || m_currentOp == DMA_SWAP) {
        std::cerr << "F018B/WARN: Attempted to use unsupported DMA operation "
                  << m_currentOp << ", aborting chain\n";
        m_dmaActive = false;
        return false;
    }

    // Extend 20-bit addresses to 28-bit using megabyte from enhanced options
    // or falling back to the $D704 register
    uint32_t srcMB = (m_enhancedMode && job.srcMBset) ? (uint32_t)job.srcMB << 20
                                                      : (uint32_t)m_regs[0x04] << 20;
    uint32_t dstMB = (m_enhancedMode && job.dstMBset) ? (uint32_t)job.dstMB << 20
                                                      : (uint32_t)m_regs[0x04] << 20;
    m_srcBase = (job.srcAddr & 0x0FFFFF) | srcMB;
    m_dstBase = (job.dstAddr & 0x0FFFFF) | dstMB;

    m_srcStep = job.srcSkipRate ? job.srcSkipRate : 0x0100;
    m_dstStep = job.dstSkipRate ? job.dstSkipRate : 0x0100;
    m_fillByte = job.srcAddr & 0xFF;  // For fill ops

    // Direction from command/bank bytes — NO auto-reverse.
    // F018B: src direction = cmd bit 4, dst direction = cmd bit 5
    // F018A: src direction = src_bank bit 6, dst direction = dst_bank bit 6
    if (f018b) {
        m_srcDir = (job.commandLsb & 0x10) != 0;
        m_dstDir = (job.commandLsb & 0x20) != 0;
    } else {
        m_srcDir = (job.srcFlags & 0x04) != 0;   // srcFlags = bank>>4, bit 6 = srcFlags bit 2
        m_dstDir = (job.dstFlags & 0x04) != 0;
    }

    m_srcAccum = 0;
    m_dstAccum = 0;

    // Zero-length job: chain immediately if possible
    if (m_bytesRemaining == 0) {
        if (m_hasChain) {
            return fetchAndBeginNextJob();
        } else {
            m_dmaActive = false;
            return false;
        }
    }

    return true;
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
        case DMA_SWAP:
            break; // Swap not implemented (rejected in fetchAndBeginNextJob)
        case DMA_MIX:
            break; // Mix not implemented
    }

    if (m_srcDir) m_srcAccum -= m_srcStep;
    else          m_srcAccum += m_srcStep;

    if (m_dstDir) m_dstAccum -= m_dstStep;
    else          m_dstAccum += m_dstStep;

    m_bytesRemaining--;

    if (m_bytesRemaining == 0) {
        // Current job done — if chain bit was set, read next job from list
        if (m_hasChain) {
            if (!fetchAndBeginNextJob()) {
                m_dmaActive = false;
            }
        } else {
            m_dmaActive = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Enhanced DMA option parsing
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
                case 0x80: job.srcMB = arg; job.srcMBset = true; break;
                case 0x81: job.dstMB = arg; job.dstMBset = true; break;
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

// Sync m_dmaListAddr back to the register shadow so ioRead reflects the
// current list pointer position (matching gs4510.vhdl reg_dmagic_addr).
void F018bDmaDevice::syncListAddrToRegs() {
    m_regs[0x00] = m_dmaListAddr & 0xFF;
    m_regs[0x05] = m_dmaListAddr & 0xFF;         // ETRIG mirrors LSB
    m_regs[0x01] = (m_dmaListAddr >> 8) & 0xFF;
    // ADDRBANK holds bits 22:16 plus I/O flag in bit 7; preserve the I/O flag
    m_regs[0x02] = (m_regs[0x02] & 0x80) | ((m_dmaListAddr >> 16) & 0x7F);
    // ADDRMB holds bits 27:20; also sync shared bits with ADDRBANK
    m_regs[0x04] = (m_dmaListAddr >> 20) & 0xFF;
}
