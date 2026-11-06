#include "cbm_cart_handler.h"
#include "crt_parser.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/io_handler.h"
#include <iostream>

CbmCartridgeHandler::CbmCartridgeHandler(const std::string& path) : m_path(path) {
    m_valid = loadCrt();
}

CbmCartridgeHandler::~CbmCartridgeHandler() {
}

bool CbmCartridgeHandler::loadCrt() {
    CrtParser parser;
    if (!parser.parse(m_path)) return false;

    m_displayName = parser.header().cartName;
    m_cartType = parser.header().cartType;
    m_exrom = parser.header().exrom;
    m_game = parser.header().game;

    for (const auto& chip : parser.chips()) {
        Bank bank;
        bank.bankNum = chip.bankNum;
        bank.loadAddr = (uint32_t)chip.loadAddr;
        bank.data = chip.data;
        m_banks.push_back(std::move(bank));
    }

    return true;
}

/**
 * The generic cartridge's mode from its CHIP packets, as VICE does
 * (c64-generic.c generic_crt_attach): a first chip at $8000 of up to 8 KB is
 * an 8 KB game (EXROM low), up to 16 KB a 16 KB game (EXROM and GAME low,
 * ROMH = its second 8 KB); a chip ending at $FFFF, alone or after the $8000
 * one, makes it Ultimax (GAME low), ROMH at $E000. Short ROMs read $FF.
 */
bool CbmCartridgeHandler::layOut() {
    m_roml.clear();
    m_romh.clear();
    if (m_banks.empty()) return false;
    auto place = [](std::vector<uint8_t>& rom, const std::vector<uint8_t>& data, size_t from, size_t at) {
        if (rom.empty()) rom.assign(0x2000, 0xFF);
        for (size_t i = from; i < data.size() && at + (i - from) < 0x2000; ++i) rom[at + (i - from)] = data[i];
    };
    size_t next = 0;
    const Bank& first = m_banks[0];
    if (first.loadAddr == 0x8000 && !first.data.empty() && first.data.size() <= 0x4000) {
        place(m_roml, first.data, 0, 0);
        if (m_banks.size() == 1) {
            if (first.data.size() > 0x2000) {
                place(m_romh, first.data, 0x2000, 0);
                m_exromLow = m_gameLow = true;           // 16 KB game
            } else {
                m_exromLow = true; m_gameLow = false;    // 8 KB game
            }
            return true;
        }
        next = 1;
    }
    if (next < m_banks.size()) {
        const Bank& hi = m_banks[next];
        if (hi.loadAddr >= 0xE000 && !hi.data.empty() && hi.loadAddr + hi.data.size() == 0x10000) {
            place(m_romh, hi.data, 0, hi.loadAddr - 0xE000);
            m_exromLow = false; m_gameLow = true;        // Ultimax
            return true;
        }
    }
    return false;
}

/** Hand the ROMs and lines to every device on the expansion port (the PLA,
 *  the VIC-II); false if none takes them. */
static bool setPort(MachineDescriptor* machine, const std::vector<uint8_t>& roml,
                    const std::vector<uint8_t>& romh, bool exrom, bool game) {
    if (!machine || !machine->ioRegistry) return false;
    std::vector<IOHandler*> handlers;
    machine->ioRegistry->enumerate(handlers);
    bool taken = false;
    for (auto* h : handlers)
        taken |= h->setCartridgePort(roml.empty() ? nullptr : roml.data(), (uint32_t)roml.size(),
                                     romh.empty() ? nullptr : romh.data(), (uint32_t)romh.size(),
                                     exrom, game);
    return taken;
}

bool CbmCartridgeHandler::attach(IBus* bus, MachineDescriptor* machine) {
    if (!bus) return false;

    // Only the generic (plain ROM) hardware type
    if (m_cartType != 0) {
        std::cerr << "CBM Loader: Unsupported cartridge type " << m_cartType << std::endl;
        return false;
    }
    if (!layOut()) {
        std::cerr << "CBM Loader: unsupported CHIP layout for a generic cartridge" << std::endl;
        return false;
    }
    if (setPort(machine, m_roml, m_romh, m_exromLow, m_gameLow)) {
        m_onPort = true;
        m_machine = machine;
        return true;
    }
    // No expansion port device (no PLA): plain ROM overlays
    for (const auto& bank : m_banks) {
        bus->addRomOverlay(bank.loadAddr, (uint32_t)bank.data.size(), bank.data.data());
    }
    return true;
}

void CbmCartridgeHandler::eject(IBus* bus) {
    if (!bus) return;
    if (m_onPort) {
        setPort(m_machine, {}, {}, false, false);
        m_onPort = false;
        return;
    }
    for (const auto& bank : m_banks) {
        bus->removeRomOverlay(bank.loadAddr);
    }
}

CartridgeMetadata CbmCartridgeHandler::metadata() const {
    CartridgeMetadata md;
    md.type = "C64 CRT Type " + std::to_string(m_cartType);
    if (m_onPort) md.type += m_gameLow ? (m_exromLow ? ", 16 KB game" : ", Ultimax") : ", 8 KB game";
    md.bankCount = (int)m_banks.size();
    if (!m_banks.empty()) {
        md.startAddr = m_banks[0].loadAddr;
        md.endAddr = md.startAddr + (uint32_t)m_banks[0].data.size() - 1;
    }
    md.imagePath = m_path;
    md.displayName = m_displayName;
    return md;
}
