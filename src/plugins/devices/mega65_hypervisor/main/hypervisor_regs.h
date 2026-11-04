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
 * Call intercepts: before HYPPO sees a user-mode trap, the machine's call
 * intercepts (libcore/main/intercept.h, from machines/mega65.json) may
 * service it, e.g. "mega65.hdos" for DOS calls from a host folder.
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

    /**
     * Called for each user-mode trap once the hypervisor is entered, with the
     * trap number (0-63) and the function: A & $7E for trap $00 (DOS, as
     * HYPPO dispatches it), else A. Return true if the call was serviced
     * (the hypervisor is left at once and HYPPO does not run).
     */
    using CallInterceptFn = std::function<bool(uint8_t trap, uint8_t function)>;
    void setCallIntercept(CallInterceptFn fn) { m_callIntercept = std::move(fn); }

private:
    MOS45GS02* m_cpu;
    std::string m_name = "HypervisorRegs";
    CallInterceptFn m_callIntercept;
};
