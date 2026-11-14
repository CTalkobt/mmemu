#include <cstdio>
#include "ram_init_pattern.h"
#include <memory>
#include <algorithm>
#include "json_machine_loader.h"
#include "util/path_util.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/machines/machine_registry.h"
#include "libcore/main/core_registry.h"
#include "libcore/main/rom_loader.h"
#include "libcore/main/intercept.h"
#include "libmem/main/memory_bus.h"
#include "libmem/main/sparse_memory_bus.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/device_registry.h"
#include "libdevices/main/isignal_line.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/joystick.h"
#include "libdevices/main/combined_port_device.h"
#include "libdevices/main/color_ram_handler.h"
#include "libdevices/main/open_bus_handler.h"
#include "libdevices/main/shared_irq_manager.h"
#include "libdevices/main/simple_signal_line.h"
#include "libdevices/main/ipointer.h"
#include "libdevices/main/iba_source.h"
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
        bool rising = level && !m_level;    // NMI is edge-triggered
        m_level = level;
        if (rising && m_cpu) m_cpu->triggerNmi();
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
    auto log = LogRegistry::instance().getLogger("system");
    auto* desc = new MachineDescriptor();

    desc->machineId        = spec.value("id",          "");
    desc->displayName      = spec.value("displayName",  "");
    desc->licenseClass     = spec.value("licenseClass", "");
    if (spec.contains("clockHz") && spec["clockHz"].is_number())
        desc->clockHz      = spec["clockHz"].get<double>();   // real-time pacing (#169)
    desc->preferredAssembler = spec.value("assembler",  "");
    desc->sourceSpec       = spec;
    log->debug("JsonMachineLoader: building '{}'", desc->machineId);

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
            // Power-up RAM as a pattern (VICE ram_init_with_pattern): "c64",
            // or {"startValue", "valueInvert", "valueOffset", "patternInvert",
            // "patternInvertValue"}; without it, zeroes
            if (busSpec.contains("ramInit")) {
                const auto& ri = busSpec["ramInit"];
                RamInitParam p;
                if (ri.is_string() && ri.get<std::string>() == "c64") p = ramInitC64();
                else if (ri.is_object()) {
                    p.startValue = ri.value("startValue", 0);
                    p.valueInvert = ri.value("valueInvert", 0);
                    p.valueOffset = ri.value("valueOffset", 0);
                    p.patternInvert = ri.value("patternInvert", 0);
                    p.patternInvertValue = ri.value("patternInvertValue", 0);
                } else log->error("JsonMachineLoader: bus.ramInit: \"c64\" or an object of pattern values");
                std::vector<uint8_t> ram((size_t)1 << addrBits);
                ramInitPattern(ram.data(), ram.size(), p);
                for (size_t a = 0; a < ram.size(); ++a) bus->write8((uint32_t)a, ram[a]);
            }
        }
        log->debug("JsonMachineLoader: bus {} ({}, {}-bit)", busName, busType, addrBits);
    } else {
        log->error("JsonMachineLoader: no 'bus' field in spec for {}", desc->machineId);
    }

    // -----------------------------------------------------------------------
    // Step 3 — CPU
    // -----------------------------------------------------------------------
    ICore* cpu = nullptr;

    if (spec.contains("cpu")) {
        const auto& cpuSpec = spec["cpu"];
        std::string cpuType = cpuSpec.value("type", "");
        cpu = CoreRegistry::instance().createCore(cpuType);

        if (cpu) {
            std::string dataBusName = cpuSpec.value("dataBus", "system");
            std::string codeBusName = cpuSpec.value("codeBus", "system");
            IBus* dataBus = busPtrs.count(dataBusName) ? busPtrs[dataBusName] : cpuBus;
            IBus* codeBus = busPtrs.count(codeBusName) ? busPtrs[codeBusName] : cpuBus;
            cpu->setDataBus(dataBus);
            cpu->setCodeBus(codeBus);
            desc->cpus.push_back({"main", cpu, dataBus, codeBus, nullptr, true, 1});
            log->debug("JsonMachineLoader: CPU {}", cpuType);
        } else {
            log->error("JsonMachineLoader: failed to create CPU of type '{}'", cpuType);
        }
    } else {
        log->error("JsonMachineLoader: no 'cpu' field in spec for {}", desc->machineId);
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
            // Device settings: "options": { "model": "6526A" }
            if (devSpec.contains("options") && devSpec["options"].is_object()) {
                for (const auto& [key, value] : devSpec["options"].items()) {
                    std::string err;
                    if (!value.is_string() || !dev->setOption(key, value.get<std::string>(), err))
                        fprintf(stderr, "machine JSON: device %s: option %s: %s\n", devName.c_str(), key.c_str(),
                                err.empty() ? "not a string" : err.c_str());
                }
            }

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

            // CIA1 PB4 driven low: the VIC-II's light pen line (VICE c64cia1.c)
            if (devSpec.value("portBWriteCallback", "") == "light_pen") {
                std::string tgtName = devSpec.value("lightPenTarget", "VIC-II");
                if (devPtrs.count(tgtName)) {
                    IOHandler* tgt = devPtrs[tgtName];
                    dev->setPortBWriteCallback([tgt](uint8_t prb, uint8_t ddrb) {
                        tgt->setLightPen((ddrb & 0x10) && !(prb & 0x10));
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

    // Phase 3 Step 12 — ramExpansions → OpenBusHandler for absent blocks
    // -----------------------------------------------------------------------
    // With "ramExpansionSlot" the blocks can be fitted and taken out at run
    // time (an expansion slot, e.g. the VIC-20's "ram"): an absent block's
    // open-bus handler is registered, a fitted one's is not.
    struct RamBlock { std::string name; uint32_t base, size; OpenBusHandler* openBus; bool fitted; };
    auto ramBlocks = std::make_shared<std::vector<RamBlock>>();
    if (spec.contains("ramExpansions")) {
        for (const auto& blk : spec["ramExpansions"]) {
            if (!blk.contains("base") || !blk.contains("size")) continue;
            uint32_t base = hexStrToU32(blk["base"].get<std::string>());
            uint32_t size = blk.value("size", 0);
            if (size == 0) continue;
            std::string name = blk.value("name", "OpenBus");
            const bool fitted = blk.value("installed", true);
            auto* ob = new OpenBusHandler(base, size, name);
            if (!fitted) io->registerHandler(ob);
            ramBlocks->push_back({name, base, size, ob, fitted});
        }
        desc->deleters.push_back([ramBlocks]() { for (auto& b : *ramBlocks) delete b.openBus; });
    }
    if (spec.contains("ramExpansionSlot") && !ramBlocks->empty()) {
        //   "ramExpansionSlot": { "name": "ram", "description": "...",
        //     "presets": [ { "name": "3K", "blocks": ["BLK0"] }, ... ] }
        // Options: a preset, or the blocks by name or number ("BLK1,BLK5", "1,5")
        const auto& rs = spec["ramExpansionSlot"];
        auto presets = std::make_shared<std::vector<std::pair<std::string, std::vector<std::string>>>>();
        std::vector<std::string> optionNames;
        for (const auto& p : rs.value("presets", nlohmann::json::array())) {
            std::vector<std::string> blocks;
            for (const auto& b : p.value("blocks", nlohmann::json::array())) blocks.push_back(b.get<std::string>());
            presets->push_back({p.value("name", ""), blocks});
            optionNames.push_back(p.value("name", ""));
        }
        MachineDescriptor::ExpansionSlot slot;
        slot.name = rs.value("name", "ram");
        slot.description = rs.value("description", "RAM expansion blocks");
        std::string blockList;
        for (const auto& b : *ramBlocks) blockList += (blockList.empty() ? "" : ", ") + b.name;
        const std::string defPreset = rs.value("default", optionNames.empty() ? std::string() : optionNames.front());
        slot.devices.push_back({"ram", "RAM in the expansion blocks (" + blockList + "): a preset, or blocks such as \"1,5\"",
                                optionNames, defPreset});
        IBus* ramBus = desc->buses.empty() ? nullptr : desc->buses[0].bus;
        slot.install = [ramBlocks, presets, io, ramBus, defPreset](const std::string& device, const std::string& option, std::string& err) {
            if (!device.empty() && device != "ram") { err = "no device '" + device + "' for this slot (ram)"; return false; }
            std::vector<bool> want(ramBlocks->size(), false);
            std::string opt = device.empty() ? "" : option;
            if (!device.empty()) {
                if (opt.empty()) opt = defPreset;
                auto upper = [](std::string x) { for (auto& c : x) c = (char)toupper((unsigned char)c); return x; };
                const std::vector<std::string>* names = nullptr;
                for (const auto& p : *presets) if (upper(p.first) == upper(opt)) names = &p.second;
                std::vector<std::string> list;
                if (names) list = *names;
                else {                                  // "BLK1,BLK5" or "1,5"
                    std::string cur;
                    for (char c : opt + ",") {
                        if (c == ',' || c == ' ') { if (!cur.empty()) list.push_back(cur); cur.clear(); }
                        else cur += c;
                    }
                }
                for (const auto& n : list) {
                    bool found = false;
                    for (size_t i = 0; i < ramBlocks->size(); ++i) {
                        const std::string& bn = (*ramBlocks)[i].name;
                        if (upper(bn) == upper(n) || upper(bn) == "BLK" + upper(n)) { want[i] = true; found = true; }
                    }
                    if (!found) {
                        std::string ps;
                        for (const auto& p : *presets) ps += (ps.empty() ? "" : ", ") + p.first;
                        err = "'" + opt + "' is neither a preset (" + ps + ") nor a list of blocks";
                        return false;
                    }
                }
            }
            for (size_t i = 0; i < ramBlocks->size(); ++i) {
                auto& b = (*ramBlocks)[i];
                if (want[i] == b.fitted) continue;
                if (want[i]) {
                    io->unregisterHandler(b.openBus);
                    if (ramBus) for (uint32_t a = 0; a < b.size; ++a) ramBus->write8(b.base + a, 0);
                } else {
                    io->registerHandler(b.openBus);
                }
                b.fitted = want[i];
            }
            return true;
        };
        slot.info = [ramBlocks, presets]() -> std::string {
            std::vector<std::string> fitted;
            uint32_t bytes = 0;
            for (const auto& b : *ramBlocks) if (b.fitted) { fitted.push_back(b.name); bytes += b.size; }
            if (fitted.empty()) return "";
            std::string names;
            for (const auto& n : fitted) names += (names.empty() ? "" : ", ") + n;
            for (const auto& p : *presets) if (p.second == fitted) return "ram (" + p.first + ": " + names + ")";
            return "ram (" + std::to_string(bytes / 1024) + "K: " + names + ")";
        };
        desc->expansionSlots.push_back(std::move(slot));
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
    // Open I/O ("openBus": {"from", "to"}): a read there that no device
    // answers returns what the video chip left on the bus (IBaSource::
    // phi1Value; the source is wired with the scheduler below). The C64's
    // IO1/IO2 without a cartridge (VICE testprogs C64/openio gauntlet)
    struct OpenBus { uint32_t from = 1, to = 0; IBaSource* src = nullptr; };
    auto openBus = std::make_shared<OpenBus>();
    if (spec.contains("openBus")) {
        openBus->from = hexStrToU32(spec["openBus"].value("from", "0x0"));
        openBus->to   = hexStrToU32(spec["openBus"].value("to", "0x0"));
    }
    auto openRead = [openBus](uint32_t a, uint8_t* v) {
        if (a < openBus->from || a > openBus->to || !openBus->src) return false;
        const int x = openBus->src->phi1Value();
        if (x < 0) return false;
        *v = (uint8_t)x;
        return true;
    };

    // FlatMemoryBus: set I/O hooks directly on the bus
    if (bus) {
        if (mirrors.empty()) {
            bus->setIoHooks(
                [io, openRead](IBus* b, uint32_t a, uint8_t* v) { return io->dispatchRead (b, a, v) || openRead(a, v); },
                [io](IBus* b, uint32_t a, uint8_t  v) { return io->dispatchWrite(b, a, v); },
                [io, openRead](IBus* b, uint32_t a, uint8_t* v) { return io->dispatchPeek (b, a, v) || openRead(a, v); });
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
                },
                [io, mirrors](IBus* b, uint32_t a, uint8_t* v) {
                    for (const auto& mr : mirrors)
                        if (a >= mr.from && a <= mr.to)
                            return mr.target->ioPeek(b, mr.targetBase + (a - mr.from), v);
                    return io->dispatchPeek(b, a, v);
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

            // Wired-OR NMI: each source is a chip's interrupt output ("DEV",
            // "DEV.irqA", "DEV.irqB") or its NMI output ("DEV.nmi"), as on the
            // VIC-20 (VIA1) and C64 (CIA2). "lines" names extra inputs, e.g.
            // "restore" for a RESTORE key wired straight to NMI.
            if (sigType == "sharedNmi" && cpu) {
                auto* mgr = new SharedNmiManager(cpu);
                desc->deleters.push_back([mgr]() { delete mgr; });
                for (const auto& srcEntry : sigSpec.value("sources", nlohmann::json::array())) {
                    std::string srcStr = srcEntry.get<std::string>();
                    auto dot = srcStr.find('.');
                    std::string devName = srcStr.substr(0, dot);
                    std::string subPin  = dot == std::string::npos ? "" : srcStr.substr(dot + 1);
                    if (!devPtrs.count(devName)) continue;
                    IOHandler* dev = devPtrs[devName];
                    ISignalLine* line = mgr->createLine();
                    if      (subPin == "irqA") dev->setIrqALine(line);
                    else if (subPin == "irqB") dev->setIrqBLine(line);
                    else if (subPin == "nmi")  dev->setNmiLine(line);
                    else                       dev->setIrqLine(line);
                }
                for (const auto& l : sigSpec.value("lines", nlohmann::json::array()))
                    signalMap[l.get<std::string>()] = mgr->createLine();
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
                // Extra inputs by name (e.g. "expansion_irq": the expansion
                // port's /IRQ, for a device fitted at run time)
                for (const auto& l : sigSpec.value("lines", nlohmann::json::array()))
                    signalMap[l.get<std::string>()] = mgr->createLine();
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
                ISignalLine* sig = nullptr;
                if (signalMap.count(sigStr)) {
                    sig = signalMap[sigStr];
                } else if (auto dot = sigStr.find('.'); dot != std::string::npos &&
                           devPtrs.count(sigStr.substr(0, dot))) {
                    // Another device's line, e.g. "IEEE.ndacOut"
                    sig = devPtrs[sigStr.substr(0, dot)]->getSignalLine(sigStr.substr(dot + 1).c_str());
                }
                if (!sig) continue;
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
                // Keys outside the matrix, wired to a line:
                //  "restore": RESTORE (VIC-20, C64), key name "restore";
                //  "nmiKey":  a plain NMI button (the PET, which had no
                //             RESTORE key), key name from "key" (default "nmi").
                // {"line": "<signal>"} is asserted while held;
                // {"line": "DEV.pin", "activeLow": true} is pulled low.
                auto wireKey = [&](const nlohmann::json& rw, const std::string& defaultKey) {
                    std::string src = rw.value("line", "");
                    std::string keyName = rw.value("key", defaultKey);
                    bool activeLow = rw.value("activeLow", false);
                    ISignalLine* line = nullptr;
                    auto dot = src.find('.');
                    if (dot != std::string::npos) {
                        std::string devName = src.substr(0, dot);
                        if (devPtrs.count(devName))
                            line = devPtrs[devName]->getSignalLine(src.substr(dot + 1).c_str());
                    } else if (signalMap.count(src)) {
                        line = signalMap[src];
                    }
                    if (!line) return;
                    if (activeLow) line->set(true);
                    auto keys = desc->onKey;
                    desc->onKey = [keys, line, activeLow, keyName](const std::string& name, bool down) {
                        std::string n = name;
                        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
                        if (n == keyName) { line->set(activeLow ? !down : down); return true; }
                        return keys(name, down);
                    };
                };
                if (kw.contains("restore")) wireKey(kw["restore"], "restore");
                if (kw.contains("nmiKey"))  wireKey(kw["nmiKey"], "nmi");
            }
        }
    }

    // -----------------------------------------------------------------------
    // Phase 3 Step 21 — Joystick wiring
    // -----------------------------------------------------------------------
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
                } else if (signalMap.count(srcStr)) {
                    return signalMap[srcStr];           // a declared signal (e.g. a latch wired to pins)
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

    // "ui": the hardware-dependent debugger panes and graphics formats
    if (spec.contains("ui") && spec["ui"].is_object()) {
        const auto& ui = spec["ui"];
        if (ui.contains("panes")) {
            desc->uiPanesSet = true;
            for (const auto& p : ui["panes"]) desc->uiPanes.push_back(p.get<std::string>());
        }
        for (const auto& b : ui.value("screenButtons", nlohmann::json::array())) {
            MachineDescriptor::ScreenButton sb;
            sb.label = b.value("label", "");
            sb.tooltip = b.value("tooltip", "");
            for (const auto& k : b.value("keys", nlohmann::json::array())) sb.keys.push_back(k.get<std::string>());
            if (!sb.label.empty() && !sb.keys.empty()) desc->screenButtons.push_back(sb);
        }
        if (ui.contains("graphicsFormats"))
            for (const auto& f : ui["graphicsFormats"]) desc->graphicsFormats.push_back(f.get<std::string>());
    }

    // "joystick": one port {device, port} or an array of them (port index =
    // array index); "joystickPrimary" names the port most software reads.
    // A port that already has a device (a keyboard matrix) shares the pins,
    // and outputs read the pins so a joystick can pull a driven line low.
    if (spec.contains("joystick") && !spec["joystick"].is_null()) {
        const auto& jw = spec["joystick"];
        std::vector<nlohmann::json> ports;
        if (jw.is_array()) for (const auto& p : jw) ports.push_back(p);
        else ports.push_back(jw);
        std::vector<Joystick*> joys;
        // Attach a port device, sharing the pins with one already there.
        auto attach = [&](IOHandler* chip, bool portA, IPortDevice* dev) {
            IPortDevice* existing = portA ? chip->portADevice() : chip->portBDevice();
            if (existing) {
                auto* both = new CombinedPortDevice();
                both->addDevice(existing);
                both->addDevice(dev);
                desc->deleters.push_back([both]() { delete both; });
                dev = both;
                chip->setOutputsReadPins(true);
            }
            if (portA) chip->setPortADevice(dev);
            else       chip->setPortBDevice(dev);
        };
        for (const auto& p : ports) {
            // {"lines": [{device, port, up|down|left|right|fire: bit}, ...]}:
            // one joystick whose switches are on chosen bits of several ports
            if (p.contains("lines")) {
                auto* joy = new Joystick();
                desc->deleters.push_back([joy]() { delete joy; });
                static const char* SWITCHES[5] = {"up", "down", "left", "right", "fire"};
                for (const auto& l : p["lines"]) {
                    std::string dev = l.value("device", "");
                    if (!devPtrs.count(dev)) continue;
                    int bitFor[5];
                    for (int i = 0; i < 5; i++) bitFor[i] = l.value(SWITCHES[i], -1);
                    auto* pins = new JoystickPins(joy, bitFor);
                    desc->deleters.push_back([pins]() { delete pins; });
                    attach(devPtrs[dev], l.value("port", "B") == "A", pins);
                }
                joys.push_back(joy);
                continue;
            }
            std::string joyDev  = p.value("device", "");
            std::string joyPort = p.value("port",   "B");
            if (!devPtrs.count(joyDev)) { joys.push_back(nullptr); continue; }
            IOHandler* chip = devPtrs[joyDev];
            auto* joy = new Joystick();
            desc->deleters.push_back([joy]() { delete joy; });
            attach(chip, joyPort == "A", joy);
            joys.push_back(joy);
        }
        desc->joystickPorts = (int)joys.size();
        desc->primaryJoystickPort = spec.value("joystickPrimary", 0);
        desc->onJoystick = [joys](int port, uint8_t bits) {
            if (port >= 0 && port < (int)joys.size() && joys[port]) joys[port]->setState(bits);
        };
    }

    // "portWiring": [{device, port: "A"|"B", from: "DEV.port"}]: attach a
    // device's named port (e.g. an IEEE-488 bus's "dataIn") to a chip port,
    // sharing it with a device already there (a keyboard).
    for (const auto& pw : spec.value("portWiring", nlohmann::json::array())) {
        std::string chipName = pw.value("device", ""), from = pw.value("from", "");
        auto dot = from.find('.');
        if (!devPtrs.count(chipName) || dot == std::string::npos || !devPtrs.count(from.substr(0, dot))) continue;
        IPortDevice* dev = devPtrs[from.substr(0, dot)]->namedPort(from.substr(dot + 1));
        if (!dev) continue;
        IOHandler* chip = devPtrs[chipName];
        bool portA = pw.value("port", "A") == "A";
        IPortDevice* existing = portA ? chip->portADevice() : chip->portBDevice();
        if (existing) {
            auto* both = new CombinedPortDevice();
            both->addDevice(existing);
            both->addDevice(dev);
            desc->deleters.push_back([both]() { delete both; });
            dev = both;
        }
        if (portA) chip->setPortADevice(dev);
        else       chip->setPortBDevice(dev);
    }

    // "pinWiring": [{device, port, bit, signal}]: a signal's level on one input
    // bit of a chip port (e.g. the PET's vertical retrace on VIA PB5), sharing
    // the port with what is already there.
    for (const auto& pw : spec.value("pinWiring", nlohmann::json::array())) {
        std::string chipName = pw.value("device", ""), sig = pw.value("signal", "");
        int bit = pw.value("bit", -1);
        if (!devPtrs.count(chipName) || !signalMap.count(sig) || bit < 0 || bit > 7) continue;
        struct SignalPin : IPortDevice {
            ISignalLine* line; uint8_t mask;
            SignalPin(ISignalLine* l, uint8_t m) : line(l), mask(m) {}
            uint8_t readPort() override { return line->get() ? 0xFF : (uint8_t)~mask; }
            void writePort(uint8_t) override {}
            void setDdr(uint8_t) override {}
        };
        IPortDevice* dev = new SignalPin(signalMap[sig], (uint8_t)(1 << bit));
        desc->deleters.push_back([dev]() { delete dev; });
        IOHandler* chip = devPtrs[chipName];
        bool portA = pw.value("port", "A") == "A";
        IPortDevice* existing = portA ? chip->portADevice() : chip->portBDevice();
        if (existing) {
            auto* both = new CombinedPortDevice();
            both->addDevice(existing);
            both->addDevice(dev);
            desc->deleters.push_back([both]() { delete both; });
            dev = both;
        }
        if (portA) chip->setPortADevice(dev);
        else       chip->setPortBDevice(dev);
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
                        log->debug("JsonMachineLoader: IEC {} -> {} port {}", iecDevName, portDev, portName);
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
    SimpleSignalLine* retraceLine = nullptr;    // low for the last retraceCycles of a frame
    uint64_t frameCycles = 20000, retraceCycles = 0;

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
            // A vertical retrace level (e.g. the PET's VIA PB5): low while the
            // frame is in its last retraceCycles cycles
            std::string rs = fa.value("retraceSignal", "");
            retraceCycles = (uint64_t)fa.value("retraceCycles", 0);
            if (!rs.empty() && signalMap.count(rs) && retraceCycles > 0 && retraceCycles < frameCycles)
                retraceLine = dynamic_cast<SimpleSignalLine*>(signalMap[rs]);
        }
    }

    // A device pulling BA (the C64's VIC-II, #266): "scheduler": {"baSource": "<device>"}
    IBaSource* baSource = nullptr;
    if (spec.contains("scheduler") && spec["scheduler"].contains("baSource")) {
        const std::string n = spec["scheduler"]["baSource"].get<std::string>();
        if (devPtrs.count(n)) baSource = dynamic_cast<IBaSource*>(devPtrs[n]);
        if (!baSource) log->error("JsonMachineLoader: scheduler.baSource: no device '{}' that pulls BA", n);
    }
    openBus->src = baSource;

    if (baSource) {
        // The CPU's read cycles wait while BA is low (RDY); its writes go on.
        // A DMA master (an REU) is given the source and waits itself.
        // The devices are brought up to each CPU access before it happens
        // (#304): the core calls back with the access's cycle.
        struct StepClock {
            MachineDescriptor* d = nullptr;
            ICore* cpu = nullptr;
            IBaSource* ba = nullptr;
            int i = 0;          // next CPU cycle of this instruction
            int t = 0;          // bus cycles so far, BA waits included
            int ticked = 0;     // bus cycles the devices have run
            uint32_t waited = 0; // CPU cycles that waited for BA (bit i: cycle i)
            // Through CPU cycle k: a read cycle waits while BA is low; `~k`:
            // through cycle k's waits, not the cycle (the CPU's RDY poll).
            // Returns the cycles so far that waited (SHA, ANE use it).
            uint32_t advance(int k) {
                const uint32_t writes = cpu->lastStepWrites();
                if (k < 0) {
                    k = ~k;
                    if (k > 0) advance(k - 1);
                    if (i == k && !(i < 32 && ((writes >> i) & 1)))
                        while (ba->baLow(t - ticked)) {
                            ++t;
                            if (i < 32) waited |= 1u << i;
                            if (d->ioRegistry) d->ioRegistry->tickAll(1);
                            ++ticked;
                        }
                    return waited;
                }
                while (i <= k) {
                    if (!(i < 32 && ((writes >> i) & 1)))
                        while (ba->baLow(t - ticked)) {
                            ++t;
                            if (i < 32) waited |= 1u << i;
                        }
                    ++t;
                    ++i;
                }
                if (t > ticked) {
                    if (d->ioRegistry) d->ioRegistry->tickAll(t - ticked);
                    ticked = t;
                }
                return waited;
            }
        };
        auto clock = std::make_shared<StepClock>();
        clock->d = desc;
        clock->ba = baSource;
        for (auto& s : desc->cpus)
            if (s.cpu) {
                clock->cpu = s.cpu;
                s.cpu->setAccessSync([clock](int k) { return clock->advance(k); });
                s.cpu->setPhi1Source([baSource]() { return baSource->phi1Value(); });
                ICore* cpu = s.cpu;
                baSource->setPhi2Source([cpu]() { return cpu->lastBusData(); });
                break;
            }
        desc->schedulerStep = [baSource, clock](MachineDescriptor& d) {
            int total = 0;
            if (d.busMasterStep && (total = d.busMasterStep()) > 0) {
                if (d.ioRegistry && total > d.busMasterTicked) d.ioRegistry->tickAll(total - d.busMasterTicked);
                return total;
            }
            for (auto& s : d.cpus) {
                if (s.active && s.cpu) {
                    if (s.cpu == clock->cpu) {
                        clock->i = clock->t = clock->ticked = 0;
                        clock->waited = 0;
                        const int n = s.cpu->step();
                        clock->advance(n - 1);
                        total += clock->t;
                        continue;
                    }
                    const int n = s.cpu->step();
                    const uint32_t writes = s.cpu->lastStepWrites();
                    int t = 0;
                    for (int i = 0; i < n; ++i) {
                        if (!(i < 32 && ((writes >> i) & 1)))
                            while (baSource->baLow(t)) ++t;
                        ++t;
                    }
                    total += t;
                    if (d.ioRegistry) d.ioRegistry->tickAll(t);
                }
            }
            return total;
        };
    } else if (hasFrameAccum && vsyncLine) {
        auto* frameAccum = new uint64_t(0);
        desc->deleters.push_back([frameAccum]() { delete frameAccum; });
        SimpleSignalLine* vsync = vsyncLine;
        SimpleSignalLine* retrace = retraceLine;
        uint64_t cycles = frameCycles, rcycles = retraceCycles;
        desc->schedulerStep = [frameAccum, vsync, retrace, cycles, rcycles](MachineDescriptor& d) {
            int total = 0;
            // A device holding the bus (an REU's DMA) runs instead of the CPU
            if (d.busMasterStep && (total = d.busMasterStep()) > 0) {
                if (d.ioRegistry && total > d.busMasterTicked) d.ioRegistry->tickAll(total - d.busMasterTicked);
            } else for (auto& s : d.cpus) {
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
            if (retrace) retrace->set(*frameAccum < cycles - rcycles);
            return total;
        };
    } else {
        desc->schedulerStep = [](MachineDescriptor& d) {
            int total = 0;
            if (d.busMasterStep && (total = d.busMasterStep()) > 0) {
                if (d.ioRegistry && total > d.busMasterTicked) d.ioRegistry->tickAll(total - d.busMasterTicked);
                return total;
            }
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

    // -----------------------------------------------------------------------
    // Step 9b — expansion slots: devices fitted at run time (the C64's REU)
    // -----------------------------------------------------------------------
    //   "expansions": [{ "name": "port", "description": "...", "canEmpty": true,
    //     "devices": [{ "id": "reu", "type": "reu", "name": "REU", "description": "...",
    //                   "options": ["128K", ...], "default": "512K", "optionKey": "size",
    //                   "irq": "expansion_irq", "busMaster": true }] }]
    // An option naming an existing file is passed as "image" (e.g. a .reu).
    if (spec.contains("expansions")) {
        IBus* dmaBus = desc->buses.empty() ? nullptr : desc->buses[0].bus;
        for (const auto& ss : spec["expansions"]) {
            struct DevSpec {
                std::string id, type, name, optionKey, defaultOption;
                ISignalLine* irq = nullptr;
                bool busMaster = false;
                IOHandler* linkTo = nullptr;        // "linkTo": {"device", "as"}: e.g. a second SID mixed into the first
                std::string linkAs;
                int pointerJoystick = -1;           // "pointer": {"joystickPort"}: the host mouse moves it
            };
            struct Fitted {
                IOHandler* dev = nullptr;
                const DevSpec* spec = nullptr;
                std::string option;
                ~Fitted() { delete dev; }
            };
            auto specs = std::make_shared<std::vector<DevSpec>>();
            auto fitted = std::make_shared<Fitted>();
            MachineDescriptor::ExpansionSlot slot;
            slot.name = ss.value("name", "port");
            slot.description = ss.value("description", "");
            slot.canEmpty = ss.value("canEmpty", true);
            for (const auto& ds : ss.value("devices", nlohmann::json::array())) {
                DevSpec d;
                d.id = ds.value("id", "");
                d.type = ds.value("type", d.id);
                d.name = ds.value("name", d.id);
                d.optionKey = ds.value("optionKey", "");
                d.defaultOption = ds.value("default", "");
                const std::string irqName = ds.value("irq", "");
                if (!irqName.empty() && signalMap.count(irqName)) d.irq = signalMap[irqName];
                d.busMaster = ds.value("busMaster", false);
                if (ds.contains("linkTo")) {
                    const std::string ld = ds["linkTo"].value("device", "");
                    if (devPtrs.count(ld)) d.linkTo = devPtrs[ld];
                    d.linkAs = ds["linkTo"].value("as", "");
                }
                if (ds.contains("pointer")) d.pointerJoystick = ds["pointer"].value("joystickPort", 0);
                std::vector<std::string> opts;
                for (const auto& o : ds.value("options", nlohmann::json::array())) opts.push_back(o.get<std::string>());
                slot.devices.push_back({d.id, ds.value("description", ""), opts, d.defaultOption});
                specs->push_back(std::move(d));
            }
            const std::string slotName = slot.name;
            auto savedMouse = std::make_shared<std::function<void(int, int, uint8_t, int)>>();
            slot.install = [desc, io, dmaBus, specs, fitted, slotName, savedMouse, baSource](const std::string& device, const std::string& option,
                                                                       std::string& err) {
                auto remove = [&]() {
                    if (!fitted->dev) return;
                    io->unregisterHandler(fitted->dev);
                    if (fitted->spec->irq) fitted->spec->irq->set(false);
                    if (fitted->spec->busMaster) desc->busMasterStep = nullptr;
                    if (fitted->spec->linkTo) fitted->spec->linkTo->setLinkedDevice(fitted->spec->linkAs.c_str(), nullptr);
                    if (fitted->spec->pointerJoystick >= 0) {
                        desc->onMouse = *savedMouse;
                        if (desc->onJoystick) desc->onJoystick(fitted->spec->pointerJoystick, 0x1F);
                    }
                    delete fitted->dev;
                    fitted->dev = nullptr;
                    fitted->spec = nullptr;
                    fitted->option.clear();
                };
                if (device.empty()) { remove(); return true; }
                const DevSpec* ds = nullptr;
                for (const auto& d : *specs) if (d.id == device) ds = &d;
                if (!ds) {
                    std::string ids;
                    for (const auto& d : *specs) ids += (ids.empty() ? "" : ", ") + d.id;
                    err = "no device '" + device + "' for the " + slotName + " slot (" + ids + ")";
                    return false;
                }
                IOHandler* dev = DeviceRegistry::instance().createDevice(ds->type);
                if (!dev) { err = "device type '" + ds->type + "' isn't loaded"; return false; }
                dev->setName(ds->name);
                dev->setDmaBus(dmaBus);
                if (desc->clockHz) dev->setClockHz((uint32_t)desc->clockHz);
                const std::string opt = option.empty() ? ds->defaultOption : option;
                if (!opt.empty()) {
                    std::ifstream probe(opt, std::ios::binary);
                    const std::string key = probe.good() && ds->optionKey == "size" ? "image" : ds->optionKey;
                    if (!dev->setOption(key, opt, err)) { delete dev; return false; }
                }
                remove();
                if (ds->irq) dev->setIrqLine(ds->irq);
                io->registerHandler(dev);
                if (ds->busMaster) {
                    dev->setBaSource(baSource);
                    if (auto* flat = dynamic_cast<FlatMemoryBus*>(dmaBus)) dev->setRamData(flat->rawData());
                    if (baSource) {
                        // Its accesses see the other devices ticked through their cycle, as the CPU's do
                        dev->setBusMasterSync([desc](int k) {
                            if (k + 1 > desc->busMasterTicked && desc->ioRegistry) {
                                desc->ioRegistry->tickAll(k + 1 - desc->busMasterTicked);
                                desc->busMasterTicked = k + 1;
                            }
                        });
                    }
                    desc->busMasterStep = [desc, dev]() { desc->busMasterTicked = 0; return dev->busMasterCycle(); };
                }
                if (ds->linkTo) ds->linkTo->setLinkedDevice(ds->linkAs.c_str(), dev);
                if (auto* p = dynamic_cast<IPointerDevice*>(dev); p && ds->pointerJoystick >= 0) {
                    // The host mouse moves it; its buttons are that joystick port's switches
                    *savedMouse = desc->onMouse;
                    const int port = ds->pointerJoystick;
                    desc->onMouse = [desc, p, port](int dx, int dy, uint8_t buttons, int) {
                        p->pointer(dx, dy, buttons);
                        if (desc->onJoystick) desc->onJoystick(port, p->joystickBits());
                    };
                }
                fitted->dev = dev;
                fitted->spec = ds;
                fitted->option = opt;
                return true;
            };
            slot.info = [fitted]() -> std::string {
                if (!fitted->dev) return "";
                std::string s = fitted->spec->id;
                DeviceInfo di;
                fitted->dev->getDeviceInfo(di);
                for (const auto& kv : di.state)
                    if (kv.first == "Size") return s + " (" + fitted->dev->name() + ", " + kv.second + ")";
                return s + " (" + fitted->dev->name() + (fitted->option.empty() ? "" : ", " + fitted->option) + ")";
            };
            slot.save = [fitted](const std::string& path, std::string& err) {
                if (!fitted->dev) { err = "nothing is fitted"; return false; }
                return fitted->dev->saveDeviceImage(path, err);
            };
            desc->expansionSlots.push_back(std::move(slot));
            desc->deleters.push_back([fitted]() { delete fitted->dev; fitted->dev = nullptr; });
        }
    }

    // -----------------------------------------------------------------------
    // Step 10 — intercepts from the machine and its ROM entries
    // -----------------------------------------------------------------------
    installIntercepts(*desc, spec, [&romByLabel](const std::string& label) -> const uint8_t* {
        auto it = romByLabel.find(label);
        return it == romByLabel.end() ? nullptr : it->second.first;
    });

    return desc;
}
