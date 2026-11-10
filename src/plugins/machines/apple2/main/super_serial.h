#pragma once

#include "slot_card.h"
#include "plugins/devices/acia6551/main/acia6551.h"
#include <memory>
#include <string>
#include <vector>

namespace apple2 {

/**
 * Apple's Super Serial Card, after AppleWin's SerialComms.cpp: a 6551 ACIA
 * (1.8432 MHz crystal: the standard rates) and its firmware.
 *
 * - $C0n1 / $C0n2: DIP switches 1 and 2 (read-only). SW1: the baud rate in
 *   bits 4-7 (the 6551's rate code; 9600 = $E), the firmware mode in bits
 *   0-1 (0: communications, "CIC"). SW2 (SSC-54): b7 two stop bits, b5 7
 *   data bits, b3/b2 parity, b1 no line feed after CR, b0 CTS (0 = clear to
 *   send, while the far end is there). Defaults as AppleWin's: 9600 8N1,
 *   CIC, no LF, interrupts on.
 * - $C0n8-$C0nB: the 6551 (data, status, command, control).
 * - $Cn00-$CnFF: the firmware's last page; $C800-$CFFF: its 2 KB.
 * - The ACIA's /IRQ goes to the 6502 (SW2-6 on).
 * - Its far end is the ACIA's: CLI `serial` (modem, tcp:PORT, loopback).
 *
 * ROM: roms/apple2/SSC.rom (2 KB, AppleWin's resource).
 */
class SuperSerialCard : public SlotCard {
public:
    /** Null with `err` set if the ROM is missing. `baud`: SW1's rate. */
    static SuperSerialCard* create(int slot, int baud, std::string& err);

    int slot() const override { return m_slot; }
    int deviceIo(int reg, const uint8_t* wr, uint64_t at) override;
    int devicePeek(int reg) override;
    int pageIo(uint8_t off, const uint8_t*, uint64_t) override { return m_rom[0x700 + off]; }
    int pagePeek(uint8_t off) override { return m_rom[0x700 + off]; }
    const uint8_t* expansionRom() const override { return m_rom.data(); }
    bool irq() const override { return m_acia.irqAsserted(); }
    Acia6551& acia() { return m_acia; }
    uint8_t dipSwitch1() const;
    /** The DIP switches' rate. */
    int baud() const { return m_baud; }
    uint8_t dipSwitch2() const;

    // IOHandler
    const char* name() const override { return m_name.c_str(); }
    void reset() override { m_acia.reset(); }
    void tick(uint64_t cycles) override { m_acia.tick(cycles); }
    bool attachSerial(const std::string& spec) override { return m_acia.attachSerial(spec); }
    void getDeviceInfo(DeviceInfo& out) const override;

private:
    SuperSerialCard(int slot, int baud, int baudCode, std::vector<uint8_t> rom);

    std::string m_name;
    int m_slot;
    int m_baud;
    uint8_t m_baudCode;
    std::vector<uint8_t> m_rom;
    Acia6551 m_acia;
};

}  // namespace apple2
