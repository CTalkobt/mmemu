#pragma once

#include "libcore/main/icore.h"
#include "imap_controller.h"
#include "plugins/devices/map_mmu/main/map_mmu.h"
#include <cstdint>

/**
 * 45GS02 Status Flags (same as 6502).
 */
#define FLAG_C 0x01
#define FLAG_Z 0x02
#define FLAG_I 0x04
#define FLAG_D 0x08
#define FLAG_B 0x10
#define FLAG_E 0x20
#define FLAG_R 0x20 // Alias for bit 5
#define FLAG_V 0x40
#define FLAG_N 0x80

/**
 * 45GS02 Internal State.
 */
struct CPU45GS02State {
    uint8_t  a;
    uint8_t  x;
    uint8_t  y;
    uint8_t  z;
    uint8_t  b;      // Base page register
    uint16_t sp;     // 16-bit stack pointer
    uint16_t pc;
    uint8_t  p;
    uint64_t cycles;

    uint8_t  irqLine;
    uint8_t  nmiLine;
    uint8_t  nmiPrev;
    uint8_t  haltLine;

    // Hypervisor state
    bool     hypervisor;    // true = hypervisor mode active
};

/**
 * Hypervisor virtualisation control registers ($D640-$D67F).
 * Stores saved CPU state on hypervisor entry; restored on exit ($D67F write).
 */
struct HypervisorState {
    uint8_t  regA, regX, regY, regZ, regB;
    uint8_t  spl, sph;
    uint8_t  pflags;
    uint16_t pc;
    uint8_t  mapLo0, mapLo1, mapHi0, mapHi1;
    uint8_t  mapLoMB, mapHiMB;
    uint32_t megabyteLow, megabyteHigh;  // MAP megabyte base addresses
    uint8_t  port00, port01;
    uint8_t  vicMode;
};

/**
 * MEGA65 45GS02 CPU Implementation.
 */
class MOS45GS02 : public ICore {
public:
    MOS45GS02();
    virtual ~MOS45GS02() { delete[] m_hyperRam; }

    // ISA identification
    const char* isaName()     const override { return "45GS02"; }
    const char* variantName() const override { return "MEGA65 45GS02"; }
    uint32_t    isaCaps()     const override { return 0; }

    // Register descriptor table
    int                  regCount()             const override { return 9; }
    const RegDescriptor* regDescriptor(int idx) const override;
    uint32_t             regRead (int idx)      const override;
    void                 regWrite(int idx, uint32_t val) override;

    // Convenience accessors
    uint32_t pc() const override { return m_state.pc; }
    void     setPc(uint32_t addr) override { m_state.pc = (uint16_t)addr; }
    uint32_t sp() const override { return m_state.sp; }

    // Execution
    int  step()  override;
    void reset() override;

    // Interrupts
    void triggerIrq()   override;
    void triggerNmi()   override;
    void triggerReset() override { reset(); }
    void setIrqLine(bool asserted) override { m_state.irqLine = asserted ? 1 : 0; }
    void setNmiLine(bool asserted) override { m_state.nmiLine = asserted ? 1 : 0; }
    void setHaltLine(bool asserted) override { m_state.haltLine = asserted ? 1 : 0; }

    // Disassembly (stubs for now)
    int disassembleOne  (IBus* bus, uint32_t addr, char* buf, int bufsz) override;
    int disassembleEntry(IBus* bus, uint32_t addr, void* entryOut)      override;

    // Snapshot support
    size_t stateSize()             const override { return sizeof(CPU45GS02State); }
    void   saveState(uint8_t* buf) const override;
    void   loadState(const uint8_t* buf) override;

    // Step-over / step-out heuristics
    int  isCallAt    (IBus* bus, uint32_t addr) override;
    bool isReturnAt  (IBus* bus, uint32_t addr) override;
    bool isProgramEnd(IBus* bus)                override;

    int writeCallHarness(IBus* bus, uint32_t scratch, uint32_t routineAddr) override;

    // Bus attachment
    void setDataBus(IBus* bus) override { m_bus = bus; }
    void setCodeBus(IBus* bus) override { m_bus = bus; }
    void setIoBus  (IBus* bus) override { (void)bus; }

    IBus* getDataBus() const override { return m_bus; }
    IBus* getCodeBus() const override { return m_bus; }
    IBus* getIoBus()   const override { return nullptr; }
    bool isHalted() const override { return m_state.haltLine; }

    uint64_t cycles() const override { return m_state.cycles; }

    // MEGA65 MAP instruction support (optional — only set if MapMmu is in use)
    IMapController* getMapMmu() const override { return m_mapMmu; }
    void setMapMmu(IMapController* mmu) override { m_mapMmu = mmu; }

    // Hypervisor support
    bool isHypervisor() const { return m_state.hypervisor; }
    void setHypervisorRom(const uint8_t* data, uint32_t size);
    void enterHypervisor(uint16_t trapAddr);
    void exitHypervisor();

    // Virtualisation control registers ($D640-$D67F)
    HypervisorState& hyperState() { return m_hyperState; }
    const HypervisorState& hyperState() const { return m_hyperState; }

private:
    CPU45GS02State  m_state;
    HypervisorState m_hyperState;
    IBus*           m_bus;
    IMapController* m_mapMmu = nullptr;

    const uint8_t*  m_hyperRom     = nullptr;
    uint8_t*        m_hyperRam     = nullptr;  // writable copy for hypervisor mode
    uint32_t        m_hyperRomSize = 0;

    // Helper for address translation (MAP)
    uint32_t translate(uint16_t addr);
    uint32_t read32(uint16_t addr);
    void     write32(uint16_t addr, uint32_t val);
    uint32_t read32_phys(uint32_t physAddr);
    void     write32_phys(uint32_t physAddr, uint32_t val);
    uint8_t  read8(uint16_t addr);
    void     write8(uint16_t addr, uint8_t val);
    uint8_t  read8_phys(uint32_t physAddr);
    void     write8_phys(uint32_t physAddr, uint8_t val);
    void     doAdc8(uint8_t v);
    void     doSbc8(uint8_t v);
    void     updateNZ(uint8_t val);
    void     updateNZ16(uint16_t val);
    void     updateNZ32(uint32_t val);
    void     push8(uint8_t v);
    uint8_t  pull8();
    void     push16(uint16_t v);
    uint16_t pull16();
};
