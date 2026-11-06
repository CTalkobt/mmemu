#pragma once

#include "libcore/main/image_loader.h"
#include <string>
#include <vector>
#include <cstdint>

class CbmCartridgeHandler : public ICartridgeHandler {
public:
    CbmCartridgeHandler(const std::string& path);
    ~CbmCartridgeHandler() override;

    bool attach(IBus* bus, MachineDescriptor* machine) override;
    void eject(IBus* bus) override;
    CartridgeMetadata metadata() const override;
    /** The file parsed as a C64 .crt ("C64 CARTRIDGE" header). */
    bool valid() const { return m_valid; }

private:
    std::string m_path;
    std::string m_displayName;
    bool m_valid = false;
    uint16_t m_cartType = 0;
    uint8_t m_exrom = 0;
    uint8_t m_game = 0;

    struct Bank {
        uint16_t bankNum;
        uint32_t loadAddr;
        std::vector<uint8_t> data;
    };
    std::vector<Bank> m_banks;

    bool loadCrt();
    bool layOut();

    // On the expansion port (a machine with a PLA): ROML/ROMH and the lines
    std::vector<uint8_t> m_roml, m_romh;
    bool m_exromLow = false, m_gameLow = false;
    bool m_onPort = false;
    MachineDescriptor* m_machine = nullptr;
};
