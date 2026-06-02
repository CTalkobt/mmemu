#pragma once
#include "libdevices/main/io_handler.h"
#include "libdevices/main/device_info.h"
#include <cstring>
#include <string>
#include <vector>

/**
 * F018B DMA Controller for MEGA65
 *
 * Register map (base $D700, 16 bytes):
 *
 *   $D700        ADDRLSBTRIG  — DMA list address low byte; write triggers DMA
 *   $D701        ADDRMSHTRIG  — DMA list address high byte; write triggers DMA
 *   $D702        ADDRBANKTRIG — DMA list address bank (bits 19:16); write triggers DMA
 *   $D703        EN018B (bit 0), NOMBWRAP (bit 1) — control flags, NO trigger
 *   $D704        ADDRMB       — DMA list upper address (bits 27:20), no trigger
 *   $D705        ETRIG        — Enhanced DMA trigger; write triggers enhanced DMA
 *   $D706-$D70F  Reserved
 *
 * F018 DMA job list format (11 bytes per job):
 *   Byte 0:      Command (bits 1:0 = operation, bit 2 = chain)
 *   Bytes 1-2:   Count (16-bit LE)
 *   Bytes 3-5:   Source address (24-bit LE)
 *   Bytes 6-8:   Destination address (24-bit LE)
 *   Byte 9:      Sub-command / modulo control
 *   Byte 10:     Modulo value
 */
class F018bDmaDevice : public IOHandler {
public:
    explicit F018bDmaDevice(uint32_t base = 0xD700);
    virtual ~F018bDmaDevice() = default;

    const char* name() const override { return m_name.c_str(); }
    uint32_t    baseAddr() const override { return m_base; }
    uint32_t    addrMask() const override { return 0x0F; }  // 16 bytes

    void setName(const std::string& n) override { m_name = n; }
    void setBaseAddr(uint32_t a) override { m_base = a; }
    void setDmaBus(IBus* bus) override { m_bus = bus; }
    bool isHaltRequested() const override { return m_dmaActive; }

    void reset() override;
    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override;
    void tick(uint64_t cycles) override;
    void getDeviceInfo(DeviceInfo& out) const override;

private:
    enum DmaOperation {
        DMA_COPY = 0,
        DMA_FILL = 1,
        DMA_SWAP = 2,
        DMA_MIX  = 3
    };

    struct DmaJob {
        uint8_t command;        // Byte 0: operation, chain, interrupt, enhanced flags
        uint16_t count;         // Bytes 1-2: transfer size
        uint32_t srcAddr;       // Bytes 3-5: source address (24-bit)
        uint32_t dstAddr;       // Bytes 6-8: destination address (24-bit)
        uint8_t subCommand;     // Byte 9: sub-command / modulo control
        uint8_t modulo;         // Byte 10: modulo value
        uint16_t srcSkipRate;   // Source fractional step rate ($0100 = 1.0 byte)
        uint16_t dstSkipRate;   // Destination fractional step rate
    };

    void executeDma();
    bool fetchJobList(uint32_t listAddr);
    void parseJobOptions(uint32_t& addr, DmaJob& job);
    void processJob(const DmaJob& job);
    void doCopy(uint32_t src, uint32_t dst, uint16_t count, uint16_t srcStep, uint16_t dstStep);
    void doFill(uint32_t dst, uint16_t count, uint8_t fillByte, uint16_t dstStep);
    void doSwap(uint32_t src, uint32_t dst, uint16_t count, uint16_t srcStep, uint16_t dstStep);

    uint32_t m_base;
    std::string m_name{"F018B DMA"};
    IBus* m_bus;
    uint8_t m_regs[16];         // Register shadow: $D700–$D70F
    uint32_t m_dmaListAddr;     // 28-bit pointer to job list
    bool m_dmaActive;           // Set during execution; CPU halts while true
    bool m_enhancedMode;        // Enhanced DMA Jobs mode (triggered via $D705)
    std::vector<DmaJob> m_jobs; // Fetched job chain
};
