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
      m_srcHold(false), m_dstHold(false), m_srcIo(false), m_dstIo(false),
      m_srcModulo(false), m_dstModulo(false),
      m_moduloValue(0), m_colLimit(0), m_rowLimit(0),
      m_colCounter(0), m_rowCounter(0), m_moduloActive(false),
      m_transparency(0x100), m_transparencyVal(0),
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
    m_transparency = 0x100; // disabled by default
    m_transparencyVal = 0;
    m_inheritSrcMB = 0; m_inheritSrcMBset = false;
    m_inheritDstMB = 0; m_inheritDstMBset = false;
    memset(&m_srcLine, 0, sizeof(m_srcLine));
    memset(&m_dstLine, 0, sizeof(m_dstLine));

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
    // Per gs4510.vhdl: count of 0 means 65536 bytes
    m_bytesRemaining = job.count ? job.count : 0x10000;
    m_currentOp = static_cast<DmaOperation>(job.commandLsb & 0x03);

    // Extend 20-bit addresses to 28-bit using megabyte register.
    // Per gs4510.vhdl DMAgicGetReady:
    //   F018B: addr(27:20) = (reg_src_mb + bank_byte(6:4)) & 0xFF, addr(19:16) = bank_byte(3:0)
    //   F018A: addr(27:20) = reg_src_mb,                            addr(19:16) = bank_byte(3:0)
    // In F018A, bank_byte bits 6:4 are direction/modulo/hold flags, not address bits.
    uint32_t srcMB, dstMB;
    if (f018b) {
        srcMB = (uint32_t)((m_inheritSrcMB + (job.srcFlags & 0x07)) & 0xFF) << 20;
        dstMB = (uint32_t)((m_inheritDstMB + (job.dstFlags & 0x07)) & 0xFF) << 20;
    } else {
        srcMB = (uint32_t)m_inheritSrcMB << 20;
        dstMB = (uint32_t)m_inheritDstMB << 20;
    }
    m_srcBase = (job.srcAddr & 0x0FFFFF) | srcMB;
    m_dstBase = (job.dstAddr & 0x0FFFFF) | dstMB;

    m_srcStep = job.srcSkipRate ? job.srcSkipRate : 0x0100;
    m_dstStep = job.dstSkipRate ? job.dstSkipRate : 0x0100;
    m_fillByte = job.srcAddr & 0xFF;  // For fill ops

    // I/O visibility — bank byte bit 7 (bit 3 of srcFlags/dstFlags)
    m_srcIo = (job.srcFlags & 0x08) != 0;
    m_dstIo = (job.dstFlags & 0x08) != 0;

    // Direction, hold, and modulo — per gs4510.vhdl lines 5946-5965:
    //   F018B: direction from command LSB bits 4-5, hold/modulo from subcommand individual bits
    //   F018A: direction/modulo/hold from bank byte bits 6/5/4
    if (f018b) {
        m_srcDir    = (job.commandLsb & 0x10) != 0;  // command bit 4
        m_dstDir    = (job.commandLsb & 0x20) != 0;  // command bit 5
        m_srcHold   = (job.commandMsb & 0x02) != 0;  // subcommand bit 1
        m_dstHold   = (job.commandMsb & 0x08) != 0;  // subcommand bit 3
        m_srcModulo = (job.commandMsb & 0x01) != 0;  // subcommand bit 0
        m_dstModulo = (job.commandMsb & 0x04) != 0;  // subcommand bit 2
    } else {
        m_srcDir    = (job.srcFlags & 0x04) != 0;    // bank byte bit 6
        m_dstDir    = (job.dstFlags & 0x04) != 0;    // bank byte bit 6
        m_srcHold   = (job.srcFlags & 0x01) != 0;    // bank byte bit 4
        m_dstHold   = (job.dstFlags & 0x01) != 0;    // bank byte bit 4
        m_srcModulo = (job.srcFlags & 0x02) != 0;    // bank byte bit 5
        m_dstModulo = (job.dstFlags & 0x02) != 0;    // bank byte bit 5
    }

    // MIX minterms — 4 boolean masks derived from command/subcommand bits 4-7.
    // F018A: bits 4-7 of command byte; F018B: bits 4-7 of subcommand byte.
    if (m_currentOp == DMA_MIX) {
        uint8_t mintermBits = f018b ? job.commandMsb : job.commandLsb;
        m_minterms[0] = (mintermBits & 0x10) ? 0xFF : 0x00;  // ~src & ~dst
        m_minterms[1] = (mintermBits & 0x20) ? 0xFF : 0x00;  // ~src &  dst
        m_minterms[2] = (mintermBits & 0x40) ? 0xFF : 0x00;  //  src & ~dst
        m_minterms[3] = (mintermBits & 0x80) ? 0xFF : 0x00;  //  src &  dst
    }

    // NOTE: Real MEGA65 hardware does NOT generate IRQs from DMAgic.
    // Command bit 3 is reserved/unused. Hypervisor handles interrupt scheduling separately.
    // m_irqOnDone = (job.commandLsb & 0x08) != 0;  // DISABLED per issue #101

    // Modulo mode: count LSB = columns, count MSB = rows, modulo value from job
    m_moduloActive = (m_srcModulo || m_dstModulo);
    if (m_moduloActive) {
        m_colLimit = (job.count & 0xFF) ? (job.count & 0xFF) : 256;
        m_rowLimit = (job.count >> 8) ? (job.count >> 8) : 256;
        m_colCounter = 0;
        m_rowCounter = 0;
        m_moduloValue = job.modulo;
        // Override bytesRemaining — modulo uses its own counters
        m_bytesRemaining = (uint32_t)m_colLimit * m_rowLimit;
    }

    // Set up transparency for this job
    m_transparency = (m_transparency & 0x100) | m_transparencyVal;

    m_srcAccum = 0;
    m_dstAccum = 0;

    // Zero-length job: chain immediately if possible.
    // Real hardware has no chain depth limit; we trust that valid programs don't have
    // circular chains. If a circular chain is encountered, it will simply consume cycles
    // until interrupted by an external event or the program is forcibly terminated.
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

uint8_t F018bDmaDevice::dmaRead(uint32_t addr, bool ioFlag) {
    // When I/O flag is set and address is in $xDxxx range (lower 16 bits),
    // route through IORegistry for $D000-$DFFF I/O register access.
    if (ioFlag && m_ioRegistry && ((addr & 0xF000) == 0xD000)) {
        uint8_t val = 0xFF;
        uint32_t ioAddr = addr & 0xFFFF;
        if (m_ioRegistry->dispatchRead(m_bus, ioAddr, &val))
            return val;
    }
    return m_bus->read8(addr);
}

void F018bDmaDevice::dmaWrite(uint32_t addr, uint8_t val, bool ioFlag) {
    if (ioFlag && m_ioRegistry && ((addr & 0xF000) == 0xD000)) {
        uint32_t ioAddr = addr & 0xFFFF;
        if (m_ioRegistry->dispatchWrite(m_bus, ioAddr, val))
            return;
    }
    m_bus->write8(addr, val);
}

// Address stepping: normal mode or line drawing mode.
// In line mode (slopeType bit 7 set), uses Bresenham-like slope accumulator
// to trace lines across VIC-IV 8×8 card-based pixel addresses.
// Based on xemu's dma65.c address_stepping() (lines 180-219).
void F018bDmaDevice::stepAddress(uint32_t& accum, uint32_t base,
                                  uint16_t step, bool hold, bool dir,
                                  LineMode& lm) {
    if (hold) return;

    if (!(lm.slopeType & 0x80)) {
        // Normal addressing: fixed-point accumulator step
        if (dir) accum -= step;
        else     accum += step;
        return;
    }

    // Line Drawing Mode (LDM)
    // addr in accum is fixed-point (low 8 bits = fractional).
    // X position within cards: bits [10:8] of (accum >> 8)
    // Y position: stepping by ±8 pixels (±0x800 in fixed-point)
    lm.slopeAccum += lm.slope;

    if (lm.slopeType & 0x40) {
        // Y is major axis: always step Y, conditionally step X on overflow
        accum += 0x800;  // +8 rows (Y step)
        // Check Y card boundary (bits [14:11])
        if ((accum & (7u << (3 + 8))) == (7u << (3 + 8)))
            accum += lm.yCol;

        if (lm.slopeAccum >= 0x10000) {
            lm.slopeAccum -= 0x10000;
            // Minor axis (X) step
            if (lm.slopeType & 0x20) {
                // Negative X
                accum -= ((accum & 0x700) == 0) ? lm.xCol + 0x100 : 0x100;
            } else {
                // Positive X
                accum += ((accum & 0x700) == 0x700) ? lm.xCol + 0x100 : 0x100;
            }
        }
    } else {
        // X is major axis: always step X, conditionally step Y on overflow
        accum += ((accum & 0x700) == 0x700) ? lm.xCol + 0x100 : 0x100;

        if (lm.slopeAccum >= 0x10000) {
            lm.slopeAccum -= 0x10000;
            // Minor axis (Y) step
            accum += (lm.slopeType & 0x20) ? (uint32_t)-0x800 : 0x800u;
        }
    }
}

void F018bDmaDevice::tickOneByte() {
    if (!m_bus || m_bytesRemaining == 0) {
        m_dmaActive = false;
        return;
    }

    uint32_t srcAddr = m_srcBase + (m_srcAccum >> 8);
    uint32_t dstAddr = m_dstBase + (m_dstAccum >> 8);

    switch (m_currentOp) {
        case DMA_COPY: {
            uint8_t byte = dmaRead(srcAddr, m_srcIo);
            // Transparency: skip write if byte matches transparent value
            if ((unsigned)byte != m_transparency)
                dmaWrite(dstAddr, byte, m_dstIo);
            break;
        }
        case DMA_FILL: {
            dmaWrite(dstAddr, m_fillByte, m_dstIo);
            break;
        }
        case DMA_SWAP: {
            uint8_t s = dmaRead(srcAddr, m_srcIo);
            uint8_t d = dmaRead(dstAddr, m_dstIo);
            dmaWrite(dstAddr, s, m_dstIo);
            dmaWrite(srcAddr, d, m_srcIo);
            break;
        }
        case DMA_MIX: {
            // MINTERM: combine source and dest using 4 boolean masks
            uint8_t s = dmaRead(srcAddr, m_srcIo);
            uint8_t d = dmaRead(dstAddr, m_dstIo);
            uint8_t result = (( s) & ( d) & m_minterms[3])
                           | (( s) & (~d) & m_minterms[2])
                           | ((~s) & ( d) & m_minterms[1])
                           | ((~s) & (~d) & m_minterms[0]);
            dmaWrite(dstAddr, result, m_dstIo);
            break;
        }
    }

    // Advance addresses (normal or line drawing mode)
    if (!m_srcHold)
        stepAddress(m_srcAccum, m_srcBase, m_srcStep, m_srcHold, m_srcDir, m_srcLine);
    if (!m_dstHold)
        stepAddress(m_dstAccum, m_dstBase, m_dstStep, m_dstHold, m_dstDir, m_dstLine);

    // Modulo addressing: at end of each row, add modulo value to affected channels
    if (m_moduloActive) {
        m_colCounter++;
        if (m_colCounter >= m_colLimit) {
            m_colCounter = 0;
            m_rowCounter++;
            if (m_rowCounter >= m_rowLimit) {
                m_bytesRemaining = 0;
            } else {
                // Add modulo value (as fixed-point) to channels with modulo enabled
                if (m_srcModulo) m_srcAccum += (uint32_t)m_moduloValue << 8;
                if (m_dstModulo) m_dstAccum += (uint32_t)m_moduloValue << 8;
            }
        }
    }

    if (m_bytesRemaining > 0) m_bytesRemaining--;

    if (m_bytesRemaining == 0) {
        // NOTE: Real MEGA65 hardware does NOT generate IRQs from DMAgic completion.
        // The hypervisor handles interrupt scheduling separately.
        // If an external agent needs to know DMA is done, they check isHaltRequested().

        // Current job done — if chain bit was set, read next job from list
        if (m_hasChain) {
            if (!fetchAndBeginNextJob()) {
                m_dmaActive = false;
            }
        } else {
            // Last job in chain: reset enhanced options (dmagic_reset_options)
            m_inheritSrcMB = 0; m_inheritSrcMBset = false;
            m_inheritDstMB = 0; m_inheritDstMBset = false;
            m_dmaActive = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Enhanced DMA option parsing
// ---------------------------------------------------------------------------

void F018bDmaDevice::parseJobOptions(uint32_t& addr, DmaJob& job) {
    if (!m_bus) return;

    // Parse enhanced DMA options. Limit to 128 to prevent infinite loop
    // if memory has no $00 terminator.
    for (int optCount = 0; optCount < 128; ++optCount) {
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
                case 0x86: m_transparencyVal = arg; break;
                // Line Drawing Mode — destination
                case 0x87: m_dstLine.xCol = (m_dstLine.xCol & 0xFF0000) | ((uint32_t)arg << 8); break;
                case 0x88: m_dstLine.xCol = (m_dstLine.xCol & 0x00FF00) | ((uint32_t)arg << 16); break;
                case 0x89: m_dstLine.yCol = (m_dstLine.yCol & 0xFF0000) | ((uint32_t)arg << 8); break;
                case 0x8A: m_dstLine.yCol = (m_dstLine.yCol & 0x00FF00) | ((uint32_t)arg << 16); break;
                case 0x8B: m_dstLine.slope = (m_dstLine.slope & 0xFF00) | arg; break;
                case 0x8C: m_dstLine.slope = (m_dstLine.slope & 0x00FF) | ((uint16_t)arg << 8); break;
                case 0x8D: m_dstLine.slopeAccum = (m_dstLine.slopeAccum & 0xFF00) | arg; break;
                case 0x8E: m_dstLine.slopeAccum = (m_dstLine.slopeAccum & 0x00FF) | ((uint16_t)arg << 8); break;
                case 0x8F: m_dstLine.slopeType = arg; break;
                // Line Drawing Mode — source
                case 0x97: m_srcLine.xCol = (m_srcLine.xCol & 0xFF0000) | ((uint32_t)arg << 8); break;
                case 0x98: m_srcLine.xCol = (m_srcLine.xCol & 0x00FF00) | ((uint32_t)arg << 16); break;
                case 0x99: m_srcLine.yCol = (m_srcLine.yCol & 0xFF0000) | ((uint32_t)arg << 8); break;
                case 0x9A: m_srcLine.yCol = (m_srcLine.yCol & 0x00FF00) | ((uint32_t)arg << 16); break;
                case 0x9B: m_srcLine.slope = (m_srcLine.slope & 0xFF00) | arg; break;
                case 0x9C: m_srcLine.slope = (m_srcLine.slope & 0x00FF) | ((uint16_t)arg << 8); break;
                case 0x9D: m_srcLine.slopeAccum = (m_srcLine.slopeAccum & 0xFF00) | arg; break;
                case 0x9E: m_srcLine.slopeAccum = (m_srcLine.slopeAccum & 0x00FF) | ((uint16_t)arg << 8); break;
                case 0x9F: m_srcLine.slopeType = arg; break;
                default:   break;
            }
        } else {
            // Options $01-$7F: single-byte flags (no argument)
            switch (option) {
                case 0x06: m_transparency |= 0x100; break;   // disable transparency
                case 0x07: m_transparency &= 0xFF; break;    // enable transparency
                case 0x0A: job.useF018A = true; break;    // Use F018A revision
                case 0x0B: job.useF018A = false; break;   // Use F018B revision

                // Unimplemented modes (recognized but not yet functional)
                case 0x0D:  // Floppy mode LSB
                case 0x0E:  // Floppy mode MSB
                case 0x0F:  // Floppy mode high
                    // TODO: Implement floppy disk controller mode
                    // Transfers raw flux data to/from floppy drive
                    break;

                case 0x10:  // SID mode
                    // TODO: Implement SID sample playback mode
                    // Allows direct SID register access for sample playback
                    break;

                case 0x53:  // Spiral mode
                    // TODO: Implement Shallan Spiral pattern drawing
                    // Draws spirals by updating destination address in spiral pattern
                    break;

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
