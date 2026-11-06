#include "cbm_cart_handler.h"
#include "crt_parser.h"
#include "libcore/main/machine_desc.h"
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

bool CbmCartridgeHandler::attach(IBus* bus, MachineDescriptor* machine) {
    if (!bus) return false;

    // For now, only support standard 8K ($8000) and 16K ($8000)
    // C64 cartridges (Type 0 = Normal)
    if (m_cartType != 0) {
        std::cerr << "CBM Loader: Unsupported cartridge type " << m_cartType << std::endl;
        return false;
    }

    for (const auto& bank : m_banks) {
        bus->addRomOverlay(bank.loadAddr, (uint32_t)bank.data.size(), bank.data.data());
    }
    
    return true;
}

void CbmCartridgeHandler::eject(IBus* bus) {
    if (!bus) return;
    for (const auto& bank : m_banks) {
        bus->removeRomOverlay(bank.loadAddr);
    }
}

CartridgeMetadata CbmCartridgeHandler::metadata() const {
    CartridgeMetadata md;
    md.type = "C64 CRT Type " + std::to_string(m_cartType);
    md.bankCount = (int)m_banks.size();
    if (!m_banks.empty()) {
        md.startAddr = m_banks[0].loadAddr;
        md.endAddr = md.startAddr + (uint32_t)m_banks[0].data.size() - 1;
    }
    md.imagePath = m_path;
    md.displayName = m_displayName;
    return md;
}
