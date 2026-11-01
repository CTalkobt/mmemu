#include "machine_speed.h"
#include "libcore/main/machine_desc.h"
#include <algorithm>
#include <sstream>

namespace machine_speed {

bool supported(const MachineDescriptor& m) {
    return m.getWarp && m.setWarp && m.cpuSpeedMHz;
}

std::string formatMHz(double mhz) {
    std::ostringstream o;
    o << mhz;
    return o.str();
}

std::string choices(const MachineDescriptor& m) {
    std::string out;
    for (double v : m.cpuSpeeds) out += formatMHz(v) + "|";
    return out + "max";
}

bool apply(MachineDescriptor& m, const std::string& value, std::string& error, double* selectedMHz) {
    if (selectedMHz) *selectedMHz = 0;
    if (!supported(m)) { error = "This machine has no CPU speed model."; return false; }
    std::string arg = value;
    std::transform(arg.begin(), arg.end(), arg.begin(), ::tolower);
    if (arg.size() > 3 && arg.compare(arg.size() - 3, 3, "mhz") == 0) arg.resize(arg.size() - 3);

    if (arg == "max") {
        m.setWarp(true);
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
    if (!m.setCpuSpeedMHz || !m.setCpuSpeedMHz(mhz)) {
        error = "Syntax: speed [" + choices(m) + "]";
        return false;
    }
    m.setWarp(false);
    if (selectedMHz) *selectedMHz = mhz;
    return true;
}

std::string describe(const MachineDescriptor& m) {
    if (!supported(m)) return "This machine has no CPU speed model.";
    if (m.getWarp())
        return "Speed: max (warp: devices advance one cycle per CPU cycle; CPU set to "
               + formatMHz(m.cpuSpeedMHz()) + " MHz)";
    return "Speed: " + formatMHz(m.cpuSpeedMHz()) + " MHz";
}

std::string pendingNote(const MachineDescriptor& m, double requestedMHz) {
    if (!supported(m) || requestedMHz <= 0 || m.cpuSpeedMHz() == requestedMHz) return "";
    return "(" + formatMHz(requestedMHz) + " MHz is selected; the CPU runs at full speed for now, "
           "e.g. in the hypervisor or after POKE 0,65)";
}

}  // namespace machine_speed
