#include "kernal_hle.h"
#include <cstdio>
#include "include/util/logging.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_handler.h"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

// Simulate RTS, handling both 8-bit (6502) and 16-bit (45GS02) stack pointers.
// On the 45GS02, the E flag (P bit 5) controls stack mode:
//   E=1 (SEE): 8-bit SP, wraps within current stack page (SPH preserved)
//   E=0 (CLE): 16-bit SP, full address space
// On the 6502, SP is always 8-bit in page $01.
static void simulateRts(ICore* cpu, IBus* bus) {
    uint32_t sp = cpu->regReadByName("SP");
    uint32_t p  = cpu->regReadByName("P");
    // Check SP register width to distinguish 6502 (8-bit) from 45GS02 (16-bit)
    bool has16bitSP = false;
    for (int i = 0; i < cpu->regCount(); ++i) {
        const auto* d = cpu->regDescriptor(i);
        if (d && std::string(d->name) == "SP" && d->width == RegWidth::R16) {
            has16bitSP = true;
            break;
        }
    }

    uint16_t addr1, addr2;
    if (has16bitSP) {
        bool eFlag = (p & 0x20) != 0;  // FLAG_E = bit 5
        if (eFlag) {
            // 8-bit stack mode: wrap within current page
            uint16_t page = sp & 0xFF00;
            addr1 = page | ((sp + 1) & 0xFF);
            addr2 = page | ((sp + 2) & 0xFF);
            cpu->regWriteByName("SP", (uint32_t)(page | ((sp + 2) & 0xFF)));
        } else {
            // 16-bit stack mode
            addr1 = (sp + 1) & 0xFFFF;
            addr2 = (sp + 2) & 0xFFFF;
            cpu->regWriteByName("SP", (uint32_t)((sp + 2) & 0xFFFF));
        }
    } else {
        // 6502: 8-bit SP always in page $01
        addr1 = 0x0100 | ((sp + 1) & 0xFF);
        addr2 = 0x0100 | ((sp + 2) & 0xFF);
        cpu->regWriteByName("SP", (uint32_t)((sp + 2) & 0xFF));
    }
    uint16_t retAddr = (bus->read8(addr1) | (bus->read8(addr2) << 8)) + 1;
    cpu->setPc(retAddr);
}

KernalHLE::KernalHLE(const nlohmann::json& e) {
    if (e.contains("load")) m_load = interceptAddr(e["load"]);
    if (e.contains("save")) m_save = interceptAddr(e["save"]);
    m_hostPath = e.value("path", std::string("."));
    if (e.contains("zeroPage")) {
        const auto& z = e["zeroPage"];
        if (z.contains("status")) m_zp.status = (uint16_t)interceptAddr(z["status"]);
        if (z.contains("fnlen"))  m_zp.fnlen  = (uint16_t)interceptAddr(z["fnlen"]);
        if (z.contains("sa"))     m_zp.sa     = (uint16_t)interceptAddr(z["sa"]);
        if (z.contains("fa"))     m_zp.fa     = (uint16_t)interceptAddr(z["fa"]);
        if (z.contains("fnadr"))  m_zp.fnadr  = (uint16_t)interceptAddr(z["fnadr"]);
    }
}

bool KernalHLE::setOption(const std::string& key, const std::string& value) {
    if (key == "path") { m_hostPath = value; return true; }
    return false;
}

std::string KernalHLE::describe() const {
    char buf[96];
    snprintf(buf, sizeof buf, "KERNAL LOAD $%04X / SAVE $%04X from host folder: ", m_load, m_save);
    return buf + m_hostPath;
}

bool KernalHLE::onExecute(MachineDescriptor& m, ICore& cpu, IBus& bus, uint32_t pc) {
    return handle(cpu, bus, pc, &m);
}

bool KernalHLE::handle(ICore& cpu, IBus& bus, uint32_t pc, MachineDescriptor* m) {
    if (pc != m_load && pc != m_save) return false;
    uint8_t device = getDevice(&bus);
    LogRegistry::instance().getLogger("hle")->debug("vector ${:04X}, device {}", pc, device);

    // Units below 8 (tape, printers...) and units an emulated drive answers
    // are left to the KERNAL
    if (device < 8) return false;
    if (m && m->ioRegistry) {
        std::vector<IOHandler*> handlers;
        m->ioRegistry->enumerate(handlers);
        for (auto* h : handlers) {
            int t, s;
            bool led;
            if (h->getDiskStatus(device, t, s, led)) {
                LogRegistry::instance().getLogger("hle")->debug("device {} handled by {}, passing to KERNAL",
                                                                device, h->name());
                return false;
            }
        }
    }

    if (pc == m_load) handleLoad(&cpu, &bus);
    else              handleSave(&cpu, &bus);
    return true;
}

void registerKernalHleIntercept() {
    InterceptRegistry::instance().registerType("cbm.kernal.fileio",
        [](const nlohmann::json& e) -> IIntercept* { return new KernalHLE(e); });
}

void KernalHLE::handleLoad(ICore* cpu, IBus* bus) {
    uint8_t sa = getSecondaryAddress(bus);
    std::string filename = getFilename(bus);
    
    // Determine load address
    uint32_t loadAddr;
    if (sa == 0) {
        // Load to address in X/Y
        loadAddr = cpu->regReadByName("X") | (cpu->regReadByName("Y") << 8);
    } else {
        // Load to address from file header (first 2 bytes)
        loadAddr = 0; // Will be set from file
    }

    std::string fullPath = (fs::path(m_hostPath) / filename).string();
    std::ifstream file(fullPath, std::ios::binary);
    if (!file) {
        // File not found
        setStatus(bus, 4); // 4 = File not found
        setCarry(cpu, true);
        
        simulateRts(cpu, bus);
        return;
    }

    // Read file
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    
    if (data.size() < 2) {
        setStatus(bus, 4);
        setCarry(cpu, true);
        return;
    }

    // The first two bytes are the file's load address: used with a non-zero
    // secondary address, skipped either way (SA 0 loads to X/Y)
    size_t startOffset = 2;
    if (sa != 0) loadAddr = data[0] | (data[1] << 8);

    // Inject data
    for (size_t i = startOffset; i < data.size(); ++i) {
        bus->write8(loadAddr++, data[i]);
    }

    // Update registers for success
    cpu->regWriteByName("X", (uint8_t)(loadAddr & 0xFF));
    cpu->regWriteByName("Y", (uint8_t)(loadAddr >> 8));
    setCarry(cpu, false);
    setStatus(bus, 0);

    simulateRts(cpu, bus);
}

// KERNAL SAVE: A = zero-page address of the start pointer, X/Y = end + 1.
// Writes <name> in the host folder: the 2-byte start address, then the data.
// As on a CBM drive, an existing file is replaced only with an "@:" (or
// "@0:") prefix; otherwise it is left alone and SAVE still returns normally
// (a real drive reports 63 FILE EXISTS on its command channel only).
void KernalHLE::handleSave(ICore* cpu, IBus* bus) {
    std::string name = getFilename(bus);
    bool replace = false;
    if (!name.empty() && name[0] == '@') { replace = true; name = name.substr(1); }
    if (name.size() >= 2 && name[0] == '0' && name[1] == ':') name = name.substr(2);
    else if (!name.empty() && name[0] == ':') name = name.substr(1);
    if (auto comma = name.find(','); comma != std::string::npos) name = name.substr(0, comma);   // ",P,W"

    auto fail = [&](uint8_t kernalError) {
        cpu->regWriteByName("A", kernalError);
        setCarry(cpu, true);
        simulateRts(cpu, bus);
    };
    if (name.empty()) { fail(8); return; }                 // 8 = MISSING FILE NAME

    uint8_t zp = (uint8_t)cpu->regReadByName("A");
    uint16_t start = bus->read8(zp) | (bus->read8((uint8_t)(zp + 1)) << 8);
    uint16_t end = (uint16_t)(cpu->regReadByName("X") | (cpu->regReadByName("Y") << 8));

    fs::path path = fs::path(m_hostPath) / name;
    std::error_code ec;
    if (fs::exists(path, ec) && !replace) {
        LogRegistry::instance().getLogger("hle")->warn(
            "SAVE \"{}\": {} exists, not replaced (use \"@:{}\" to replace)", name, path.string(), name);
        setStatus(bus, 0);
        setCarry(cpu, false);
        simulateRts(cpu, bus);
        return;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) { fail(5); return; }                        // host folder not writable
    file.put((char)(start & 0xFF));
    file.put((char)(start >> 8));
    for (uint32_t a = start; a < end; ++a) file.put((char)bus->read8((uint16_t)a));
    file.close();
    if (!file) { fail(5); return; }

    setStatus(bus, 0);
    setCarry(cpu, false);
    simulateRts(cpu, bus);
}

uint8_t KernalHLE::getDevice(IBus* bus) {
    return bus->read8(m_zp.fa);
}

uint8_t KernalHLE::getSecondaryAddress(IBus* bus) {
    return bus->read8(m_zp.sa);
}

std::string KernalHLE::getFilename(IBus* bus) {
    uint8_t len = bus->read8(m_zp.fnlen);
    uint16_t ptr = bus->read8(m_zp.fnadr) | (bus->read8(m_zp.fnadr + 1) << 8);
    std::string name;
    for (int i = 0; i < len; ++i) {
        name += (char)bus->read8(ptr + i);
    }
    return name;
}

void KernalHLE::setStatus(IBus* bus, uint8_t status) {
    bus->write8(m_zp.status, status);
}

void KernalHLE::setCarry(ICore* cpu, bool set) {
    uint8_t p = cpu->regReadByName("P");
    if (set) p |= 0x01;
    else     p &= ~0x01;
    cpu->regWriteByName("P", p);
}
