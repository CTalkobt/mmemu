#pragma once

// The Super Game Boy (#15), after Mesen2 (Core/SNES/Coprocessors/SGB/SuperGameboy.cpp;
// GPL-3.0, Sour and the Mesen contributors).
//
// The SGB cartridge is a SNES BIOS (chipset $E3, or game code 042J) and the
// ICD2 chip between the SNES and a whole Game Boy (gb::Gameboy, its CPU at
// the SNES master clock / 5 on the SGB1, a 20.97 MHz crystal / 5 on the
// SGB2; the divider is $6003's low bits). The ICD2's registers are at
// $6000-$7FFF of banks $00-$3F/$80-$BF:
//   $6000 R   the LCD row being written (bits 3-7) and the buffer it goes to (0-1)
//   $6001 W   the row buffer to read next (0-3); resets the read position
//   $6002 R   1: a joypad packet is waiting at $7000-$700F (read: no longer)
//   $6003 W   bit 7 runs the Game Boy (0 holds it reset, a rise powers it on),
//             bits 4-5 the number of players, bits 0-1 the clock divider (4, 5, 7, 9)
//   $6004-7 W the four players' buttons, as $FF00 reads them (0 = pressed)
//   $600F R   the ICD2's version ($21)
//   $7000-F R the 16-byte packet the Game Boy sent through P14/P15
//   $7800 R   the selected row buffer, a byte at a time: 20 tiles of 2 bits per pixel
//             (320 bytes), then $FF
// The Game Boy's LCD fills four 8-line buffers in turn as it draws; its
// joypad port writes are decoded as packets (a reset pulse, then 128 bits
// and a stop bit) or, with both lines high, step to the next player.
// Its sound is box-filtered to 32 kHz, queued, and added to the S-DSP's
// output a sample at a time (Mesen uses a Hermite resampler; and leaves it
// out when the DSP is muted), through a DC-blocking filter.
//
// As in Mesen, the BIOS is the SNES cartridge (Mesen's SGB1.sfc/SGB2.sfc)
// and the Game Boy cartridge is what the user loads.

#include "snes_msu1.h"
#include "plugins/machines/gameboy/main/gameboy.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace snes {

class Sgb : public gb::SgbLink, public AudioSource {
public:
    /** `sgb2`: the Super Game Boy 2 (its own crystal and boot ROM). */
    explicit Sgb(bool sgb2);

    /** The Game Boy cartridge (.gb); false and `err` if it can't be. Empty: none. */
    bool loadCartridge(const std::vector<uint8_t>& rom, std::string& err);
    bool hasCartridge() const { return m_gb.loaded(); }
    gb::Gameboy& gameboy() { return m_gb; }
    bool sgb2() const { return m_sgb2; }

    /** As at power on and on RESET (Mesen's Reset): the Game Boy held, the ICD2 cleared. */
    void reset(uint64_t master, uint64_t masterHz);
    /** Bring the Game Boy up to the SNES's master clock. */
    void runTo(uint64_t master);
    /** $6000-$7FFF. */
    uint8_t read(uint16_t addr, uint64_t master, bool peek);
    void write(uint16_t addr, uint8_t v, uint64_t master);
    /** Mesen leaves the Game Boy's sound out when the S-DSP is muted (FLG bit 6). */
    void setMuteCheck(std::function<bool()> f) { m_muted = std::move(f); }

    void mix(int32_t& left, int32_t& right) override;

    // SgbLink: what the Game Boy does
    void hblank() override;
    void vblank() override;
    void lcdPixel(uint8_t x, uint8_t shade) override;
    void joypadWrite(uint8_t select) override;
    uint8_t joypadId() override { return (uint8_t)(0x0F - m_inputIndex); }
    uint8_t joypadButtons() override { return m_input[m_inputIndex]; }

    // For tests and the debugger
    uint8_t control() const { return m_control; }
    bool packetReady() const { return m_packetReady; }
    const uint8_t* packet() const { return m_packet; }
    uint32_t packetCount() const { return m_packetCount; }
    uint8_t player() const { return m_inputIndex; }
    const uint8_t* lcdBuffer(int i) const { return m_lcd[i & 3]; }
    uint64_t gbClockHz() const;

private:
    uint8_t playerCount() const;
    void setInputIndex(uint8_t i);
    void setInputValue(uint8_t i, uint8_t v);
    /** The Game Boy's clock: on a divider change it is caught up and the ratio rebased (Mesen's UpdateClockRatio). */
    void updateClockRatio(uint64_t master);
    uint64_t gbTarget(uint64_t master) const;

    gb::Gameboy m_gb;
    bool m_sgb2;
    uint64_t m_masterHz = 21477272;
    std::function<bool()> m_muted;

    uint8_t m_control = 0x01;
    uint64_t m_resetClock = 0, m_clockOffset = 0;
    uint32_t m_divider = 0;          // the current clock divider (0: not set yet)

    uint8_t m_row = 0, m_bank = 0;
    uint8_t m_input[4] = {};
    uint8_t m_inputIndex = 0;

    bool m_listening = false, m_waitForHigh = true, m_packetReady = false;
    uint8_t m_inputValue = 0;
    uint8_t m_packet[16] = {};
    uint8_t m_packetByte = 0, m_packetBit = 0, m_packetBuffer = 0;
    uint32_t m_packetCount = 0;

    uint8_t m_lcdRowSelect = 0;
    uint16_t m_readPosition = 0;
    uint8_t m_lcd[4][1280] = {};

    // The sound: a DC blocker's state and the last sample (repeated when the Game Boy is behind)
    double m_dcInL = 0, m_dcInR = 0, m_dcOutL = 0, m_dcOutR = 0;
    int16_t m_lastL = 0, m_lastR = 0;
};

}  // namespace snes
