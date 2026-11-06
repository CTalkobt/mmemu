#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/isignal_line.h"
#include <string>

/**
 * C64 PLA (Programmable Logic Array): the banking controller.
 *
 * The memory configuration is the 6510 port's LORAM (bit 0), HIRAM (1) and
 * CHAREN (2) plus the expansion port's EXROM (3) and GAME (4), each of those
 * two set while the cartridge pulls the line low. What each of the 32
 * configurations maps follows VICE (c64meminit.c, c64mem.c):
 *
 *   $8000-$9FFF  ROML          8 KB/16 KB game with LORAM and HIRAM; Ultimax
 *   $A000-$BFFF  BASIC         LORAM and HIRAM, no cartridge or 8 KB game
 *                ROMH          16 KB game with HIRAM
 *   $D000-$DFFF  I/O           CHAREN and LORAM or HIRAM (any cartridge
 *                              mode but Ultimax); always in Ultimax
 *                character ROM no CHAREN and LORAM or HIRAM (16 KB game: HIRAM)
 *   $E000-$FFFF  KERNAL        HIRAM (not Ultimax)
 *                ROMH          Ultimax
 *   Ultimax:     $1000-$7FFF, $A000-$CFFF are open (read $FF here; VICE
 *                returns the VIC's last fetch) and drop writes
 *
 * Everything else is RAM. Writes under ROM go to RAM; writes to $D000-$DFFF
 * reach the I/O chips only while I/O is mapped there.
 *
 * The PLA registers at $1000 so it sees every access it may have to
 * redirect; it answers RAM reads in that range itself. When it returns false
 * for $D000-$DFFF the IORegistry goes on to the VIC-II, SID, CIAs...
 *
 * Usage (machine factory):
 *   auto* pla = new C64PLA();
 *   pla->setSignals(cpu6510->signalLoram(),
 *                   cpu6510->signalHiram(),
 *                   cpu6510->signalCharen());
 *   pla->setBasicRom (basicBuf,  8192);
 *   pla->setKernalRom(kernalBuf, 8192);
 *   pla->setCharRom  (charBuf,   4096);
 *   pla->setRamData  (ram);
 *   ioRegistry->registerHandler(pla);
 */
class C64PLA : public IOHandler {
public:
    C64PLA() = default;
    ~C64PLA() override = default;

    // -----------------------------------------------------------------------
    // Configuration (call before registering in IORegistry)
    // -----------------------------------------------------------------------
    void setSignals(ISignalLine* loram, ISignalLine* hiram, ISignalLine* charen) {
        m_loram  = loram;
        m_hiram  = hiram;
        m_charen = charen;
    }

    /** Generic named-signal setter: "loram", "hiram", "charen". */
    void setSignalLine(const char* name, ISignalLine* line) override {
        std::string n(name);
        if (n == "loram")  m_loram  = line;
        else if (n == "hiram")  m_hiram  = line;
        else if (n == "charen") m_charen = line;
    }

    void setBasicRom (const uint8_t* data, uint32_t size) override { m_basicRom  = data; m_basicSize  = size; }
    void setKernalRom(const uint8_t* data, uint32_t size) override { m_kernalRom = data; m_kernalSize = size; }
    void setCharRom  (const uint8_t* data, uint32_t size) override { m_charRom   = data; m_charSize   = size; }

    /** The flat system RAM (64 KB): what the PLA serves where no ROM or I/O
     *  is mapped, and where it puts writes that must not reach the I/O chips. */
    void setRamData(const uint8_t* ram) override { m_ram = const_cast<uint8_t*>(ram); }

    /** The expansion port (see IOHandler::setCartridgePort). */
    bool setCartridgePort(const uint8_t* roml, uint32_t romlSize,
                          const uint8_t* romh, uint32_t romhSize,
                          bool exrom, bool game) override {
        m_roml = roml; m_romlSize = romlSize;
        m_romh = romh; m_romhSize = romhSize;
        m_exrom = exrom; m_game = game;
        return true;
    }

    /** The current memory configuration, 0-31 (VICE's mem_config). */
    int config() const {
        return (loram() ? 1 : 0) | (hiram() ? 2 : 0) | (charen() ? 4 : 0) |
               (m_exrom ? 8 : 0) | (m_game ? 16 : 0);
    }

    // -----------------------------------------------------------------------
    // IOHandler interface
    // -----------------------------------------------------------------------
    const char* name()     const override { return "C64PLA"; }
    uint32_t    baseAddr() const override { return 0x1000; }  // sort before $D000 devices
    uint32_t    addrMask() const override { return 0xFFFF; }  // range checked internally

    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t  val) override;
    void reset() override;
    void tick(uint64_t) override {}

private:
    ISignalLine* m_loram  = nullptr;
    ISignalLine* m_hiram  = nullptr;
    ISignalLine* m_charen = nullptr;

    const uint8_t* m_basicRom  = nullptr;  uint32_t m_basicSize  = 0;
    const uint8_t* m_kernalRom = nullptr;  uint32_t m_kernalSize = 0;
    const uint8_t* m_charRom   = nullptr;  uint32_t m_charSize   = 0;
    const uint8_t* m_roml      = nullptr;  uint32_t m_romlSize   = 0;
    const uint8_t* m_romh      = nullptr;  uint32_t m_romhSize   = 0;
    uint8_t*       m_ram       = nullptr;
    bool m_exrom = false, m_game = false;

    bool hiram()  const { return m_hiram  ? m_hiram->get()  : true; }
    bool loram()  const { return m_loram  ? m_loram->get()  : true; }
    bool charen() const { return m_charen ? m_charen->get() : true; }
};
