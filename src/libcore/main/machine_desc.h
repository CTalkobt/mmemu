#pragma once
#include <algorithm>

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
class IIntercept;

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

    /**
     * Buttons under the screen (GUI): each holds its keys (onKey names, pressed
     * in order, released in reverse) while it is held. E.g. RUN/STOP + RESTORE
     * and RESTORE on the C64, VIC-20 and MEGA65; the PET's NMI button (it had
     * no RESTORE key). Machine JSON: "ui": {"screenButtons": [{"label",
     * "keys", "tooltip"}]}.
     */
    struct ScreenButton {
        std::string label;
        std::vector<std::string> keys;
        std::string tooltip;
    };
    std::vector<ScreenButton> screenButtons;

    /** Memory as RAM, also under ROM, for KERNAL file calls that fetch the
     *  file name and SAVE data from RAM whatever is banked in (the Plus/4's
     *  KERNAL does). Empty: they read through the bus. */
    std::function<uint8_t(uint16_t addr)> readRam;

    /**
     * Debugger panes that depend on this machine's hardware, by GUI pane id:
     * "graphics-vic" (VIC-I/II/IV sprites and characters), "sidscope",
     * "cartridge", "tape", "drive", "mega65". Panes
     * that suit any machine (console, CPU, memory, ...) are always shown.
     * Unset (uiPanesSet false): every pane, for machines that do not say.
     * Machine JSON: "ui": {"panes": [...]}.
     */
    std::vector<std::string> uiPanes;
    bool uiPanesSet = false;
    bool hasPane(const std::string& id) const {
        return !uiPanesSet || std::find(uiPanes.begin(), uiPanes.end(), id) != uiPanes.end();
    }
    /**
     * VIC graphics formats for the "graphics-vic" pane, CLI gfx and MCP
     * render_graphics (gfx format ids, e.g. "char", "sprite-mc"), in menu
     * order; empty = all formats. None at all without "graphics-vic".
     * Machine JSON: "ui": {"graphicsFormats": [...]}.
     */
    std::vector<std::string> graphicsFormats;

    /** Joystick injection hook.
     *  @param port     Joystick port index (0 or 1).
     *  @param bits     5-bit active-low state (0=pressed, 1=idle).
     */
    std::function<void(int port, uint8_t bits)> onJoystick;
    /** Number of onJoystick ports. */
    int joystickPorts = 0;
    /** The port most software reads (the C64's control port 2 is index 1);
     *  a host's first controller goes here. */
    int primaryJoystickPort = 0;

    /** Gamepad buttons for onGamepad (1 = pressed). */
    enum GamepadButton : uint32_t {
        PAD_UP = 1u << 0, PAD_DOWN = 1u << 1, PAD_LEFT = 1u << 2, PAD_RIGHT = 1u << 3,
        PAD_A = 1u << 4, PAD_B = 1u << 5, PAD_X = 1u << 6, PAD_Y = 1u << 7,
        PAD_L = 1u << 8, PAD_R = 1u << 9, PAD_SELECT = 1u << 10, PAD_START = 1u << 11,
    };
    /** Gamepad injection hook (machines with SNES-style pads, e.g. the X16).
     *  @param port     Pad index from 0.
     *  @param buttons  PAD_* bits held (1 = pressed).
     *  @return false if the machine has no such port.
     */
    std::function<bool(int port, uint32_t buttons)> onGamepad;

    /** PAD_* bits as a 5-bit active-low CBM joystick (up, down, left, right,
     *  fire); any face button fires. */
    static uint8_t cbmJoystickBits(uint32_t buttons) {
        uint8_t bits = 0x1F;
        if (buttons & PAD_UP)    bits &= ~0x01;
        if (buttons & PAD_DOWN)  bits &= ~0x02;
        if (buttons & PAD_LEFT)  bits &= ~0x04;
        if (buttons & PAD_RIGHT) bits &= ~0x08;
        if (buttons & (PAD_A | PAD_B | PAD_X | PAD_Y)) bits &= ~0x10;
        return bits;
    }

    /** Mouse injection hook.
     *  @param dx, dy   Movement since the last call, in host pixels (y down).
     *  @param buttons  Bit 0 left, bit 1 right, bit 2 middle (1 = held).
     *  @param wheel    Wheel steps (positive = away from the user).
     */
    std::function<void(int dx, int dy, uint8_t buttons, int wheel)> onMouse;

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

    /**
     * Real-time pacing (#169). clockHz is the device (phi) clock in Hz; 0
     * means the machine is never paced. Front ends that pace (GUI, CLI `run`)
     * keep deviceCycles() in step with the wall clock unless the machine runs
     * at max speed (warp on, or maxSpeed for machines without getWarp).
     * Machines are created at max speed, so tests and the MCP server run
     * flat out; the GUI and CLI select real time when they create one
     * (`--speed` overrides).
     */
    double                      clockHz = 0;
    /** Device cycles elapsed; unset = the main CPU's cycle count. */
    std::function<uint64_t()>   deviceCycles;
    /** Run unpaced; used by machines without getWarp/setWarp. */
    bool                        maxSpeed = true;

    /**
     * Problems found while building the machine that the user should see
     * (e.g. no MEGA65 SD card image), one sentence each (#172). They are
     * also logged; the GUI shows them in a notice bar and MCP create_machine
     * adds them to its reply.
     */
    std::vector<std::string>    warnings;

    /**
     * Intercepts installed from the machine's configuration (installIntercepts
     * in libcore/main/intercept.h); owned through deleters.
     */
    std::vector<IIntercept*>    intercepts;

    std::vector<MemOverlay> overlays;
    std::vector<uint8_t*>   roms; // Buffers owned by the machine
    std::vector<std::string> symbolFiles;
};
