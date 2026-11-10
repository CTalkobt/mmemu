#pragma once

#include "plugins/z80/main/cpu_z80.h"
#include "spi_sd_card.h"
#include <cstdint>
#include <cstring>
#include <vector>

namespace spectrum {

/**
 * The DivMMC, after Fuse's divmmc.c and divxxx.c: 8 KB of EEPROM (esxDOS's
 * firmware), 128 KB of RAM in 8 KB banks and an SD card on SPI.
 *
 * - Port $E3 (control): b7 CONMEM (the EEPROM at $0000, a RAM bank at
 *   $2000, whatever the traps say), b6 MAPRAM (bank 3 instead of the EEPROM,
 *   read-only; once set it stays until power-off), b0-b3 the bank at $2000.
 * - Port $E7: card select (b0 low: card 0). Port $EB: the SPI byte.
 * - Automapping (the EEPROM's write-protect jumper on, as Fuse defaults):
 *   an instruction fetched at $3D00-$3DFF maps it in before the fetch; at
 *   $0000, $0008, $0038, $0066, $04C6 or $0562 after it; at $1FF8-$1FFF it
 *   maps out after the fetch.
 * - Mapped: $0000-$1FFF the EEPROM (or bank 3 with MAPRAM, read-only),
 *   $2000-$3FFF the bank (read-only if it's bank 3 under MAPRAM).
 */
class DivMmc : public IZ80FetchHook {
public:
    static constexpr int BANKS = 16;

    explicit DivMmc(std::vector<uint8_t> eeprom) : m_eeprom(std::move(eeprom)), m_ram((size_t)BANKS * 0x2000, 0) {
        m_eeprom.resize(0x2000, 0xFF);
    }

    SpiSdCard& card() { return m_card; }
    uint8_t control() const { return m_control; }
    bool automap() const { return m_automap; }

    /** Power on (`hard`), or the reset line: MAPRAM survives a soft reset. */
    void reset(bool hard) {
        if (hard) {
            m_control = 0;
            std::fill(m_ram.begin(), m_ram.end(), 0);
        } else {
            m_control &= MAPRAM;
        }
        m_automap = false;
        m_card.reset();
        m_selected = false;
    }

    /** Its memory is in at $0000-$3FFF. */
    bool paged() const {
        if (m_control & CONMEM) return true;
        return (WRITE_PROTECT || (m_control & MAPRAM)) && m_automap;
    }
    uint8_t read(uint16_t a) const {
        if (a < 0x2000) return (!(m_control & CONMEM) && (m_control & MAPRAM)) ? m_ram[3 * 0x2000 + a] : m_eeprom[a];
        return m_ram[(size_t)bank() * 0x2000 + (a - 0x2000)];
    }
    void write(uint16_t a, uint8_t v) {
        if (a < 0x2000) {
            if ((m_control & CONMEM) && !WRITE_PROTECT) m_eeprom[a] = v;
            return;                                  // the EEPROM, or bank 3 under MAPRAM: read-only
        }
        if (!(m_control & CONMEM) && (m_control & MAPRAM) && bank() == 3) return;
        m_ram[(size_t)bank() * 0x2000 + (a - 0x2000)] = v;
    }

    /** I/O: true if the port is the DivMMC's. */
    bool portWrite(uint16_t port, uint8_t v) {
        switch (port & 0xFF) {
            case 0xE3: m_control = (uint8_t)(v | (m_control & MAPRAM)); return true;   // MAPRAM only sets
            case 0xE7: m_selected = (v & 3) == 2; return true;                       // card 0 (card 1 not fitted)
            case 0xEB: if (m_selected) m_card.write(v); return true;
            default: return false;
        }
    }
    bool portRead(uint16_t port, uint8_t& v) {
        if ((port & 0xFF) != 0xEB) return false;
        v = m_selected ? m_card.read() : 0xFF;
        return true;
    }

    // IZ80FetchHook (Fuse z80_ops.c divmmc_early / divmmc_late)
    void before(uint16_t pc) override {
        if ((pc & 0xFF00) == 0x3D00) m_automap = true;
    }
    void after(uint16_t pc) override {
        if ((pc & 0xFFF8) == 0x1FF8) m_automap = false;
        else if (pc == 0x0000 || pc == 0x0008 || pc == 0x0038 || pc == 0x0066 || pc == 0x04C6 || pc == 0x0562)
            m_automap = true;
    }

private:
    static constexpr uint8_t CONMEM = 0x80, MAPRAM = 0x40;
    static constexpr bool WRITE_PROTECT = true;     // the EEPROM jumper
    int bank() const { return m_control & (BANKS - 1); }

    std::vector<uint8_t> m_eeprom, m_ram;
    uint8_t m_control = 0;
    bool m_automap = false, m_selected = false;
    SpiSdCard m_card;
};

}  // namespace spectrum
