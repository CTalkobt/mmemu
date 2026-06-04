#pragma once
#include "libdevices/main/io_handler.h"
#include <cstring>
#include <string>

/**
 * MEGA65 System I/O Stub Handler
 *
 * Catch-all for unimplemented MEGA65-specific I/O registers in the
 * $D600-$D6FF range. Absorbs reads/writes with sensible defaults
 * so HYPPO can proceed through initialization without reading garbage.
 *
 * Specific sub-ranges handled:
 *   $D600-$D60F  UART / VDC emulation (returns READY on status)
 *   $D610-$D61F  Keyboard scan / ASCII input
 *   $D620-$D63F  FPGA info, model ID, firmware dates
 *   $D640-$D67F  Hypervisor regs (handled by HypervisorRegs — excluded)
 *   $D680-$D6FF  SD card handled by SdCardDevice — partially excluded
 *   $D6Fx        Ethernet, misc system registers
 *
 * Also handles colour RAM at $D800-$DBFF.
 *
 * This handler is registered with LOW priority (checked after specific
 * handlers) to act as a fallback.
 */
class Mega65IoStub : public IOHandler {
public:
    Mega65IoStub();
    ~Mega65IoStub() override = default;

    const char* name() const override { return "MEGA65 I/O"; }
    uint32_t    baseAddr() const override { return 0xD600; }
    uint32_t    addrMask() const override { return 0x03FF; } // $D600-$D9FF (covers D600-D6FF + D800-DBFF with colour RAM)

    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override;
    void reset() override;
    void tick(uint64_t) override {}

private:
    uint8_t m_regs[256];     // Shadow for $D600-$D6FF
    uint8_t m_colorRam[1024]; // Colour RAM $D800-$DBFF
};
