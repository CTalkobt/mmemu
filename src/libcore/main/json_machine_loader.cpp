#include "json_machine_loader.h"
#include "util/path_util.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/machines/machine_registry.h"
#include "libcore/main/core_registry.h"
#include "libcore/main/rom_loader.h"
#include "libmem/main/memory_bus.h"
#include "libmem/main/sparse_memory_bus.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/device_registry.h"
#include "libdevices/main/isignal_line.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/joystick.h"
#include "libdevices/main/color_ram_handler.h"
#include "libdevices/main/open_bus_handler.h"
#include "libdevices/main/shared_irq_manager.h"
#include "libdevices/main/simple_signal_line.h"
#include "util/logging.h"
#include <fstream>
#include <sstream>
#include <map>
#include <string>
#include <iostream>

// ---------------------------------------------------------------------------
// Local signal-line helpers (CpuIrqLine, CpuNmiLine)
// These forward level-changes to the CPU interrupt pins.
// ---------------------------------------------------------------------------

namespace {

class CpuIrqLine : public ISignalLine {
public:
    explicit CpuIrqLine(ICore* cpu) : m_cpu(cpu) {}
    bool get()  const override { return m_level; }
    void pulse() override { if (m_cpu) m_cpu->triggerIrq(); }
    void set(bool level) override {
        m_level = level;
        if (level && m_cpu) m_cpu->triggerIrq();
    }
private:
    ICore* m_cpu;
    bool   m_level = false;
};

class CpuNmiLine : public ISignalLine {
public:
    explicit CpuNmiLine(ICore* cpu) : m_cpu(cpu) {}
    bool get()  const override { return m_level; }
    void pulse() override { if (m_cpu) m_cpu->triggerNmi(); }
    void set(bool level) override {
        m_level = level;
        if (level && m_cpu) m_cpu->triggerNmi();
    }
private:
    ICore* m_cpu;
    bool   m_level = false;
};

/**
 * DMA bus: wraps FlatMemoryBus, bypasses I/O hooks for raw memory reads.
 * Used for VIC-II DMA reads so they see underlying RAM/ROM rather than
 * device registers.
 */
class DmaBus : public IBus {
public:
    explicit DmaBus(FlatMemoryBus* base) : m_base(base) {}
    const BusConfig& config() const override { return m_base->config(); }
    const char*      name()   const override { return "DmaBus"; }
    uint8_t  read8(uint32_t addr)  override { return m_base->read8Raw(addr); }
    void     write8(uint32_t, uint8_t) override {}
    uint8_t  peek8(uint32_t addr)  override { return m_base->peek8Raw(addr); }
    size_t   stateSize()             const override { return 0; }
    void     saveState(uint8_t*)     const override {}
    void     loadState(const uint8_t*)     override {}
    int      writeCount()            const override { return 0; }
    void     getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void     clearWriteLog()               override {}
private:
    FlatMemoryBus* m_base;
};

} // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint32_t hexStrToU32(const std::string& s) {
    return static_cast<uint32_t>(std::stoul(s, nullptr, 0));
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

int JsonMachineLoader::loadFile(const std::string& path) {
    std::string actualPath = PathUtil::findResource(path);
    std::ifstream f(actualPath);
    if (!f.is_open()) return 0;
    std::ostringstream ss;
    ss << f.rdbuf();
    return loadString(ss.str());
}

int JsonMachineLoader::loadString(const std::string& json) {
    try {
        auto doc = nlohmann::json::parse(json);
        return registerAll(doc);
    } catch (const nlohmann::json::exception&) {
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

int JsonMachineLoader::registerAll(const nlohmann::json& doc) {
    const nlohmann::json* machines = nullptr;
    nlohmann::json singleArr;

    if (doc.is_array()) {
        machines = &doc;
    } else if (doc.is_object()) {
        if (doc.contains("machines") && doc["machines"].is_array()) {
            machines = &doc["machines"];
        } else {
            singleArr = nlohmann::json::array({ doc });
            machines  = &singleArr;
        }
    } else {
        return 0;
    }

    int count = 0;
    for (const auto& spec : *machines) {
        if (!spec.contains("id") || !spec["id"].is_string()) continue;
        std::string id = spec["id"].get<std::string>();

        nlohmann::json specCopy = spec;
        std::string displayName;
        if (spec.contains("displayName") && spec["displayName"].is_string())
            displayName = spec["displayName"].get<std::string>();
        LogRegistry::instance().getLogger("system")->debug("Registering machine: {}", id);
        MachineRegistry::instance().registerMachine(id,
            [specCopy]() -> MachineDescriptor* {
                LogRegistry::instance().getLogger("system")->debug("Factory lambda called for machine");
                return JsonMachineLoader::buildFromSpec(specCopy);
            }, displayName);
        ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------
// buildFromSpec — Phase 2 + Phase 3
// ---------------------------------------------------------------------------

MachineDescriptor* JsonMachineLoader::buildFromSpec(const nlohmann::json& spec) {
    std::cerr << "\n========== [JsonMachineLoader::buildFromSpec] CALLED ==========\n" << std::flush;
    std::cerr << "Spec contents: " << spec.dump() << "\n" << std::flush;
    auto* desc = new MachineDescriptor();

    desc->machineId        = spec.value("id",          "");
    desc->displayName      = spec.value("displayName",  "");
    desc->licenseClass     = spec.value("licenseClass", "");
    desc->preferredAssembler = spec.value("assembler",  "");
    desc->sourceSpec       = spec;
    std::cerr << "[JsonMachineLoader::buildFromSpec] Machine ID: " << desc->machineId << "\n" << std::flush;

    // -----------------------------------------------------------------------
    // Step 3 — Bus
    // -----------------------------------------------------------------------
    FlatMemoryBus*   bus      = nullptr;  // non-null for FlatMemoryBus machines (C64, VIC-20, etc.)
    SparseMemoryBus* physBus  = nullptr;  // non-null for SparseMemoryBus machines (MEGA65)
    IBus*            cpuBus   = nullptr;  // the bus the CPU sees (may be MapMmu or FlatMemoryBus)
    std::map<std::string, IBus*> busPtrs;

    if (spec.contains("bus")) {
        const auto& busSpec = spec["bus"];
        std::string busType  = busSpec.value("type", "FlatMemoryBus");
        std::string busName  = busSpec.value("name", "system");
        int         addrBits = busSpec.value("addrBits", 16);

        if (busType == "SparseMemoryBus") {
            physBus = new SparseMemoryBus(busName, addrBits);
            cpuBus  = physBus;
            desc->buses.push_back({busName, physBus});
            busPtrs[busName] = physBus;
        } else {
            bus    = new FlatMemoryBus(busName, addrBits);
            cpuBus = bus;
            desc->buses.push_back({busName, bus});
            busPtrs[busName] = bus;
        }
        std::cerr << "[JsonMachineLoader] Created bus: " << busName
                  << " (type=" << busType << ", " << addrBits << "-bit)\n" << std::flush;
    } else {
        std::cerr << "[JsonMachineLoader] ERROR: No 'bus' field in spec for " << desc->machineId << "\n" << std::flush;
    }

    // -----------------------------------------------------------------------
    // Step 3 — CPU
    // -----------------------------------------------------------------------
    ICore* cpu = nullptr;

    if (spec.contains("cpu")) {
        std::cerr << "[JsonMachineLoader] spec contains 'cpu' field\n" << std::flush;
        const auto& cpuSpec = spec["cpu"];
        std::string cpuType = cpuSpec.value("type", "");
        std::cerr << "[JsonMachineLoader] Looking for CPU type: '" << cpuType << "'\n" << std::flush;
        cpu = CoreRegistry::instance().createCore(cpuType);

        if (cpu) {
            std::cerr << "[JsonMachineLoader] CPU created successfully\n" << std::flush;
            std::string dataBusName = cpuSpec.value("dataBus", "system");
            std::string codeBusName = cpuSpec.value("codeBus", "system");
            IBus* dataBus = busPtrs.count(dataBusName) ? busPtrs[dataBusName] : cpuBus;
            IBus* codeBus = busPtrs.count(codeBusName) ? busPtrs[codeBusName] : cpuBus;
            cpu->setDataBus(dataBus);
            cpu->setCodeBus(codeBus);
            desc->cpus.push_back({"main", cpu, dataBus, codeBus, nullptr, true, 1});
            std::cerr << "[JsonMachineLoader] CPU added to cpus list (" << desc->cpus.size() << " CPUs total)\n" << std::flush;
        } else {
            std::cerr << "[JsonMachineLoader] ERROR: Failed to create CPU of type '" << cpuType << "'\n" << std::flush;
        }
    } else {
        std::cerr << "[JsonMachineLoader] ERROR: No 'cpu' field in spec\n" << std::flush;
    }

    // -----------------------------------------------------------------------
    // Step 6 — IORegistry (created before devices so handlers can register)
    // -----------------------------------------------------------------------
    auto* io = new IORegistry();
    desc->ioRegistry = io;

    // -----------------------------------------------------------------------
    // Step 4 — ROM allocation and loading
    // -----------------------------------------------------------------------
    std::map<std::string, std::pair<uint8_t*, uint32_t>> romByLabel; // label → {ptr, size}

    if (spec.contains("roms")) {
        for (const auto& romSpec : spec["roms"]) {
            std::string label    = romSpec.value("label",    "");
            std::string path     = romSpec.value("path",     "");
            int         size     = romSpec.value("size",     0);
            bool        writable = romSpec.value("writable", false);

            if (size <= 0) continue;

            auto* ptr = new uint8_t[size]();
            desc->roms.push_back(ptr);
            if (!label.empty()) romByLabel[label] = {ptr, (uint32_t)size};

            if (!path.empty())
                romLoad(PathUtil::findResource(path).c_str(), ptr, (size_t)size);

            if (romSpec.contains("overlayAddr") && !romSpec["overlayAddr"].is_null()) {
                uint32_t addr = hexStrToU32(romSpec["overlayAddr"].get<std::string>());
                if (bus)     bus->addOverlay(addr, (uint32_t)size, ptr, writable);
                if (physBus) physBus->addRomOverlay(addr, (uint32_t)size, ptr);
            }
        }
    }

    // -----------------------------------------------------------------------
    // symbolFiles parsing
    // -----------------------------------------------------------------------
    if (spec.contains("symbolFiles") && spec["symbolFiles"].is_array()) {
        for (const auto& symPath : spec["symbolFiles"]) {
            desc->symbolFiles.push_back(PathUtil::findResource(symPath.get<std::string>()));
        }
    }

    // -----------------------------------------------------------------------
    // Step 5 — Device creation
    // -----------------------------------------------------------------------
    std::map<std::string, IOHandler*> devPtrs;
    uint8_t* colorRamBuf  = nullptr; // buffer for inline_color_ram, wired to video devices

    if (spec.contains("devices")) {
        for (const auto& devSpec : spec["devices"]) {
            if (devSpec.value("_same", false)) continue;

            std::string type    = devSpec.value("type", "");
            std::string devName = devSpec.value("name", "");

            // Phase 3 Step 10 — inline_color_ram
            if (type == "inline_color_ram") {
                uint32_t base = devSpec.contains("baseAddr") && !devSpec["baseAddr"].is_null()
                    ? hexStrToU32(devSpec["baseAddr"].get<std::string>()) : 0x9400;
                uint32_t size = devSpec.value("size", 1024);
                colorRamBuf = new uint8_t[size]();
                desc->roms.push_back(colorRamBuf);
                io->registerOwnedHandler(new ColorRamHandler(base, size, colorRamBuf));
                continue;
            }

            if (type.size() >= 7 && type.substr(0, 7) == "inline_") continue;

            IOHandler* dev = DeviceRegistry::instance().createDevice(type);
            if (!dev) continue;

            dev->setName(devName);

            if (devSpec.contains("baseAddr") && !devSpec["baseAddr"].is_null())
                dev->setBaseAddr(hexStrToU32(devSpec["baseAddr"].get<std::string>()));
            if (devSpec.contains("clockHz") && !devSpec["clockHz"].is_null())
                dev->setClockHz(devSpec["clockHz"].get<uint32_t>());

            devPtrs[devName] = dev;
            io->registerOwnedHandler(dev);
        }
    }

    // -----------------------------------------------------------------------
    // Phase 3 Step 5.5 — ROM passToDevice
    // -----------------------------------------------------------------------
    if (spec.contains("roms")) {
        for (const auto& romSpec : spec["roms"]) {
            if (!romSpec.contains("passToDevice")) continue;
            std::string label = romSpec.value("label", "");
            auto it = romByLabel.find(label);
            if (it == romByLabel.end()) continue;
            uint8_t* ptr  = it->second.first;
            uint32_t size = it->second.second;
            for (const auto& targ : romSpec["passToDevice"]) {
                std::string tname = targ.get<std::string>();
                if (!devPtrs.count(tname)) continue;
                IOHandler* dev = devPtrs[tname];
                if      (label == "char"   || label == "char_rom") dev->setCharRom (ptr, size);
                else if (label == "basic")                          dev->setBasicRom(ptr, size);
                else if (label == "kernal")                         dev->setKernalRom(ptr, size);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Phase 3 Step 5.6 — Device-specific extra wiring
    // -----------------------------------------------------------------------
    if (spec.contains("devices")) {
        for (const auto& devSpec : spec["devices"]) {
            if (devSpec.value("_same", false)) continue;
            std::string type    = devSpec.value("type", "");
            std::string devName = devSpec.value("name", "");
            if (type == "inline_color_ram") continue;
            if (type.size() >= 7 && type.substr(0, 7) == "inline_") continue;
            if (!devPtrs.count(devName)) continue;
            IOHandler* dev = devPtrs[devName];

            // DMA bus — for SparseMemoryBus, use physical bus directly;
            // for FlatMemoryBus, wrap to bypass I/O hooks.
            if (devSpec.contains("dmaBus") && !devSpec["dmaBus"].is_null()) {
                if (physBus) {
                    dev->setDmaBus(physBus);
                } else if (bus) {
                    auto* dmaBus = new DmaBus(bus);
                    dev->setDmaBus(dmaBus);
                    desc->deleters.push_back([dmaBus]() { delete dmaBus; });
                }
            }

            // Color RAM buffer wiring (passBufTo on inline_color_ram)
            if (devSpec.contains("colorRam") && !devSpec["colorRam"].is_null() && colorRamBuf) {
                dev->setColorRam(colorRamBuf);
            }

            // RAM data for PLA banking
            if (devSpec.value("ramData", "") == "system" && bus) {  // FlatMemoryBus only
                dev->setRamData(bus->rawData());
            }

            // CIA2 VIC-II bank-select callback
            if (devSpec.value("portAWriteCallback", "") == "vic2_bank_select") {
                std::string tgtName = devSpec.value("bankSelectTarget", "VIC-II");
                if (devPtrs.count(tgtName)) {
                    IOHandler* tgt = devPtrs[tgtName];
                    dev->setPortAWriteCallback([tgt](uint8_t pra, uint8_t ddra) {
                        uint8_t eff = (pra & ddra) | (~ddra & 0xFF);
                        tgt->setBankBase((uint32_t)(~eff & 0x03) * 0x4000);
                    });
                }
            }

            // Generic device-to-device links (e.g. PetVideo → CRTC6545)
            if (devSpec.contains("linkedDevices") && devSpec["linkedDevices"].is_object()) {
                for (auto& [role, tgtVal] : devSpec["linkedDevices"].items()) {
                    std::string tgtName = tgtVal.get<std::string>();
                    if (devPtrs.count(tgtName))
                        dev->setLinkedDevice(role.c_str(), devPtrs[tgtName]);
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Phase 3 Step 5.7 — plaWiring: CPU signal lines → PLA device
    // -----------------------------------------------------------------------
    if (spec.contains("plaWiring") && cpu) {
        const auto& pw = spec["plaWiring"];
        std::string plaDevName = pw.value("device", "");
        if (devPtrs.count(plaDevName)) {
            IOHandler* pla = devPtrs[plaDevName];
            if (pw.contains("signals")) {
                for (auto& [devSigName, cpuSigName] : pw["signals"].items()) {
                    ISignalLine* sig = cpu->getSignalLine(
                        cpuSigName.get<std::string>().c_str());
                    if (sig) pla->setSignalLine(devSigName.c_str(), sig);
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Phase 14 — tapeWiring: Datasette <-> CPU Port / CIA / VIA
    // -----------------------------------------------------------------------
    if (spec.contains("tapeWiring") && !spec["tapeWiring"].is_null()) {
        const auto& tw = spec["tapeWiring"];
        std::string tapeDevName = tw.value("device", "");
        if (devPtrs.count(tapeDevName)) {
            auto* tape = devPtrs[tapeDevName];

            auto getSig = [&](const std::string& key) -> ISignalLine* {
                if (!tw.contains(key)) return nullptr;
                std::string srcStr = tw[key].get<std::string>();
                auto dot = srcStr.find(".");
                if (dot != std::string::npos) {
                    std::string devName = srcStr.substr(0, dot);
                    std::string subPin  = srcStr.substr(dot + 1);
                    if (devPtrs.count(devName))
                        return devPtrs[devName]->getSignalLine(subPin.c_str());
                } else if (cpu) {
                    return cpu->getSignalLine(srcStr.c_str());
                }
                return nullptr;
            };

            ISignalLine* sig;
            if ((sig = getSig("sense")))     tape->setSignalLine("sense", sig);
            if ((sig = getSig("motor")))     tape->setSignalLine("motor", sig);
            if ((sig = getSig("write")))     tape->setSignalLine("write", sig);
            if ((sig = getSig("readPulse"))) tape->setSignalLine("readPulse", sig);
        }
    }

    // Phase 3 Step 12 — ramExpansions → OpenBusHandler for absent blocks
    // -----------------------------------------------------------------------
    if (spec.contains("ramExpansions")) {
        for (const auto& blk : spec["ramExpansions"]) {
            if (blk.value("installed", true)) continue;
            if (!blk.contains("base") || !blk.contains("size")) continue;
            uint32_t base = hexStrToU32(blk["base"].get<std::string>());
            uint32_t size = blk.value("size", 0);
            if (size == 0) continue;
            std::string name = blk.value("name", "OpenBus");
            io->registerOwnedHandler(new OpenBusHandler(base, size, name));
        }
    }

    // -----------------------------------------------------------------------
    // Phase 3 Step 14 — ioHook.mirrorRanges
    // -----------------------------------------------------------------------
    struct MirrorRange { uint32_t from, to, targetBase; IOHandler* target; };
    std::vector<MirrorRange> mirrors;

    if (spec.contains("ioHook") && spec["ioHook"].contains("mirrorRanges")) {
        for (const auto& mr : spec["ioHook"]["mirrorRanges"]) {
            if (!mr.contains("from") || !mr.contains("to") || !mr.contains("target")) continue;
            uint32_t from = hexStrToU32(mr["from"].get<std::string>());
            uint32_t to   = hexStrToU32(mr["to"].get<std::string>());
            std::string tname = mr["target"].get<std::string>();
            uint32_t targetBase = mr.contains("targetBase")
                ? hexStrToU32(mr["targetBase"].get<std::string>()) : from;
            if (devPtrs.count(tname))
                mirrors.push_back({from, to, targetBase, devPtrs[tname]});
        }
    }

    // -----------------------------------------------------------------------
    // Step 6 — Bus I/O hook (with optional mirror ranges)
    // -----------------------------------------------------------------------
    // FlatMemoryBus: set I/O hooks directly on the bus
    if (bus) {
        if (mirrors.empty()) {
            bus->setIoHooks(
                [io](IBus* b, uint32_t a, uint8_t* v) { return io->dispatchRead (b, a, v); },
                [io](IBus* b, uint32_t a, uint8_t  v) { return io->dispatchWrite(b, a, v); });
        } else {
            bus->setIoHooks(
                [io, mirrors](IBus* b, uint32_t a, uint8_t* v) {
                    for (const auto& mr : mirrors)
                        if (a >= mr.from && a <= mr.to)
                            return mr.target->ioRead(b, mr.targetBase + (a - mr.from), v);
                    return io->dispatchRead(b, a, v);
                },
                [io, mirrors](IBus* b, uint32_t a, uint8_t v) {
                    for (const auto& mr : mirrors)
                        if (a >= mr.from && a <= mr.to)
                            return mr.target->ioWrite(b, mr.targetBase + (a - mr.from), v);
                    return io->dispatchWrite(b, a, v);
                });
        }

        bus->setHaltCheck([io]() {
            if (!io) return false;
            std::vector<IOHandler*> handlers;
            io->enumerate(handlers);
            for (auto* h : handlers) {
                if (h->isHaltRequested()) return true;
            }
            return false;
        });

        if (io) {
            bus->setIoLowBase(io->lowestHandlerBase());
        }
    }
    // SparseMemoryBus machines: MapMmu and I/O hooks are wired by the
    // machine's plugin factory (not the generic loader), since MapMmu is
    // a plugin-level class.

    // -----------------------------------------------------------------------
    // Step 7 / Phase 3 Step 13 — Signal lines
    // -----------------------------------------------------------------------
    std::map<std::string, ISignalLine*> signalMap;

    if (spec.contains("signals")) {
        for (const auto& sigSpec : spec["signals"]) {
            std::string sigName = sigSpec.value("name", "");
            std::string sigType = sigSpec.value("type", "simple");

            // Phase 3: latching signal (e.g. vsync)
            if (sigType == "latch") {
                auto* line = new SimpleSignalLine();
                signalMap[sigName] = line;
                desc->deleters.push_back([line]() { delete line; });
                continue;
            }

            // Phase 2: simple IRQ / NMI
            if (sigType == "simple" || sigType == "nmi") {
                if (!cpu) continue;
                ISignalLine* line = nullptr;
                bool isNmi = (sigType == "nmi" || sigName == "nmi");
                line = isNmi ? (ISignalLine*)new CpuNmiLine(cpu)
                             : (ISignalLine*)new CpuIrqLine(cpu);
                signalMap[sigName] = line;
                desc->deleters.push_back([line]() { delete line; });

                if (sigSpec.contains("sources")) {
                    for (const auto& srcEntry : sigSpec["sources"]) {
                        std::string srcName = srcEntry.get<std::string>();
                        if (!devPtrs.count(srcName)) continue;
                        if (isNmi)
                            devPtrs[srcName]->setNmiLine(line);
                        else
                            devPtrs[srcName]->setIrqLine(line);
                    }
                }
                continue;
            }

            // Phase 3 Step 13: shared wired-OR IRQ
            if (sigType == "shared" && cpu) {
                auto* mgr = new SharedIrqManager(cpu);
                desc->deleters.push_back([mgr]() { delete mgr; });

                if (sigSpec.contains("sources")) {
                    for (const auto& srcEntry : sigSpec["sources"]) {
                        std::string srcStr = srcEntry.get<std::string>();
                        ISignalLine* line = mgr->createLine();

                        // Dot notation: "DEVNAME.subpin"
                        auto dot = srcStr.find('.');
                        if (dot != std::string::npos) {
                            std::string devName = srcStr.substr(0, dot);
                            std::string subPin  = srcStr.substr(dot + 1);
                            if (!devPtrs.count(devName)) continue;
                            IOHandler* dev = devPtrs[devName];
                            if      (subPin == "irqA") dev->setIrqALine(line);
                            else if (subPin == "irqB") dev->setIrqBLine(line);
                            else                       dev->setIrqLine(line);
                        } else {
                            if (devPtrs.count(srcStr))
                                devPtrs[srcStr]->setIrqLine(line);
                        }
                    }
                }
                continue;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Phase 3 Step 15.5 — Device lines wiring (CA1/CB1/irqA/irqB)
    // -----------------------------------------------------------------------
    if (spec.contains("devices")) {
        for (const auto& devSpec : spec["devices"]) {
            if (!devSpec.contains("lines")) continue;
            std::string devName = devSpec.value("name", "");
            if (!devPtrs.count(devName)) continue;
            IOHandler* dev = devPtrs[devName];
            for (auto& [lineName, sigRef] : devSpec["lines"].items()) {
                std::string sigStr = sigRef.get<std::string>();
                if (!signalMap.count(sigStr)) continue;
                ISignalLine* sig = signalMap[sigStr];
                if      (lineName == "ca1")  dev->setCA1Line(sig);
                else if (lineName == "cb1")  dev->setCB1Line(sig);
                else if (lineName == "ca2")  dev->setCA2Line(sig);
                else if (lineName == "cb2")  dev->setCB2Line(sig);
                else if (lineName == "irqA") dev->setIrqALine(sig);
                else if (lineName == "irqB") dev->setIrqBLine(sig);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Phase 3 Step 19 — Keyboard wiring
    // -----------------------------------------------------------------------
    if (spec.contains("kbdWiring") && !spec["kbdWiring"].is_null()) {
        const auto& kw = spec["kbdWiring"];
        std::string kbdDevName = kw.value("device", "");
        if (devPtrs.count(kbdDevName)) {
            IKeyboardMatrix* kbd =
                dynamic_cast<IKeyboardMatrix*>(devPtrs[kbdDevName]);
            if (kbd) {
                if (kw.contains("colPort")) {
                    const auto& cp = kw["colPort"];
                    std::string colDev  = cp.value("device", "");
                    std::string colPort = cp.value("port",   "B");
                    if (devPtrs.count(colDev)) {
                        if (colPort == "A")
                            devPtrs[colDev]->setPortADevice(kbd->getPort(0));
                        else
                            devPtrs[colDev]->setPortBDevice(kbd->getPort(0));
                    }
                }
                if (kw.contains("rowPort")) {
                    const auto& rp = kw["rowPort"];
                    std::string rowDev  = rp.value("device", "");
                    std::string rowPort = rp.value("port",   "A");
                    if (devPtrs.count(rowDev)) {
                        if (rowPort == "B")
                            devPtrs[rowDev]->setPortBDevice(kbd->getPort(1));
                        else
                            devPtrs[rowDev]->setPortADevice(kbd->getPort(1));
                    }
                }
                desc->onKey = [kbd](const std::string& keyName, bool down) {
                    return kbd->pressKeyByName(keyName, down);
                };
            }
        }
    }

    // -----------------------------------------------------------------------
    // Phase 3 Step 21 — Joystick wiring
    // -----------------------------------------------------------------------
    if (spec.contains("joystick") && !spec["joystick"].is_null()) {
        const auto& jw = spec["joystick"];
        std::string joyDev  = jw.value("device", "");
        std::string joyPort = jw.value("port",   "B");
        if (devPtrs.count(joyDev)) {
            auto* joy = new Joystick();
            desc->deleters.push_back([joy]() { delete joy; });
            if (joyPort == "A")
                devPtrs[joyDev]->setPortADevice(joy);
            else
                devPtrs[joyDev]->setPortBDevice(joy);
            desc->onJoystick = [joy](int port, uint8_t bits) {
                if (port == 0) joy->setState(bits);
            };
        }
    }

    // -----------------------------------------------------------------------
    // Phase 15.2 — iecWiring: VirtualIEC <-> CIA2 Port A
    // -----------------------------------------------------------------------
    if (spec.contains("iecWiring") && !spec["iecWiring"].is_null()) {
        const auto& iw = spec["iecWiring"];
        std::string iecDevName = iw.value("device", "");
        if (devPtrs.count(iecDevName)) {
            // Note: VirtualIECBus is an IOHandler that also implements IPortDevice
            IPortDevice* iec = dynamic_cast<IPortDevice*>(devPtrs[iecDevName]);
            if (iec) {
                if (iw.contains("busPort")) {
                    const auto& bp = iw["busPort"];
                    std::string portDev  = bp.value("device", "");
                    std::string portName = bp.value("port",   "A");
                    if (devPtrs.count(portDev)) {
                        fprintf(stderr, "[IEC Wiring] Connecting %s to %s port %s (dev=%p, iec=%p)\n",
                                iecDevName.c_str(), portDev.c_str(), portName.c_str(),
                                (void*)devPtrs[portDev], (void*)iec);
                        fflush(stderr);
                        if (portName == "A")
                            devPtrs[portDev]->setPortADevice(iec);
                        else
                            devPtrs[portDev]->setPortBDevice(iec);
                    }
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Step 8 — Scheduler (simple step_tick or frame-accumulator)
    // -----------------------------------------------------------------------
    bool hasFrameAccum = false;
    SimpleSignalLine* vsyncLine = nullptr;
    uint64_t frameCycles = 20000;

    if (spec.contains("scheduler")) {
        const auto& sched = spec["scheduler"];
        hasFrameAccum = sched.contains("frameAccumulator") &&
                        !sched["frameAccumulator"].is_null();
        if (hasFrameAccum) {
            const auto& fa = sched["frameAccumulator"];
            frameCycles = (uint64_t)fa.value("cycles", 20000);
            std::string vsyncSig = fa.value("signal", "vsync");
            // Use an existing latch signal if declared, else create one
            if (signalMap.count(vsyncSig)) {
                vsyncLine = dynamic_cast<SimpleSignalLine*>(signalMap[vsyncSig]);
            }
            if (!vsyncLine) {
                auto* newVsync = new SimpleSignalLine();
                vsyncLine = newVsync;
                desc->deleters.push_back([newVsync]() { delete newVsync; });
            }
        }
    }

    if (hasFrameAccum && vsyncLine) {
        auto* frameAccum = new uint64_t(0);
        desc->deleters.push_back([frameAccum]() { delete frameAccum; });
        SimpleSignalLine* vsync = vsyncLine;
        uint64_t cycles = frameCycles;
        desc->schedulerStep = [frameAccum, vsync, cycles](MachineDescriptor& d) {
            int total = 0;
            for (auto& s : d.cpus) {
                if (s.active && s.cpu) {
                    total += s.cpu->step();
                    if (d.ioRegistry) d.ioRegistry->tickAll(total);
                }
            }
            *frameAccum += (uint64_t)total;
            if (*frameAccum >= cycles) {
                *frameAccum -= cycles;
                vsync->set(false);
                if (d.ioRegistry) d.ioRegistry->tickAll(0);
                vsync->set(true);
                if (d.ioRegistry) d.ioRegistry->tickAll(0);
            }
            return total;
        };
    } else {
        desc->schedulerStep = [](MachineDescriptor& d) {
            int total = 0;
            for (auto& s : d.cpus) {
                if (s.active && s.cpu) {
                    total += s.cpu->step();
                    if (d.ioRegistry) d.ioRegistry->tickAll(total);
                }
            }
            return total;
        };
    }

    // -----------------------------------------------------------------------
    // Step 9 — onReset
    // -----------------------------------------------------------------------
    desc->onReset = [](MachineDescriptor& d) {
        if (d.ioRegistry) d.ioRegistry->resetAll();
        for (auto& slot : d.cpus)
            if (slot.cpu) slot.cpu->reset();
    };

    return desc;
}
