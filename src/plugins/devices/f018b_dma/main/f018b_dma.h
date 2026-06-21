#pragma once
#include "libdevices/main/io_handler.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/device_info.h"
#include <cstring>
#include <string>
#include <vector>

/**
 * F018B DMA Controller for MEGA65
 *
 * Register map (base $D700, 16 bytes):
 *
 *   $D700        ADDRLSBTRIG  — DMA list address LSB; write triggers C65-compat DMA
 *   $D701        ADDRMSB      — DMA list address high byte (bits 15:8), no trigger
 *   $D702        ADDRBANK     — DMA list address bank (bits 22:16), no trigger
 *                               Writing resets $D704 (ADDRMB) to zero.
 *   $D703        EN018B (bit 0), NOMBWRAP (bit 1) — control flags, no trigger
 *   $D704        ADDRMB       — DMA list upper address (bits 27:20), no trigger
 *   $D705        ETRIG        — Enhanced DMA trigger (28-bit flat address)
 *   $D706        ETRIGMAPD    — Enhanced DMA trigger (MAP'd 16-bit address)
 *   $D707-$D70F  Reserved
 *
 * F018 DMA job list format (11 bytes per job, EN018B=0):
 *   Byte 0:      Command LSB (bits 1:0 = operation, bit 2 = chain)
 *   Bytes 1-2:   Count (16-bit LE)
 *   Bytes 3-4:   Source address LSB/MSB
 *   Byte 5:      Source address BANK and FLAGS
 *   Bytes 6-7:   Destination address LSB/MSB
 *   Byte 8:      Destination address BANK and FLAGS
 *   Bytes 9-10:  Modulo (16-bit LE)
 *
 * F018B DMA job list format (12 bytes per job, EN018B=1):
 *   Bytes 0-8:   Same as F018
 *   Byte 9:      Command MSB (addressing modes, reserved)
 *   Bytes 10-11: Modulo (16-bit LE)
 *
 * Execution model: DMA processes one byte per tick() call (cycle-by-cycle).
 * Jobs are read one at a time: read→execute→if chained, read next from
 * current list address. This matches gs4510.vhdl behavior.
 * While active, isHaltRequested() returns true to halt the CPU.
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
    void setIoRegistry(IORegistry* io) { m_ioRegistry = io; }
    bool isHaltRequested() const override { return m_dmaActive; }

    void reset() override;
    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override;
    void tick(uint64_t cycles) override;
    void getDeviceInfo(DeviceInfo& out) const override;

    std::vector<std::string> deviceAliases() const override { return {"DMA", "F018B", "F018BDMA", "DMAGIC"}; }
    std::vector<std::pair<std::string, uint32_t>> getDerivedValues() const override {
        return {
            {"LISTADDR", m_dmaListAddr},
            {"ACTIVE",   m_dmaActive ? 1u : 0u},
            {"ENHANCED", m_enhancedMode ? 1u : 0u},
        };
    }

private:
    enum DmaOperation {
        DMA_COPY = 0,
        DMA_MIX  = 1,
        DMA_SWAP = 2,
	DMA_FILL = 3
    };

    struct DmaJob {
        uint8_t commandLsb;     // Byte 0: operation (1:0), chain (2), yield (3), minterm (7:4)
        uint8_t commandMsb;     // F018B byte 9: src addr mode (9:8), dst addr mode (11:10)
        uint16_t count;         // Bytes 1-2: transfer size
        uint32_t srcAddr;       // Bytes 3-5: source address (20-bit: 16-bit addr + 4-bit bank)
        uint8_t srcFlags;       // Upper nibble of source BANK+FLAGS byte
        uint32_t dstAddr;       // Bytes 6-8: dest address (20-bit: 16-bit addr + 4-bit bank)
        uint8_t dstFlags;       // Upper nibble of dest BANK+FLAGS byte
        uint16_t modulo;        // Modulo value (16-bit LE)
        uint16_t srcSkipRate;   // Source fractional step rate ($0100 = 1.0 byte)
        uint16_t dstSkipRate;   // Destination fractional step rate
        uint8_t srcMB;          // Enhanced: source megabyte (option $80)
        uint8_t dstMB;          // Enhanced: destination megabyte (option $81)
        bool srcMBset;          // True if srcMB was explicitly set via option $80
        bool dstMBset;          // True if dstMB was explicitly set via option $81
        bool useF018A;          // Enhanced: per-job F018A revision flag (option $0A)
    };

    // Read one job from m_dmaListAddr, set up execution state.
    // Returns false if the job is unsupported (swap/mix).
    void startDma();
    bool fetchAndBeginNextJob();
    void parseJobOptions(uint32_t& addr, DmaJob& job);
    void syncListAddrToRegs();

    // Per-byte tick
    void tickOneByte();

    uint8_t dmaRead(uint32_t addr, bool ioFlag);
    void dmaWrite(uint32_t addr, uint8_t val, bool ioFlag);

    uint32_t m_base;
    std::string m_name{"F018B DMA"};
    IBus* m_bus;
    IORegistry* m_ioRegistry = nullptr;
    uint8_t m_regs[16];         // Register shadow: $D700–$D70F
    uint32_t m_dmaListAddr;     // 28-bit pointer into job list (advances as bytes are read)
    bool m_dmaActive;           // Set during execution; CPU halts while true
    bool m_enhancedMode;        // Enhanced DMA Jobs mode (triggered via $D705)

    // Current job state
    bool     m_hasChain;        // Current job's chain bit — read next job when done
    uint32_t m_bytesRemaining;  // Bytes left in current job (count 0 = 65536)
    uint32_t m_srcAccum;        // Source address accumulator (fractional, in 256ths)
    uint32_t m_dstAccum;        // Dest address accumulator (fractional, in 256ths)
    uint32_t m_srcBase;         // Physical source base address for current job
    uint32_t m_dstBase;         // Physical dest base address for current job
    uint16_t m_srcStep;         // Source step rate for current job
    uint16_t m_dstStep;         // Dest step rate for current job
    uint8_t  m_fillByte;        // Fill byte for fill operations
    DmaOperation m_currentOp;   // Current operation type
    bool     m_srcDir;          // Source direction: false=forward, true=backward
    bool     m_dstDir;          // Dest direction: false=forward, true=backward
    bool     m_srcHold;         // Source hold: address doesn't change
    bool     m_dstHold;         // Dest hold: address doesn't change
    bool     m_srcIo;           // Source I/O visibility ($D000-$DFFF mapped)
    bool     m_dstIo;           // Dest I/O visibility ($D000-$DFFF mapped)
    bool     m_srcModulo;       // Source uses modulo addressing
    bool     m_dstModulo;       // Dest uses modulo addressing

    // Modulo mode state
    uint16_t m_moduloValue;     // Value added to address at end of each row
    uint16_t m_colLimit;        // Columns per row (from count LSB, 0=256)
    uint16_t m_rowLimit;        // Number of rows (from count MSB, 0=256)
    uint16_t m_colCounter;      // Current column position
    uint16_t m_rowCounter;      // Current row position
    bool     m_moduloActive;    // Modulo counting is in use for this job

    // Transparency
    uint16_t m_transparency;    // Low byte = transparent value; bit 8 set = disabled
    uint8_t  m_transparencyVal; // The transparent byte value (enhanced option $86)

    // Inherited enhanced options across chained jobs
    uint8_t  m_inheritSrcMB;    bool m_inheritSrcMBset;
    uint8_t  m_inheritDstMB;    bool m_inheritDstMBset;
};
