#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include "device_info.h"

class IBus;
class ISignalLine;
class IBaSource;
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
    virtual IPortDevice* portADevice() const { return nullptr; }
    /** A port this device offers by name, for machine JSON "portWiring" (e.g. an IEEE-488 bus's "dataIn"). */
    virtual IPortDevice* namedPort(const std::string&) { return nullptr; }
    virtual IPortDevice* portBDevice() const { return nullptr; }
    /** Port bits set as outputs read the pin level, so a device can pull a
     *  line low that the chip drives high (the C64's joysticks on CIA1). */
    virtual void setOutputsReadPins(bool) {}

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
    /**
     * The C64 expansion port: the cartridge's ROML ($8000) and ROMH ($A000,
     * or $E000 in Ultimax mode) images and its EXROM/GAME lines (true = the
     * cartridge pulls the line low). All null/false: no cartridge. The PLA
     * maps them; the VIC-II uses ROMH in Ultimax mode. True if the device
     * took them.
     */
    virtual bool setCartridgePort(const uint8_t* /*roml*/, uint32_t /*romlSize*/,
                                  const uint8_t* /*romh*/, uint32_t /*romhSize*/,
                                  bool /*exrom*/, bool /*game*/) { return false; }

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
    /** A serial port's far end (CLI `serial`): e.g. "loopback", "tcp:PORT",
     *  "none". False if this device has no serial port or the spec fails. */
    virtual bool attachSerial(const std::string& /*spec*/)   { return false; }
    /** A bus master (an REU's DMA): while it holds the bus, run one cycle of
     *  its work and return the cycles used (the CPU waits); 0 while it
     *  doesn't. Machines with a device that can take the bus call it before
     *  each CPU step (MachineDescriptor::busMasterStep). */
    virtual int busMasterCycle()                             { return 0; }
    /** The device pulling BA (the C64's VIC-II, #266): a bus master waits
     *  while it has the bus. */
    virtual void setBaSource(IBaSource* /*ba*/)              {}
    /** Cycle-exact access for a bus master (#304): called with the cycle
     *  (from 0) of busMasterCycle()'s access before it, so the machine can
     *  bring the other devices up to that cycle first. */
    virtual void setBusMasterSync(std::function<void(int)> /*sync*/) {}
    /** A setting for a device fitted at run time (an expansion's option:
     *  e.g. "size" = "512K", "image" = a path); false with `err` set if the
     *  device has no such option or the value doesn't fit. */
    virtual bool setOption(const std::string& /*key*/, const std::string& /*value*/, std::string& err) {
        err = "no such option";
        return false;
    }
    /** Save the device's memory as an image file (an REU's RAM). */
    virtual bool saveDeviceImage(const std::string& /*path*/, std::string& err) {
        err = "this device has no image to save";
        return false;
    }

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
     * Read a register's value WITHOUT side effects, for debugger views, memory
     * dumps and the "before" value of a write: no flag cleared, no pointer or
     * queue advanced, no pending work done. The default is ioRead, for devices
     * whose reads change nothing; a device whose read changes state must
     * override it.
     * @return true if the address was handled by this device.
     */
    virtual bool ioPeek(IBus* bus, uint32_t addr, uint8_t* val) { return ioRead(bus, addr, val); }

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
