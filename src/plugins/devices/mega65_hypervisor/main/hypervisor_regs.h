#pragma once

#include "libdevices/main/io_handler.h"
#include "plugins/45gs02/main/cpu45gs02.h"
#include <functional>
#include <string>

/**
 * MEGA65 Hypervisor Virtualisation Control Registers ($D640-$D67F).
 *
 * In user mode: writes to $D640-$D67F trigger SYSCALL traps.
 * In hypervisor mode: these are read/write registers for saved CPU state.
 * Writing to $D67F (ENTEREXIT) exits hypervisor mode.
 *
 * HDOS trap virtualization: when enabled, trap 0 (DOS) intercepts
 * function calls and services them from the host filesystem, bypassing
 * HYPPO's SD card code entirely.
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

    std::vector<std::string> deviceAliases() const override { return {"MEGA65_HYPERVISOR", "M45_HYPERVISOR"}; }

    /// Set HDOS trap handler. Called with function code (from A register).
    /// Returns true if the function was virtualized (caller should exit hypervisor).
    using HdosTrapFn = std::function<bool(uint8_t func, MOS45GS02* cpu)>;
    void setHdosTrapHandler(HdosTrapFn fn) { m_hdosTrap = std::move(fn); }

private:
    MOS45GS02* m_cpu;
    std::string m_name = "HypervisorRegs";
    HdosTrapFn m_hdosTrap;

    bool handleDosTrap();
};
