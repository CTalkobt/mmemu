#pragma once

// The Game Boy (DMG), the Game Boy Color (CGB), and the Game Boy inside the
// Super Game Boy, after Mesen2 (Core/Gameboy/: Gameboy.cpp, GbMemoryManager.cpp,
// GbTimer.cpp, GbDmaController.cpp, GbControlManager.cpp; GPL-3.0, Sour and the
// Mesen contributors).
//
// Time is the 4.194304 MHz clock (T-cycles; on the Super Game Boy the SNES
// master clock divided by 5). As in Mesen the CPU (gb_cpu.h) drives
// everything: each of its machine cycles is two exec() calls of two clocks,
// each running the timer, OAM DMA, the serial port and two PPU dots
// (gb_ppu.h); the APU (gb_apu.h) catches up when it is touched.
//
// The memory map is 256 pages of 256 bytes, each either memory (ROM, work
// RAM, cartridge RAM, the boot ROM) or registers (VRAM, OAM, $FF00-$FFFF and
// whatever the cartridge's mapper claims, gb_cart.h).
//
// Without a boot ROM the DMG starts in the state its boot ROM leaves (Pan
// Docs' table); the Super Game Boy and the Game Boy Color run SameBoy's open
// source SGB and CGB boot ROMs (LIJI32, MIT licence, as Mesen embeds them).
// The SGB's sends the cartridge's header to the SNES as the real one does; the
// CGB's turns the colour features off for a DMG cartridge (header $0143 bit 7
// clear: KEY0 $FF4C) and gives it palettes by its title, as Nintendo's does.
//
// The CGB adds (as Mesen): double speed (KEY1 and STOP: everything but the
// PPU and the APU's clock runs twice as fast), VRAM bank 1 (VBK $FF4F) with
// the background's attribute map, work RAM banks 1-7 (SVBK $FF70), the colour
// palettes (BCPS/BCPD/OCPS/OCPD $FF68-$FF6B), HDMA ($FF51-$FF55, at once or a
// block each HBlank), PCM12/PCM34 ($FF76/$FF77), the undocumented $FF72-$FF75
// and the infrared port's register (RP $FF56, nothing connected).

#include "gb_apu.h"
#include "gb_cart.h"
#include "gb_cpu.h"
#include "gb_ppu.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gb {

enum Irq : uint8_t { IRQ_VBLANK = 0x01, IRQ_STAT = 0x02, IRQ_TIMER = 0x04, IRQ_SERIAL = 0x08, IRQ_JOYPAD = 0x10 };

/** The joypad's buttons for setButtons (1 = pressed). */
enum Button : uint8_t {
    BTN_RIGHT = 0x01, BTN_LEFT = 0x02, BTN_UP = 0x04, BTN_DOWN = 0x08,
    BTN_A = 0x10, BTN_B = 0x20, BTN_SELECT = 0x40, BTN_START = 0x80,
};

/**
 * The Super Game Boy's ICD2 as the Game Boy sees it (Mesen's SuperGameboy calls):
 * the LCD's pixels and blanking, and the joypad port.
 */
class SgbLink {
public:
    virtual ~SgbLink() = default;
    virtual void hblank() = 0;
    virtual void vblank() = 0;
    /** Pixel `x` of the line being drawn: its shade, 0-3. */
    virtual void lcdPixel(uint8_t x, uint8_t shade) = 0;
    /** P14/P15 written ($FF00 bits 4-5): the packet protocol and the player switch. */
    virtual void joypadWrite(uint8_t select) = 0;
    /** $FF00's low nibble with both lines high: $F minus the current player. */
    virtual uint8_t joypadId() = 0;
    /** The current player's buttons as the ICD2 holds them (active low: high nibble buttons, low the pad). */
    virtual uint8_t joypadButtons() = 0;
};

/** The debugger's view of the CPU's data reads and writes (not opcode or operand fetches). */
class AccessHook {
public:
    virtual ~AccessHook() = default;
    virtual void cpuRead(uint16_t addr, uint8_t v) = 0;
    virtual void cpuWrite(uint16_t addr, uint8_t before, uint8_t v) = 0;
};

class Gameboy {
public:
    enum class Model { Dmg, Sgb, Sgb2, Cgb };
    enum class Mem { None, Rom, WorkRam, CartRam, BootRom };
    enum Access : uint8_t { ACC_NONE = 0, ACC_READ = 1, ACC_WRITE = 2, ACC_RW = 3 };

    static constexpr uint32_t CLOCK_HZ = 4194304;

    Gameboy();
    ~Gameboy();

    /** Load a cartridge image (.gb, .gbc); false and `err` if its mapper isn't supported. */
    bool load(std::vector<uint8_t> rom, std::string& err);
    bool loaded() const { return (bool)m_cart; }
    const Header& header() const { return m_header; }
    const char* mapperName() const { return m_cart ? m_cart->name() : ""; }

    /** The model; Sgb/Sgb2 and Cgb run their built-in boot ROM unless one is given. */
    void setModel(Model m) { m_model = m; }
    Model model() const { return m_model; }
    bool isSgb() const { return m_model == Model::Sgb || m_model == Model::Sgb2; }
    bool isCgb() const { return m_model == Model::Cgb; }
    /** A boot ROM (256 bytes; the CGB's 2304, $0000-$00FF and $0200-$08FF) to run; empty: the built-in choice (none for the DMG). */
    void setBootRom(std::vector<uint8_t> rom) { m_bootRomOverride = std::move(rom); }
    /** The CGB without a boot ROM: it starts where the boot ROM leaves off (the DMG never has a built-in one). */
    void setSkipBootRom(bool skip) { m_skipBootRom = skip; }

    /** Power on (the cartridge's RAM is kept). `sgb`: the ICD2, for the Super Game Boy. */
    void powerOn(SgbLink* sgb = nullptr);
    /** Run whole instructions until the clock reaches `cycle`. */
    void runTo(uint64_t cycle);
    /** Run to the end of the next frame (the LCD's vertical blank, or a frame's time with it off). */
    void runFrame();
    uint64_t cycles() const { return m_cpu.cycles(); }

    // The screen: 160x144 shades (0 white - 3 black), the last frame
    const uint8_t* screen() const { return m_ppu.frame(); }
    /** The CGB's screen: 160x144 colours, 15-bit BGR (bits 0-4 red, 5-9 green, 10-14 blue). */
    const uint16_t* colorScreen() const { return m_ppu.colorFrame(); }
    uint32_t frameCount() const { return m_ppu.frameCount(); }

    /** The joypad (Button bits) for the plain Game Boy. */
    void setButtons(uint8_t buttons);

    /** Audio: stereo samples at `hz` of the Game Boy's clock (`clockHz` on the SGB, where it runs off the SNES's). */
    Apu& apu() { return m_apu; }

    // Battery RAM (and the MBC3's clock, Mesen's .rtc)
    bool hasBattery() const { return m_header.hasBattery(); }
    std::vector<uint8_t>& cartRam() { return m_cartRam; }
    Mbc3Rtc* rtc() { return m_cart ? m_cart->rtc() : nullptr; }

    /** Bytes the serial port sends with the internal clock (Blargg's tests print through it). */
    const std::string& serialOutput() const { return m_serialOut; }

    // For tests and the debugger
    Cpu& cpu() { return m_cpu; }
    /** The work RAM: 8 KB, the CGB's 32 KB (bank n at n * $1000). */
    std::vector<uint8_t>& workRam() { return m_wram; }
    /** The work RAM bank at $D000 (1 but on a CGB in CGB mode). */
    uint8_t wramBank() const;
    /** In the CGB's double speed. */
    bool isHighSpeed() const { return m_highSpeed; }
    Ppu& ppu() { return m_ppu; }
    uint8_t peek(uint16_t addr);
    /** A write with no side effect on registers (only memory pages). */
    void poke(uint16_t addr, uint8_t v);
    /** Told of the CPU's data accesses (null: none; the debugger's watchpoints). */
    void setAccessHook(AccessHook* h) { m_accessHook = h; }
    AccessHook* accessHook() const { return m_accessHook; }
    bool bootRomDisabled() const { return m_bootRomOff; }

    // ---------------------------------------------------------------
    // What the parts use (Mesen's GbMemoryManager)
    // ---------------------------------------------------------------
    /** Two clocks: the timer, OAM DMA, the serial port, two PPU dots. */
    void exec();
    /** One clock (HALT's and some register writes' finer timing). */
    void execMasterCycle();
    template <OamCorruption C> uint8_t read(uint16_t addr);
    void cpuWrite(uint16_t addr, uint8_t v);
    void write(uint16_t addr, uint8_t v);
    void requestIrq(uint8_t src) { m_if |= src; }
    void clearIrq(uint8_t src) { m_if &= (uint8_t)~src; }
    /** The highest priority interrupt requested and enabled, or 0. */
    uint8_t pendingIrq() const;
    uint64_t apuCycles() const { return m_apuCycles; }
    SgbLink* sgb() const { return m_sgb; }
    /**
     * Run `f` (a change to what $FF00 reads) and raise the joypad interrupt if one of
     * P10-P13 fell (Pan Docs: high to low; Mesen raises it on any change to the
     * reading, select bits included, which the SGB's boot leaves no trace of: boot_hwio-S).
     */
    template <class F> void inputChange(F f) {
        const uint8_t before = readJoypad();
        f();
        if (before & ~readJoypad() & 0x0F) requestIrq(IRQ_JOYPAD);
    }
    uint8_t readJoypad() const;
    bool cpuHalted() const { return m_cpu.halted(); }
    uint8_t joypadLines() const { return m_joySelect; }

    // OAM DMA (Mesen's GbDmaController)
    bool oamDmaRunning() const { return m_dmaRunning; }
    uint8_t dmaLastWriteAddress() const { return m_dmaCounter <= 160 ? (uint8_t)(160 - m_dmaCounter) : 0; }

    // The CGB's HDMA and speed switch (Mesen's GbDmaController::ProcessHdma, GbMemoryManager::ToggleSpeed)
    /** A PPU mode change or the CPU leaving HALT: the HBlank block of HDMA, if one is due. */
    void processHdma();
    bool speedSwitchRequested() const { return m_speedSwitchRequest; }
    void toggleSpeed() {
        m_speedSwitchRequest = false;
        m_highSpeed = !m_highSpeed;
        m_apu.speedChanged(m_highSpeed);
    }
    /** DIV reset (by STOP: `duringStop`). */
    void resetDivider(bool duringStop);

    // The map, for the mappers
    void map(uint16_t start, uint16_t end, Mem type, uint32_t offset, bool readOnly);
    void unmap(uint16_t start, uint16_t end);
    void mapRegisters(uint16_t start, uint16_t end, uint8_t access);
    std::vector<uint8_t>& rom() { return m_rom; }

    // The timer (Mesen's GbTimer): the frame sequencer runs off DIV bit 12 (13 in double speed)
    bool frameSequencerBit() const { return (m_div & (m_highSpeed ? 0x2000 : 0x1000)) != 0; }

private:
    void refreshMappings();
    std::vector<uint8_t>* memory(Mem type);
    uint8_t readRegister(uint16_t addr);
    void writeRegister(uint16_t addr, uint8_t v);
    void timerExec();
    void timerWrite(uint16_t addr, uint8_t v);
    uint8_t timerRead(uint16_t addr) const;
    /** The internal counter set to `v` (`counting`: by the clock, not a reset). */
    void setDivider(uint16_t v, bool counting);
    void reloadTimer();
    void dmaExec();
    bool dmaConflict(uint16_t addr) const;
    uint16_t dmaReadAddress() const { return (uint16_t)((m_dmaDest << 8) + (160 - m_dmaCounter)); }
    uint8_t dmaRead(uint16_t addr);
    void timerDmaSerial();
    void hdmaWrite(uint16_t addr, uint8_t v);
    void hdmaBlock();
    void postBootState();
    void cgbPostBootState();
    void sgbBootDuration();

    Model m_model = Model::Dmg;
    Cpu m_cpu;
    Ppu m_ppu;
    Apu m_apu;
    std::unique_ptr<Cart> m_cart;
    Header m_header;
    SgbLink* m_sgb = nullptr;
    AccessHook* m_accessHook = nullptr;

    std::vector<uint8_t> m_rom, m_cartRam, m_wram, m_bootRom, m_bootRomOverride;
    uint8_t m_hram[0x7F] = {};

    uint8_t* m_reads[256] = {};
    uint8_t* m_writes[256] = {};
    bool m_readReg[256] = {}, m_writeReg[256] = {};

    uint8_t m_ie = 0, m_if = 0;
    bool m_bootRomOff = false;
    bool m_sgbBootRom = false;                           // the built-in SGB boot ROM is running
    bool m_skipBootRom = false;
    uint64_t m_apuCycles = 0;
    uint8_t m_serialData = 0, m_serialControl = 0, m_serialBits = 0;
    std::string m_serialOut;
    uint8_t m_joySelect = 0x30, m_buttons = 0;

    // The timer
    uint16_t m_div = 0;
    bool m_needReload = false, m_reloaded = false, m_timerOn = false;
    uint8_t m_tima = 0, m_tma = 0, m_tac = 0;
    uint16_t m_timerBit = 1024;
    uint8_t m_frameSeqDelay = 0;                         // a frame sequencer edge due in this many exec()s
    bool m_frameSeqDelayedRise = false;

    // OAM DMA
    uint8_t m_dmaSource = 0xFF, m_dmaStartDelay = 0, m_dmaDest = 0, m_dmaCounter = 0, m_dmaBuffer = 0;
    bool m_dmaRunning = false;

    // The CGB's: speed, work RAM bank, HDMA, the undocumented and infrared registers
    bool m_highSpeed = false, m_speedSwitchRequest = false;
    uint8_t m_wramBank = 1;
    uint16_t m_hdmaSource = 0, m_hdmaDest = 0;
    uint8_t m_hdmaLength = 0;
    bool m_hdmaRunning = false, m_hdmaPending = false, m_hdmaTrigger = false;
    uint8_t m_ff72 = 0, m_ff73 = 0, m_ff74 = 0, m_ff75 = 0, m_rp = 0;
};

}  // namespace gb
