#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include "device_info.h"

class IBus;
class ISignalLine;
class IPortDevice;

/**
 * Abstract base class for memory-mapped I/O devices.
 */
class IOHandler {
public:
    virtual ~IOHandler();

    // -----------------------------------------------------------------------
    // Configuration setters (overridden by concrete devices).
    // Defaults are no-ops so devices that don't need a setter can ignore it.
    // -----------------------------------------------------------------------
    virtual void setName    (const std::string&) {}
    virtual void setBaseAddr(uint32_t)           {}
    virtual void setClockHz (uint32_t)           {}
    virtual void setIrqLine (ISignalLine*)        {}
    virtual void setNmiLine (ISignalLine*)        {}

    // Port device attachment (VIA, CIA, PIA)
    virtual void setPortADevice(IPortDevice*) {}
    virtual void setPortBDevice(IPortDevice*) {}

    // Port-A write callback — called with (pra, ddra) after each PRA/DDRA write.
    // CIA6526 overrides this for VIC-II bank switching.
    virtual void setPortAWriteCallback(std::function<void(uint8_t, uint8_t)>) {}

    // Control-line attachment (VIA, PIA)
    virtual void setCA1Line(ISignalLine*) {}
    virtual void setCB1Line(ISignalLine*) {}
    virtual void setCA2Line(ISignalLine*) {}
    virtual void setCB2Line(ISignalLine*) {}

    // PIA-specific dual interrupt lines
    virtual void setIrqALine(ISignalLine*) {}
    virtual void setIrqBLine(ISignalLine*) {}

    // Named signal-line input (used by C64PLA for loram/hiram/charen)
    virtual void setSignalLine(const char*, ISignalLine*) {}

    // DMA / direct-read bus (VIC-II, ANTIC)
    virtual void setDmaBus(IBus*) {}

    // VIC banking base address
    virtual void setBankBase(uint32_t) {}

    // Color RAM pointer (VIC-I, VIC-II)
    virtual void setColorRam(const uint8_t*) {}

    // ROM data passthrough (video chips and banking controllers)
    virtual void setCharRom (const uint8_t*, uint32_t) {}
    virtual void setBasicRom(const uint8_t*, uint32_t) {}
    virtual void setKernalRom(const uint8_t*, uint32_t) {}
    virtual void setRamData  (const uint8_t*)           {}

    // Generic device-to-device link by role name (e.g. "crtc" → CRTC6545*).
    // Allows the JSON loader to wire companion devices without machine-specific casts.
    virtual void setLinkedDevice(const char*, IOHandler*) {}
    virtual ISignalLine* getSignalLine(const char*) { return nullptr; }
    virtual bool mountTape(const std::string& path) { return false; }
    virtual void controlTape(const std::string& op) {}
    virtual bool startTapeRecord()                           { return false; }
    virtual void stopTapeRecord()                            {}
    virtual bool saveTapeRecording(const std::string& path)  { return false; }
    virtual bool isTapeRecording() const                     { return false; }
    virtual bool isHaltRequested() const                     { return false; }

    // -----------------------------------------------------------------------
    // Identity (pure virtual — every device must provide these)
    // -----------------------------------------------------------------------

    /**
     * Unique name of the device (e.g. "VIC-II").
     */
    virtual const char* name() const = 0;

    /**
     * Base address where this device is mapped.
     */
    virtual uint32_t baseAddr() const = 0;

    /**
     * Address mask to determine the range handled by this device.
     */
    virtual uint32_t addrMask() const = 0;

    /**
     * Read from the device at the given address.
     * @return true if the address was handled by this device.
     */
    virtual bool ioRead(IBus* bus, uint32_t addr, uint8_t* val) = 0;

    /**
     * Write to the device at the given address.
     * @return true if the address was handled by this device.
     */
    virtual bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) = 0;

    /**
     * Reset the device to its power-on state.
     */
    virtual void reset() = 0;

    /**
     * Tick the device forward. Called each CPU step or cycle.
     */
    virtual void tick(uint64_t cycles) = 0;

    /**
     * Get detailed information about the device's current state.
     */
    virtual void getDeviceInfo(DeviceInfo& out) const {
        out.name = name();
        out.baseAddr = baseAddr();
        out.addrMask = addrMask();
    }

    /**
     * Return named computed values derived from internal device state.
     * These bypass I/O mapping and ROM overlays — they call device methods directly.
     * Used by the expression evaluator for "DEVICE.NAME" lookups (e.g. "VIC4.SCREEN").
     * Names should be uppercase, without the device prefix (caller prepends it).
     */
    virtual std::vector<std::pair<std::string, uint32_t>> getDerivedValues() const { return {}; }

    /** Short aliases for device name matching (e.g. "VIC4" for "VIC-IV"). */
    virtual std::vector<std::string> deviceAliases() const { return {}; }

    // Disk/Image management (optional overrides)
    virtual bool mountDisk(int unit, const std::string& path) { return false; }
    virtual void ejectDisk(int unit)                          {}
    virtual bool getDiskStatus(int unit, int& track, int& sector, bool& led) { return false; }
    virtual std::string getMountedDiskPath(int unit) { return ""; }
};
