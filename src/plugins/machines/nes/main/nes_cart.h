#pragma once

// NES cartridges: the iNES / NES 2.0 image format and the mappers.
//
// After Mesen2 (Core/NES/BaseMapper.cpp, NesHeader.cpp, Mappers/Nintendo/
// NROM.h, MMC1.h, UNROM.h, CNROM.h, MMC3.h, AXROM.h, A12Watcher.h),
// GPL-3.0, Sour and the Mesen contributors, and the nesdev wiki mapper pages.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nes {

enum class Mirroring { Horizontal, Vertical, SingleA, SingleB, FourScreen };

/** What an iNES / NES 2.0 header says. */
struct RomInfo {
    int mapper = 0;
    int submapper = 0;
    bool nes2 = false;
    bool battery = false;
    bool trainer = false;
    Mirroring mirroring = Mirroring::Horizontal;
    uint32_t prgSize = 0;          // bytes
    uint32_t chrSize = 0;          // bytes of CHR ROM (0: CHR RAM)
    uint32_t chrRamSize = 0;       // bytes of CHR RAM
    uint32_t prgRamSize = 0x2000;  // bytes of work / save RAM at $6000
};

/** Parse the 16-byte header; false with `err` if `image` is not an iNES file. */
bool parseHeader(const std::vector<uint8_t>& image, RomInfo& info, std::string& err);

/** A mapper's display name ("MMC3", "NROM"...), or "mapper N". */
std::string mapperName(int mapper);

/**
 * A cartridge: PRG ROM and work RAM on the CPU's bus ($4020-$FFFF), CHR ROM
 * or RAM and the nametable mirroring on the PPU's ($0000-$3EFF). The base
 * class banks PRG in 8 KB slots ($8000/$A000/$C000/$E000) and CHR in 1 KB
 * slots; each mapper sets them from its registers.
 */
class Cartridge {
public:
    /** The cartridge for an iNES image, or nullptr with `err` (bad image, mapper not supported). */
    static std::unique_ptr<Cartridge> load(const std::vector<uint8_t>& image, std::string& err);
    /** Mappers implemented: 0 NROM, 1 MMC1, 2 UxROM, 3 CNROM, 4 MMC3, 7 AxROM. */
    static bool supported(int mapper);

    virtual ~Cartridge() = default;

    const RomInfo& info() const { return m_info; }
    /** "MMC3, 256K PRG, 256K CHR" and so on. */
    std::string describe() const;
    /** Bank registers in words, for the debugger. */
    virtual std::string state() const;

    /** Power-on / reset. */
    virtual void reset();

    // CPU side, $4020-$FFFF. `cycle` is the CPU cycle of a write (MMC1 ignores
    // writes on consecutive cycles, as the 6502's read-modify-write makes).
    virtual uint8_t cpuRead(uint16_t addr, uint8_t openBus);
    virtual uint8_t cpuPeek(uint16_t addr, uint8_t openBus) const;
    virtual void cpuWrite(uint16_t addr, uint8_t value, uint64_t cycle);

    // PPU side, $0000-$3EFF (pattern tables and nametables)
    uint8_t ppuRead(uint16_t addr) const;
    void ppuWrite(uint16_t addr, uint8_t value);
    /** The PPU drove `addr` on its bus at PPU dot `dot` (MMC3 watches A12). */
    virtual void ppuAddress(uint16_t /*addr*/, uint64_t /*dot*/) {}

    /** The cartridge's /IRQ. */
    virtual bool irq() const { return false; }

    /** Work/save RAM at $6000-$7FFF (battery saves). */
    std::vector<uint8_t>& prgRam() { return m_prgRam; }

protected:
    explicit Cartridge(RomInfo info, std::vector<uint8_t> prg, std::vector<uint8_t> chr);

    /** Map 8 KB PRG bank `bank` (negative: from the end) at slot 0-3 ($8000 + slot*$2000). */
    void prg8(int slot, int bank);
    /** 16 KB bank `bank` (negative: from the end) at slot 0 ($8000) or 1 ($C000). */
    void prg16(int slot, int bank);
    /** 32 KB bank `bank` at $8000. */
    void prg32(int bank);
    /** Map 1 KB CHR bank `bank` at slot 0-7 ($0000 + slot*$400). */
    void chr1(int slot, int bank);
    /** 4 KB CHR bank at slot 0 ($0000) or 1 ($1000). */
    void chr4(int slot, int bank) { for (int i = 0; i < 4; ++i) chr1(slot * 4 + i, bank * 4 + i); }
    /** 8 KB CHR bank. */
    void chr8(int bank) { for (int i = 0; i < 8; ++i) chr1(i, bank * 8 + i); }
    void setMirroring(Mirroring m);

    RomInfo m_info;
    std::vector<uint8_t> m_prg, m_chr, m_prgRam;
    std::vector<uint8_t> m_vram;   // 2 KB of console nametable RAM (4 KB for four-screen)
    bool m_chrIsRam = false;
    bool m_prgRamEnabled = true, m_prgRamWritable = true;
    uint32_t m_prgMap[4] = {};
    uint32_t m_chrMap[8] = {};
    uint32_t m_ntMap[4] = {};
    Mirroring m_mirroring = Mirroring::Horizontal;
};

}  // namespace nes
