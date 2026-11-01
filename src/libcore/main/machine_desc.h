#pragma once

#include "icore.h"
#include "libmem/main/ibus.h"
#include "libdevices/main/io_registry.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <functional>
#include <map>

/**
 * Slot for a CPU in a machine.
 */
struct CpuSlot {
    std::string slotName;
    ICore*      cpu;
    IBus*       dataBus;
    IBus*       codeBus;
    IBus*       ioBus;
    bool        active;
    uint64_t    clockDiv;
};

/**
 * Slot for a Bus in a machine.
 */
struct BusSlot {
    std::string busName;
    IBus*       bus;
};

/**
 * Memory overlay definition.
 */
struct MemOverlay {
    std::string path;
    uint32_t    base;
    bool        cpuVisible;
    bool        active;
};

/**
 * Forward declaration of IORegistry.
 */
class IORegistry;

/**
 * Machine Descriptor.
 */
struct MachineDescriptor {
    ~MachineDescriptor() {
        for (auto& d : deleters) d();
        delete ioRegistry;
        for (auto& slot : cpus) delete slot.cpu;
        for (auto& slot : buses) delete slot.bus;
        for (auto* rom : roms) delete[] rom;
    }

    std::string machineId;
    std::string displayName;
    std::string licenseClass;
    std::string preferredAssembler;  // From machine JSON "assembler" key, e.g. "ca45"

    std::vector<CpuSlot> cpus;
    std::vector<BusSlot> buses;

    IORegistry* ioRegistry = nullptr;

    // Named devices for device-specific CLI commands (e.g., cia1.advance)
    // Maps device name to IOHandler pointer
    std::map<std::string, class IOHandler*> namedDevices;

    // Original JSON spec used to build this machine
    nlohmann::json sourceSpec;

    // Generic deleters for objects not owned by Registry or Vectors
    std::vector<std::function<void()>> deleters;

    // Lifecycle hooks
    std::function<void(MachineDescriptor&)> onReset;
    std::function<void(MachineDescriptor&)> onInit;

    /** Key injection hook — set by machines that have a keyboard matrix.
     *  @param keyName  Symbolic key name (e.g. "a", "return", "f1").
     *  @param down     true = press, false = release.
     *  @return true if the key name was recognised.
     */
    std::function<bool(const std::string& keyName, bool down)> onKey;

    /** Joystick injection hook.
     *  @param port     Joystick port index (0 or 1).
     *  @param bits     5-bit active-low state (0=pressed, 1=idle).
     */
    std::function<void(int port, uint8_t bits)> onJoystick;

    // Scheduler
    std::function<int(MachineDescriptor&)> schedulerStep;

    /**
     * CPU speed modelling (optional; set by machines whose CPU clock can run
     * faster than the device clock, e.g. the MEGA65 at 3.5/40.5 MHz).
     * Warp on: devices advance one cycle per CPU cycle whatever the speed
     * (fast). Warp off: devices run at real time relative to the CPU.
     */
    std::function<bool()>     getWarp;
    std::function<void(bool)> setWarp;
    /** Current CPU clock in MHz as the hardware would run it (ignores warp). */
    std::function<double()>   cpuSpeedMHz;
    /**
     * Select a CPU clock (MHz) the way software would (e.g. MEGA65 speed
     * registers). Returns false if the machine has no such speed. Software
     * may change it again afterwards, as on hardware.
     */
    std::function<bool(double)> setCpuSpeedMHz;
    /** CPU clocks setCpuSpeedMHz() accepts, slowest first. */
    std::vector<double>       cpuSpeeds;

    std::vector<MemOverlay> overlays;
    std::vector<uint8_t*>   roms; // Buffers owned by the machine
    std::vector<std::string> symbolFiles;
};
