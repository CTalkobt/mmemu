#pragma once

// Cartridges for the CoCo 1/2 (#355): the Speech/Sound Cartridge and the
// CoCo Max Hi-Res Input Pack.

#include "coco_cart.h"
#include "plugins/devices/sp0256/main/sp0256.h"
#include "plugins/devices/tms7000/main/tms7000.h"
#include "plugins/machines/spectrum/main/spectrum_ay.h"

namespace coco {

/**
 * The Radio Shack Speech/Sound Cartridge (26-3144), after MAME's coco_ssc
 * (tim lindner): a TMS7040 microcontroller running the cartridge's own
 * firmware (its mask ROM), 2 KB of static RAM, an SP0256-AL2 speech
 * processor (3.12 MHz crystal) and an AY-3-8913 sound generator (the
 * AY-3-8910 without I/O ports; mmsim's AY core is the Spectrum's), both
 * clocked with the TMS7040 from the cartridge port's E x 2 (1.79 MHz).
 *
 * The CoCo sees two registers:
 *  - $FF7D write: bit 0 high holds the TMS7040, the SP0256 and the AY in
 *    reset; low lets them run (the TMS7040 from its reset vector). Not read.
 *  - $FF7E write: a byte for the TMS7040 (its port A; raises its INT3 and
 *    sets BUSY until the firmware raises port C bit 7). Read: bit 7 low =
 *    BUSY, bit 6 the SP0256's SBY (high: not speaking), bit 5 low while
 *    the sound activity circuit hears the AY, bits 4-0 high.
 *
 * Inside: port B is the RAM's A0-A7; port C bit 0 A8 / the AY's BC1, 1-2
 * A9-A10, 3 the RAM's R/W / the AY's BDIR, 4 the RAM's /CS, 5 the
 * SP0256's /ALD (falling: port D, if under 64, is the allophone), 6 the
 * AY's /CS, 7 BUSY's reset; port D the data bus. The SP0256's LRQ is the
 * TMS7040's INT1.
 *
 * What the CoCo sends (the speech text, sound commands) is the firmware's
 * business: it runs as is. Both chips' ROMs are copyrighted, so they are
 * the user's: pic-7040-510.bin (the TMS7040's 4 KB, required) and
 * sp0256-al2.bin (the SP0256-AL2's 2 KB; without it speech is silent but
 * SBY / LRQ still work) from firmwareDir(), or the option's paths.
 *
 * The sound activity circuit is MAME's model of it (a high-pass filter and
 * an envelope follower on the AY's output, with hysteresis), as is the mix.
 */
class SpeechSoundPak : public Cart {
public:
    static constexpr const char* MCU_ROM = "pic-7040-510.bin";
    static constexpr const char* SPEECH_ROM = "sp0256-al2.bin";

    SpeechSoundPak(std::vector<uint8_t> mcuRom, std::vector<uint8_t> speechRom);
    std::string kind() const override { return "ssc"; }
    std::string describe() const override;
    void reset() override;
    bool read(uint16_t a, bool cts, bool scs, uint8_t& d) override;
    bool peek(uint16_t a, bool cts, bool scs, uint8_t& d) const override;
    void write(uint16_t a, bool cts, bool scs, uint8_t d) override;
    void clock(uint64_t ticks) override;
    void setClockHz(double hz) override { m_hz = hz; }
    float audio() const override { return m_audio; }

    /** $FF7E as the CoCo reads it. */
    uint8_t status() const;
    bool busy() const { return m_busy; }
    bool soundActive() const { return m_sacActive; }
    Tms7000& mcu() { return m_mcu; }
    Sp0256& speech() { return m_sp; }
    spectrum::SpectrumAy& psg() { return m_ay; }
    uint8_t ram(int a) const { return m_ram[a & 0x7FF]; }
    /** The AY's level (0..1, all three channels at full volume = 1), last step. */
    float psgLevel() const { return m_ayLevel; }

private:
    uint8_t portIn(int port);
    void portOut(int port, uint8_t v);
    void portCWrite(uint8_t v);
    void portDRead();
    void runSound(int64_t ticks);
    void sacStep(float x);

    Tms7000 m_mcu;
    Sp0256 m_sp;
    spectrum::SpectrumAy m_ay;
    uint8_t m_ram[0x800] = {};
    bool m_haveSpeech = false;
    double m_hz = 14318180.0;

    uint8_t m_resetLine = 0;
    bool m_busy = false;
    uint8_t m_portA = 0, m_portB = 0, m_portC = 0, m_portD = 0;
    bool m_ayActive = false;
    uint8_t m_ayLatch = 0;

    int64_t m_budget = 0;           // crystal ticks the TMS7040 is behind
    uint64_t m_ayAcc = 0;
    double m_spAcc = 0, m_sacAcc = 0;
    float m_ayLevel = 0;
    double m_aySum = 0;
    int m_ayCount = 0;
    float m_audio = 0;
    // The sound activity circuit
    float m_sacEnvelope = 0, m_sacPrevIn = 0, m_sacPrevOut = 0;
    bool m_sacActive = false;
};

/** The Speech/Sound Cartridge from `option` ("[MCU ROM][,SP0256 ROM]", else the
 *  firmware directory's files); nullptr and `err` without the TMS7040's ROM. */
std::unique_ptr<Cart> makeSpeechSoundPak(const std::string& option, std::string& err);

/**
 * The CoCo Max Hi-Res Input Pack (Colorware; CoCo Max I/II on the CoCo
 * 1/2), after MAME's coco_max (tim lindner): an 8-bit A/D converter for
 * the mouse or joystick plugged into the pack, read at $FF90-$FF97. A read
 * returns the last conversion and starts the next of its channel: $FF90 Y,
 * $FF91 X (0-255), $FF92 the left button ($00 pressed, $FF not), $FF93
 * the right button; $FF94-$FF97 start nothing. The host mouse moves it (one
 * step a pixel). No ROM (CoCo Max's program is on tape or disk). It is not
 * for the CoCo 3, whose GIME is at $FF90.
 */
class CocoMax : public Cart {
public:
    std::string kind() const override { return "cocomax"; }
    std::string describe() const override { return "CoCo Max Hi-Res Input Pack (A/D at $FF90-$FF97)"; }
    void reset() override { m_result = 0; }
    bool read(uint16_t a, bool cts, bool scs, uint8_t& d) override;
    bool peek(uint16_t a, bool cts, bool scs, uint8_t& d) const override;

    /** Where the stick (mouse) is: 0-255 each. */
    void setPosition(int x, int y);
    void move(int dx, int dy) { setPosition(m_x + dx, m_y + dy); }
    /** Bit 0 the left button, bit 1 the right. */
    void setButtons(uint8_t b) { m_buttons = b; }
    int x() const { return m_x; }
    int y() const { return m_y; }

private:
    int m_x = 0x80, m_y = 0x80;
    uint8_t m_buttons = 0;
    uint8_t m_result = 0;
};

/** The CoCo Max in the port or in a Multi-Pak slot, else nullptr. */
CocoMax* findCocoMax(Cart* port);

}  // namespace coco
