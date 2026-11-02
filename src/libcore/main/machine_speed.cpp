#include "machine_speed.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/icore.h"
#include <algorithm>
#include <sstream>

namespace machine_speed {

bool supported(const MachineDescriptor& m) {
    return m.getWarp && m.setWarp && m.cpuSpeedMHz;
}

bool paceable(const MachineDescriptor& m) {
    return m.clockHz > 0;
}

bool controllable(const MachineDescriptor& m) {
    return supported(m) || paceable(m);
}

bool atMax(const MachineDescriptor& m) {
    if (m.getWarp) return m.getWarp();
    return m.maxSpeed;
}

void setMax(MachineDescriptor& m, bool on) {
    if (m.setWarp) m.setWarp(on);
    else m.maxSpeed = on;
}

uint64_t deviceCycles(const MachineDescriptor& m) {
    if (m.deviceCycles) return m.deviceCycles();
    for (const auto& s : m.cpus)
        if (s.cpu) return s.cpu->cycles();
    return 0;
}

std::string formatMHz(double mhz) {
    std::ostringstream o;
    o << mhz;
    return o.str();
}

std::string choices(const MachineDescriptor& m) {
    std::string out;
    if (supported(m))
        for (double v : m.cpuSpeeds) out += formatMHz(v) + "|";
    if (paceable(m)) out += "real|";
    return out + "max";
}

bool apply(MachineDescriptor& m, const std::string& value, std::string& error, double* selectedMHz) {
    if (selectedMHz) *selectedMHz = 0;
    if (!controllable(m)) { error = "This machine has no CPU speed model."; return false; }
    std::string arg = value;
    std::transform(arg.begin(), arg.end(), arg.begin(), ::tolower);
    if (arg.size() > 3 && arg.compare(arg.size() - 3, 3, "mhz") == 0) arg.resize(arg.size() - 3);

    if (arg == "max") {
        setMax(m, true);
        return true;
    }
    if (arg == "real" || arg == "realtime" || arg == "real-time") {
        if (!paceable(m)) { error = "Syntax: speed [" + choices(m) + "]"; return false; }
        setMax(m, false);
        return true;
    }
    double mhz = 0;
    try {
        size_t used = 0;
        mhz = std::stod(arg, &used);
        if (used != arg.size()) mhz = 0;
    } catch (...) {
        mhz = 0;
    }
    if (mhz == 40.0) mhz = 40.5;   // "40" means the 40.5 MHz mode
    if (!supported(m) || !m.setCpuSpeedMHz || !m.setCpuSpeedMHz(mhz)) {
        error = "Syntax: speed [" + choices(m) + "]";
        return false;
    }
    m.setWarp(false);
    if (selectedMHz) *selectedMHz = mhz;
    return true;
}

std::string describe(const MachineDescriptor& m) {
    if (!controllable(m)) return "This machine has no CPU speed model.";
    if (supported(m)) {
        if (m.getWarp())
            return "Speed: max (warp: devices advance one cycle per CPU cycle; CPU set to "
                   + formatMHz(m.cpuSpeedMHz()) + " MHz)";
        return "Speed: " + formatMHz(m.cpuSpeedMHz()) + " MHz" + (paceable(m) ? ", real time" : "");
    }
    if (m.maxSpeed) return "Speed: max (unpaced)";
    std::ostringstream o;
    o << "Speed: real time (" << (uint64_t)m.clockHz << " Hz)";
    return o.str();
}

std::string pendingNote(const MachineDescriptor& m, double requestedMHz) {
    if (!supported(m) || requestedMHz <= 0 || m.cpuSpeedMHz() == requestedMHz) return "";
    return "(" + formatMHz(requestedMHz) + " MHz is selected; the CPU runs at full speed for now, "
           "e.g. in the hypervisor or after POKE 0,65)";
}

// ---------------------------------------------------------------------------
// RealTimePacer

bool RealTimePacer::ahead(uint64_t cycles, double hz, Clock::time_point now) {
    if (hz <= 0) { m_started = false; return false; }
    if (!m_started || hz != m_hz || cycles < m_baseCycles) {
        m_started = true;
        m_hz = hz;
        m_baseCycles = cycles;
        m_baseTime = now;
        return false;
    }
    auto emulated = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>((double)(cycles - m_baseCycles) / hz));
    auto wall = now - m_baseTime;
    if (emulated > wall) return true;
    if (wall - emulated > maxLag) {
        // Too far behind (slow host or a stall): resync rather than burst to catch up
        m_baseCycles = cycles;
        m_baseTime = now;
    }
    return false;
}

bool RealTimePacer::ahead(const MachineDescriptor& m, Clock::time_point now) {
    if (!paceable(m) || atMax(m)) { m_started = false; return false; }
    return ahead(deviceCycles(m), m.clockHz, now);
}

RealTimePacer::Clock::duration RealTimePacer::waitTime(uint64_t cycles, Clock::time_point now) const {
    if (!m_started || m_hz <= 0 || cycles < m_baseCycles) return Clock::duration::zero();
    auto emulated = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>((double)(cycles - m_baseCycles) / m_hz));
    auto wall = now - m_baseTime;
    return emulated > wall ? emulated - wall : Clock::duration::zero();
}

}  // namespace machine_speed
