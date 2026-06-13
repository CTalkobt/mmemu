#pragma once

#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <functional>

class SparseMemoryBus;
class MapMmu;

/**
 * C64 Bank Controller for MEGA65
 *
 * Provides C64-compatible memory banking in the MEGA65's 28-bit address space.
 * Manages ROM overlay visibility on SparseMemoryBus based on the CPU I/O port
 * at $00/$01 (LORAM/HIRAM/CHAREN bits).
 *
 * Banking regions (same as C64 PLA):
 *   $A000-$BFFF  BASIC ROM     visible when HIRAM=1 && LORAM=1
 *   $D000-$DFFF  Char ROM      visible when HIRAM=1 && CHAREN=0
 *   $D000-$DFFF  I/O devices   visible when HIRAM=1 && CHAREN=1
 *   $E000-$FFFF  KERNAL ROM    visible when HIRAM=1
 *
 * Unlike the C64PLA (which intercepts reads on every access), this controller
 * updates SparseMemoryBus overlays when banking state changes. The $D000-$DFFF
 * char ROM case is handled by intercepting reads (like C64PLA) since it
 * conflicts with I/O handlers.
 *
 * The controller also emulates the 6510-compatible CPU I/O port at $00/$01:
 *   $00 = data direction register
 *   $01 = port value (bits 0-2 = LORAM, HIRAM, CHAREN)
 *
 * Usage:
 *   auto* bc = new C64BankController(physBus);
 *   bc->setBasicRom(data, size);
 *   bc->setKernalRom(data, size);
 *   bc->setCharRom(data, size);
 *   ioRegistry->registerHandler(bc);
 */
class C64BankController : public IOHandler {
public:
    explicit C64BankController(SparseMemoryBus* physBus);
    ~C64BankController() override = default;

    void setBasicRom (const uint8_t* data, uint32_t size) override;
    void setKernalRom(const uint8_t* data, uint32_t size) override;
    void setCharRom  (const uint8_t* data, uint32_t size) override;

    /** Set the MapMmu so we can check MAP enable state. */
    void setMapMmu(MapMmu* mmu) { m_mapMmu = mmu; }

    /** Set full C65 ROM (128KB) for $D030 ROM banking. */
    void setFullRom(const uint8_t* data, uint32_t size) { m_fullRom = data; m_fullRomSize = size; }

    /** Set callback to query VIC-III $D030 register for C65 ROM banking. */
    void setD030Query(std::function<uint8_t()> fn) { m_d030Query = std::move(fn); }

    /** Set callback to check if CPU is in hypervisor mode. */
    void setHypervisorQuery(std::function<bool()> fn) { m_hyperQuery = std::move(fn); }

    // IOHandler interface
    const char* name()     const override { return "C64BankCtrl"; }
    uint32_t    baseAddr() const override { return 0x0000; }
    uint32_t    addrMask() const override { return 0xFFFF; }

    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t  val) override;
    void reset() override;
    void tick(uint64_t) override {}

    uint8_t portValue() const { return m_portOut; }

private:
    SparseMemoryBus* m_physBus;
    MapMmu*          m_mapMmu = nullptr;

    uint8_t m_portDDR = 0x2F;   // Data direction register ($00)
    uint8_t m_portOut = 0x37;   // Port output latch ($01)

    const uint8_t* m_basicRom  = nullptr;  uint32_t m_basicSize  = 0;
    const uint8_t* m_kernalRom = nullptr;  uint32_t m_kernalSize = 0;
    const uint8_t* m_charRom   = nullptr;  uint32_t m_charSize   = 0;
    const uint8_t* m_fullRom   = nullptr;  uint32_t m_fullRomSize = 0;

    bool hiram()  const { return (effectivePort() >> 1) & 1; }
    bool loram()  const { return  effectivePort()       & 1; }
    bool charen() const { return (effectivePort() >> 2) & 1; }
    uint8_t effectivePort() const { return m_portOut | ~m_portDDR; }

    bool isBlockMapped(int block) const;
    void updateOverlays();

    std::function<uint8_t()> m_d030Query;
    std::function<bool()>    m_hyperQuery;
    uint8_t d030() const { return m_d030Query ? m_d030Query() : 0; }
    bool inHypervisor() const { return m_hyperQuery ? m_hyperQuery() : false; }
};
