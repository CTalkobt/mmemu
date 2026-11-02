#pragma once

#include <chrono>
#include <cstdint>
#include <string>

struct MachineDescriptor;

/**
 * CPU speed control shared by the CLI `speed` command, the MCP `set_speed`
 * tool and the GUI Control > Speed menu. "max" means unbounded (on the MEGA65,
 * warp: devices advance one cycle per CPU cycle); "real" runs at real time; a
 * number selects that CPU clock in MHz (machines with a CPU speed model).
 *
 * Real time (#169): the GUI and CLI `run` keep the machine's device clock in
 * step with the wall clock (RealTimePacer) unless it runs at max speed.
 */
namespace machine_speed {

/** True if the machine has a CPU speed model (getWarp/setWarp/cpuSpeedMHz). */
bool supported(const MachineDescriptor& m);

/** True if the machine can run at real time (clockHz set). */
bool paceable(const MachineDescriptor& m);

/** True if the machine has any speed setting (a CPU speed model or real time). */
bool controllable(const MachineDescriptor& m);

/** True if the machine runs unpaced: warp on, or maxSpeed without a speed model. */
bool atMax(const MachineDescriptor& m);

/** Select max (unpaced) or real time. */
void setMax(MachineDescriptor& m, bool on);

/** Device cycles elapsed (deviceCycles(), else the main CPU's cycles). */
uint64_t deviceCycles(const MachineDescriptor& m);

/**
 * Apply "max", "real" or a clock in MHz ("1", "3.5", "40", "2MHz"; "40" means 40.5).
 * Returns false and sets `error` (listing the choices) if the value is invalid.
 * `selectedMHz` (optional) receives the clock selected, or 0 for max.
 */
bool apply(MachineDescriptor& m, const std::string& value, std::string& error,
           double* selectedMHz = nullptr);

/** The accepted values, e.g. "1|2|3.5|40.5|real|max" or "real|max". */
std::string choices(const MachineDescriptor& m);

/** One-line description, e.g. "Speed: max (warp: ...)", "Speed: 3.5 MHz, real time". */
std::string describe(const MachineDescriptor& m);

/**
 * Extra line when `requestedMHz` differs from what the CPU runs at now (e.g.
 * in the MEGA65 hypervisor or after POKE 0,65); empty otherwise.
 */
std::string pendingNote(const MachineDescriptor& m, double requestedMHz);

/** Format a clock: 1, 3.5, 40.5. */
std::string formatMHz(double mhz);

/**
 * Keeps emulated device time from running ahead of the wall clock.
 *
 * Front ends call ahead() between steps: true means emulation is ahead of
 * real time and should wait (end the GUI slice, or sleep in the CLI). When
 * emulation falls more than maxLag behind (slow host, a pause, a modal
 * dialog) the pacer resyncs instead of catching up in a burst.
 */
class RealTimePacer {
public:
    using Clock = std::chrono::steady_clock;

    /** Forget the reference point; the next call resyncs (e.g. when Run starts). */
    void reset() { m_started = false; }

    /**
     * Core arithmetic: `cycles` device cycles at `hz` against `now`.
     * hz <= 0 never paces.
     */
    bool ahead(uint64_t cycles, double hz, Clock::time_point now);

    /** ahead() for a machine; false (and reset) when it is not paceable or at max. */
    bool ahead(const MachineDescriptor& m, Clock::time_point now);

    /** How long to wait until wall time reaches emulated time (zero if not ahead). */
    Clock::duration waitTime(uint64_t cycles, Clock::time_point now) const;

    static constexpr std::chrono::milliseconds maxLag{100};

private:
    bool              m_started = false;
    uint64_t          m_baseCycles = 0;
    double            m_hz = 0;
    Clock::time_point m_baseTime;
};

}  // namespace machine_speed
