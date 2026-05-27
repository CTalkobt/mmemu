#pragma once

#include "libdevices/main/io_handler.h"
#include "plugins/45gs02/main/cpu45gs02.h"

/**
 * MEGA65 Hypervisor Virtualisation Control Registers ($D640-$D67F).
 *
 * In user mode: writes to $D640-$D67F trigger SYSCALL traps.
 * In hypervisor mode: these are read/write registers for saved CPU state.
 * Writing to $D67F (ENTEREXIT) exits hypervisor mode.
 */
class HypervisorRegs : public IOHandler {
public:
    explicit HypervisorRegs(MOS45GS02* cpu);
    ~HypervisorRegs() override = default;

    const char* name() const override { return "HypervisorRegs"; }
    uint32_t baseAddr() const override { return 0xD640; }
    uint32_t addrMask() const override { return 0x003F; } // 64 bytes

    void setName(const std::string& n) override { m_name = n; }
    void setBaseAddr(uint32_t) override {}

    bool ioRead(IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override;
    void reset() override {}
    void tick(uint64_t) override {}

private:
    MOS45GS02* m_cpu;
    std::string m_name = "HypervisorRegs";
};
