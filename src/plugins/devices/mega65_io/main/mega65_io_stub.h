#pragma once
#include "libdevices/main/io_handler.h"
#include <cstring>
#include <string>
#include <vector>

/**
 * MEGA65 System I/O Stub Handler
 *
 * Catch-all for unimplemented MEGA65-specific I/O registers in the
 * $D600-$D6FF range. Absorbs reads/writes with sensible defaults
 * so HYPPO can proceed through initialization without reading garbage.
 *
 * Specific sub-ranges handled:
 *   $D600-$D60F  UART / VDC emulation (returns READY on status)
 *   $D60A        Keyboard queue status + modifier state at event time
 *   $D610        ASCII key at top of typing event queue
 *   $D611        Current modifier key state (immediate, read-only)
 *   $D619        PETSCII key at top of typing event queue
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
    uint32_t    addrMask() const override { return 0x7FFF; } // $D600-$DFFF range

    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override;
    void reset() override;
    void tick(uint64_t) override {}

    /// Access the colour RAM buffer (32KB, shared with VIC4 for rendering)
    uint8_t* colorRam() { return m_colorRam; }

    /// Set callback to query if 2KB Color RAM view ($D800-$DFFF) is enabled.
    void setCram2kQuery(std::function<bool()> q) { m_cram2kQuery = q; }

    // -----------------------------------------------------------------------
    // Keyboard buffer ($D610/$D619/$D60A/$D611)
    // -----------------------------------------------------------------------

    /// Push a key event into the typing queue.
    /// @param ascii   ASCII code (0 if no ASCII representation)
    /// @param petscii PETSCII code (0 if no PETSCII representation)
    /// @param mods    Modifier bitmask at time of event:
    ///                bit 0 = left shift, bit 1 = right shift,
    ///                bit 2 = CTRL, bit 3 = MEGA/C=,
    ///                bit 4 = ALT, bit 5 = NOSCRL, bit 6 = CAPS
    void pushKey(uint8_t ascii, uint8_t petscii, uint8_t mods);

    /// Update the current (immediate) modifier state for $D611.
    void setModifiers(uint8_t mods) { m_currentMods = mods; }

    /// Flush the keyboard queue.
    void flushKeyQueue() { m_keyQueue.clear(); }

private:
    struct KeyEvent {
        uint8_t ascii;
        uint8_t petscii;
        uint8_t mods;    // modifier state at time of event
    };

    uint8_t m_regs[256];     // Shadow for $D600-$D6FF
    uint8_t m_colorRam[32768]; // Colour RAM (32KB total)
    std::function<bool()> m_cram2kQuery;

    std::vector<KeyEvent> m_keyQueue;
    uint8_t m_currentMods = 0; // $D611: immediate modifier state
};
