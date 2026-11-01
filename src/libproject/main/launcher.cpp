#include "launcher.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/icore.h"
#include "libcore/main/image_loader.h"
#include "libdebug/main/debug_context.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libmem/main/ibus.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace project {

MachineLaunchInfo launchInfoFor(const std::string& id) {
    // Boot step counts leave margin over the measured time to READY
    if (id == "c64")    return {0x0801, 0x2D, 1500000};
    if (id == "vic20")  return {0x1001, 0x2D, 1500000};
    if (id == "pet")    return {0x0401, 0x2A, 1500000};
    if (id == "mega65") return {0x2001, -1,   7000000};
    return {};
}

static IKeyboardMatrix* findKeyboard(MachineDescriptor& m) {
    if (!m.ioRegistry) return nullptr;
    std::vector<IOHandler*> handlers;
    m.ioRegistry->enumerate(handlers);
    for (auto* h : handlers)
        if (auto* k = dynamic_cast<IKeyboardMatrix*>(h)) return k;
    return nullptr;
}

static std::string hex4(uint32_t v) {
    std::ostringstream o;
    o << "$" << std::hex << std::uppercase;
    o.width(4); o.fill('0');
    o << v;
    return o.str();
}

LaunchResult launch(MachineDescriptor& m, DebugContext* dbg, const std::string& file, bool reset) {
    LaunchResult r;
    if (m.cpus.empty() || !m.cpus[0].cpu || m.buses.empty()) { r.message = "Machine has no CPU or bus"; return r; }
    ICore* cpu = m.cpus[0].cpu;
    const MachineLaunchInfo info = launchInfoFor(m.machineId);

    std::string ext = fs::path(file).extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    const bool isDisk = ext == ".d64" || ext == ".d71" || ext == ".d81" || ext == ".d80" || ext == ".d82" || ext == ".g64";

    // Reset and boot to READY (programs loaded before the KERNAL initialises
    // would be overwritten, and RUN needs BASIC ready for input)
    if (reset || cpu->cycles() == 0) {
        if (m.onReset) m.onReset(m);
        else cpu->reset();
        for (uint64_t i = 0; i < info.bootSteps; ++i) {
            if (m.schedulerStep) m.schedulerStep(m);
            else cpu->step();
            if (dbg && dbg->isPaused()) dbg->resume();   // ignore breakpoints hit while booting
        }
    }

    IImageLoader* loader = ImageLoaderRegistry::instance().findLoader(file);
    if (!loader) { r.message = "No loader for " + file; return r; }
    if (!loader->load(file, m.buses[0].bus, &m, 0)) { r.message = "Could not load " + file; return r; }

    IKeyboardMatrix* kbd = findKeyboard(m);
    if (isDisk) {
        // Mounted: load and run the first program like a user would
        if (!kbd || !info.basicStart) { r.message = "Mounted " + file + " (no keyboard/BASIC to start it)"; r.ok = true; return r; }
        kbd->enqueueText("LOAD\"*\",8,1\nRUN\n");
        r.typedRun = true;
        r.ok = true;
        r.message = "Mounted " + fs::path(file).filename().string() + " and typed LOAD\"*\",8,1 / RUN";
        return r;
    }

    // PRG: two-byte load address header
    std::ifstream f(file, std::ios::binary | std::ios::ate);
    std::streamoff size = f.tellg();
    f.seekg(0);
    uint8_t h[2] = {0, 0};
    f.read((char*)h, 2);
    r.loadAddress = h[0] | (h[1] << 8);
    uint32_t end = r.loadAddress + (uint32_t)(size > 2 ? size - 2 : 0);   // first byte after the program

    if (info.basicStart && r.loadAddress == info.basicStart && kbd) {
        if (info.endPointer >= 0) {
            IBus* bus = m.buses[0].bus;
            bus->write8((uint32_t)info.endPointer, end & 0xFF);
            bus->write8((uint32_t)info.endPointer + 1, (end >> 8) & 0xFF);
        }
        kbd->enqueueText("RUN\n");
        r.typedRun = true;
        r.message = "Loaded " + fs::path(file).filename().string() + " at " + hex4(r.loadAddress) + " and typed RUN";
    } else {
        cpu->setPc(r.loadAddress);
        r.message = "Loaded " + fs::path(file).filename().string() + " at " + hex4(r.loadAddress) +
                    "; PC = " + hex4(r.loadAddress);
    }
    r.ok = true;
    return r;
}

std::string loadDebugInfo(DebugContext& dbg, const Config& cfg) {
    int symFiles = 0, lines = 0, maps = 0;
    for (const auto& s : cfg.symbols)
        if (dbg.symbols().loadSym(cfg.resolve(s))) ++symFiles;
    for (const auto& sm : cfg.sourceMaps) {
        std::vector<std::pair<std::string, uint32_t>> syms;
        int n = dbg.sourceMap().loadCa45SourceMap(cfg.resolve(sm.map), cfg.resolve(sm.source), &syms);
        if (n > 0) { ++maps; lines += n; }
        for (const auto& [label, addr] : syms) dbg.symbols().addSymbol(addr, label);
    }
    if (cfg.symbols.empty() && cfg.sourceMaps.empty()) return "";
    std::ostringstream o;
    o << "Debug info: " << symFiles << "/" << cfg.symbols.size() << " symbol files, "
      << maps << "/" << cfg.sourceMaps.size() << " source maps (" << lines << " lines)";
    return o.str();
}

}  // namespace project
