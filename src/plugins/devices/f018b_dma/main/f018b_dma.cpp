#include "f018b_dma.h"
#include "libmem/main/ibus.h"
#include "include/mmemu_plugin_api.h"
#include <cstring>

F018bDmaDevice::F018bDmaDevice(uint32_t base)
    : m_base(base), m_bus(nullptr), m_dmaListAddr(0), m_dmaActive(false), m_enhancedMode(false) {
    std::memset(m_regs, 0, sizeof(m_regs));
}

void F018bDmaDevice::reset() {
    std::memset(m_regs, 0, sizeof(m_regs));
    m_dmaListAddr = 0;
    m_dmaActive = false;
    m_jobs.clear();
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

    // Write to $D703 triggers standard DMA execution
    if (offset == 0x03) {
        m_enhancedMode = false;
        executeDma();
    }
    // Write to $D705 triggers Enhanced DMA execution
    else if (offset == 0x05) {
        m_enhancedMode = true;
        executeDma();
    }

    return true;
}

void F018bDmaDevice::tick(uint64_t /*cycles*/) {
    // DMA execution happens synchronously in ioWrite; tick is a no-op
    // If async execution is needed in the future, execute chained jobs here
    // and clear m_dmaActive when complete.
}

void F018bDmaDevice::parseJobOptions(uint32_t& addr, DmaJob& job) {
    if (!m_bus) return;

    while (true) {
        uint8_t option = m_bus->read8(addr);
        addr++;

        // End of job option list
        if (option == 0x00) break;

        // Options with MSB set take a 1-byte argument
        if (option & 0x80) {
            uint8_t arg = m_bus->read8(addr);
            addr++;

            switch (option) {
                case 0x82:  // Source skip rate (256ths of bytes) - fractional part
                    job.srcSkipRate = (job.srcSkipRate & 0xFF00) | arg;
                    break;
                case 0x83:  // Source skip rate (whole bytes) - integer part
                    job.srcSkipRate = (job.srcSkipRate & 0x00FF) | (arg << 8);
                    break;
                case 0x84:  // Destination skip rate (256ths of bytes) - fractional part
                    job.dstSkipRate = (job.dstSkipRate & 0xFF00) | arg;
                    break;
                case 0x85:  // Destination skip rate (whole bytes) - integer part
                    job.dstSkipRate = (job.dstSkipRate & 0x00FF) | (arg << 8);
                    break;
                // Other options are ignored for now (line drawing, etc.)
                default:
                    break;
            }
        }
        // Options without MSB are no-argument tokens
    }
}

void F018bDmaDevice::executeDma() {
    if (!m_bus) {
        // Bus not available yet, will be set via ioWrite
        return;
    }

    // Assemble 28-bit DMA list address from registers
    // $D700 = low byte, $D701 = high byte, $D702 = bits 19:16, $D704 = bits 27:20
    uint32_t addr_low = m_regs[0x00];           // $D700, bits 7:0
    uint32_t addr_mid = m_regs[0x01];           // $D701, bits 15:8
    uint32_t addr_bank = m_regs[0x02];          // $D702, bits 19:16 (lower 4 bits)
    uint32_t addr_upper = m_regs[0x04];         // $D704, bits 27:20

    m_dmaListAddr = (addr_low) | (addr_mid << 8) | ((addr_bank & 0x0F) << 16) | (addr_upper << 20);
    m_dmaListAddr &= 0x0FFFFFFF;  // Mask to 28-bit address space

    // Fetch and parse job list
    m_jobs.clear();
    if (!fetchJobList(m_dmaListAddr)) {
        // Debug: job list fetch failed
        return;
    }

    // Set DMA active flag (CPU will halt via isHaltRequested())
    m_dmaActive = true;

    // Execute all chained jobs
    for (const auto& job : m_jobs) {
        processJob(job);
    }

    // Clear DMA active flag when complete
    m_dmaActive = false;
}

bool F018bDmaDevice::fetchJobList(uint32_t listAddr) {
    if (!m_bus) return false;

    // Read jobs from memory until we encounter a job without the chain bit set
    const uint32_t MAX_JOBS = 256;  // Safety limit
    uint32_t currentAddr = listAddr;

    for (uint32_t jobIdx = 0; jobIdx < MAX_JOBS; ++jobIdx) {
        DmaJob job = {};
        job.srcSkipRate = 0x0100;  // Default: 1.0 bytes per iteration
        job.dstSkipRate = 0x0100;

        // Parse job option tokens if in enhanced mode
        if (m_enhancedMode) {
            parseJobOptions(currentAddr, job);
        }

        // Read job descriptor (10 bytes for standard format)
        // Byte 0: command
        job.command = m_bus->read8(currentAddr + 0);

        // Bytes 1–2: count (16-bit little-endian)
        uint8_t count_lo = m_bus->read8(currentAddr + 1);
        uint8_t count_hi = m_bus->read8(currentAddr + 2);
        job.count = count_lo | (count_hi << 8);

        // Bytes 3–5: source address (24-bit little-endian)
        uint8_t src_lo = m_bus->read8(currentAddr + 3);
        uint8_t src_mid = m_bus->read8(currentAddr + 4);
        uint8_t src_hi = m_bus->read8(currentAddr + 5);
        job.srcAddr = src_lo | (src_mid << 8) | (src_hi << 16);

        // Bytes 6–8: destination address (24-bit little-endian)
        uint8_t dst_lo = m_bus->read8(currentAddr + 6);
        uint8_t dst_mid = m_bus->read8(currentAddr + 7);
        uint8_t dst_hi = m_bus->read8(currentAddr + 8);
        job.dstAddr = dst_lo | (dst_mid << 8) | (dst_hi << 16);

        // Byte 9: chain/end byte
        job.chainByte = m_bus->read8(currentAddr + 9);

        m_jobs.push_back(job);

        // Check chain bit (bit 2 of command)
        bool hasChain = (job.command & 0x04) != 0;
        if (!hasChain) {
            break;  // End of job chain
        }

        // Move to next job (10-byte descriptor)
        currentAddr += 10;
    }

    return !m_jobs.empty();
}

void F018bDmaDevice::processJob(const DmaJob& job) {
    if (!m_bus) return;

    uint8_t op = job.command & 0x03;  // Bits 1:0 = operation

    // Extend 24-bit addresses to 28-bit using upper bank from $D704
    uint32_t bankHigh = static_cast<uint32_t>(m_regs[0x04]) << 20;
    uint32_t srcPhys = (job.srcAddr & 0x0FFFFF) | bankHigh;
    uint32_t dstPhys = (job.dstAddr & 0x0FFFFF) | bankHigh;

    // Use default 1.0 byte stepping if not set by options
    uint16_t srcStep = job.srcSkipRate ? job.srcSkipRate : 0x0100;
    uint16_t dstStep = job.dstSkipRate ? job.dstSkipRate : 0x0100;

    switch (op) {
        case DMA_COPY:
            doCopy(srcPhys, dstPhys, job.count, srcStep, dstStep);
            break;
        case DMA_FILL:
            // Fill byte is the low byte of the source address
            doFill(dstPhys, job.count, job.srcAddr & 0xFF, dstStep);
            break;
        case DMA_SWAP:
            doSwap(srcPhys, dstPhys, job.count, srcStep, dstStep);
            break;
        case DMA_MIX:
            // Mix operation not yet implemented
            break;
    }
}

void F018bDmaDevice::doCopy(uint32_t src, uint32_t dst, uint16_t count, uint16_t srcStep, uint16_t dstStep) {
    if (!m_bus || count == 0) return;

    // srcStep and dstStep are in format: high byte = integer bytes, low byte = 256ths
    // E.g., $0100 = 1.0 bytes, $0080 = 0.5 bytes, $0200 = 2.0 bytes

    uint32_t srcAccum = 0;  // Accumulated address offset (in 256ths)
    uint32_t dstAccum = 0;

    for (uint16_t i = 0; i < count; ++i) {
        uint8_t byte = m_bus->read8(src + (srcAccum >> 8));
        m_bus->write8(dst + (dstAccum >> 8), byte);

        srcAccum += srcStep;
        dstAccum += dstStep;
    }
}

void F018bDmaDevice::doFill(uint32_t dst, uint16_t count, uint8_t fillByte, uint16_t dstStep) {
    if (!m_bus || count == 0) return;

    uint32_t dstAccum = 0;  // Accumulated address offset (in 256ths)

    for (uint16_t i = 0; i < count; ++i) {
        m_bus->write8(dst + (dstAccum >> 8), fillByte);
        dstAccum += dstStep;
    }
}

void F018bDmaDevice::doSwap(uint32_t src, uint32_t dst, uint16_t count, uint16_t srcStep, uint16_t dstStep) {
    if (!m_bus || count == 0) return;

    uint32_t srcAccum = 0;  // Accumulated address offset (in 256ths)
    uint32_t dstAccum = 0;

    for (uint16_t i = 0; i < count; ++i) {
        uint8_t srcByte = m_bus->read8(src + (srcAccum >> 8));
        uint8_t dstByte = m_bus->read8(dst + (dstAccum >> 8));
        m_bus->write8(src + (srcAccum >> 8), dstByte);
        m_bus->write8(dst + (dstAccum >> 8), srcByte);

        srcAccum += srcStep;
        dstAccum += dstStep;
    }
}

// Plugin manifest
static IOHandler* createF018bDma() {
    return new F018bDmaDevice();
}

static DevicePluginInfo s_devices[] = {
    {"f018b_dma", createF018bDma}
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "f018b_dma_plugin",
    "F018B DMA Controller",
    "1.0.0",
    nullptr, nullptr,           // deps, supportedMachineIds
    0, nullptr,                 // coreCount, cores
    0, nullptr,                 // toolchainCount, toolchains
    1, s_devices,               // deviceCount, devices
    0, nullptr,                 // machineCount, machines
    0, nullptr,                 // loaderCount, loaders
    0, nullptr                  // cartridgeCount, cartridges
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    (void)host;
    return &s_manifest;
}
