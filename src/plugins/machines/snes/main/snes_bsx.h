#pragma once

// The Satellaview (BS-X) (#15), after Mesen2 (Core/SNES/Coprocessors/BSX/,
// GPL-3.0, Sour and the Mesen contributors).
//
// The BS-X BIOS cartridge (chipset $E5) has 512 KB of PSRAM, a slot for an
// 8M flash memory pack and sixteen 1-bit registers at $00-$0F:5000 that
// lay out the map: the pack and the PSRAM as LoROM or HiROM, where the
// PSRAM sits, holes in the pack, and the BIOS over banks $00-$3F/$80-$BF.
// A register write is pending until $0E:5000 is written. As in Mesen the
// map is a table of 4 KB pages that each change rewrites a part of, so what
// an earlier layout put somewhere stays until something covers it.
//
// The Satellaview itself (on the expansion port) is two data streams at
// $2188-$2193 and status registers at $2194-$219A. Channel 0 is the time
// (the host's, or a date given); other channels play files
// BSX<channel>-<n>.bin (packets of 22 bytes), as Mesen, from a folder.
//
// Some ordinary cartridges have a pack slot too (game code Zxxx, maker
// $33): BsxPack is used alone there.

#include <cstdint>
#include <ctime>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace snes {

/** The flash memory pack (Mesen's BsxMemoryPack): its data and its command state. */
class BsxPack {
public:
    explicit BsxPack(std::vector<uint8_t> data);
    std::vector<uint8_t>& data() { return m_data; }
    uint32_t pages() const { return (uint32_t)(m_data.size() / 0x1000); }

    /** Read through 4 KB page `page` (Mesen's handler for it); `peek` without side effects. */
    uint8_t read(uint32_t page, uint16_t a, bool peek);
    void write(uint32_t page, uint16_t a, uint8_t v);
    void reset();

private:
    uint8_t& at(uint32_t page, uint16_t a);
    void command(uint8_t v, uint32_t page);

    std::vector<uint8_t> m_data;
    uint8_t m_calculatedSize = 0x0C;
    bool m_enableCsr = false, m_enableEsr = false, m_enableVendorInfo = false, m_writeByte = false;
    uint16_t m_command = 0;
};

/** One of the Satellaview's two streams (Mesen's BsxStream). */
class BsxStream {
public:
    void reset(int64_t date, const std::string& dir);
    uint16_t channel() const { return m_channel; }
    bool needUpdate() const { return m_queueLength > 0; }
    bool fillQueues();

    uint8_t prefixCount();
    uint8_t prefix();
    uint8_t data(uint64_t master, uint64_t masterHz);
    uint8_t status(bool reset);

    void setChannelLow(uint8_t v);
    void setChannelHigh(uint8_t v);
    void setPrefixLatch(uint8_t v) { m_prefixLatch = v != 0; m_prefixQueueLength = 0; }
    void setDataLatch(uint8_t v) { m_dataLatch = v != 0; m_dataQueueLength = 0; }

private:
    bool loadFile();
    uint8_t time(uint64_t master, uint64_t masterHz);

    std::string m_dir;
    std::ifstream m_file;
    std::tm m_tm = {};
    uint16_t m_channel = 0;
    uint8_t m_prefix = 0, m_data = 0, m_status = 0;
    bool m_prefixLatch = false, m_dataLatch = false, m_firstPacket = false;
    uint32_t m_fileOffset = 0;
    uint8_t m_fileIndex = 0;
    uint16_t m_queueLength = 0;
    uint8_t m_prefixQueueLength = 0, m_dataQueueLength = 0;
    uint16_t m_activeChannel = 0;
    uint8_t m_activeFileIndex = 0;
    int64_t m_date = 0;
};

/** The BS-X cartridge and the Satellaview. */
class Bsx {
public:
    static constexpr uint32_t PSRAM_SIZE = 512 * 1024;

    /** `rom`: the BIOS (LoROM); `sram`: its RAM; `pack`: the memory pack (empty: none). */
    Bsx(const std::vector<uint8_t>* rom, std::vector<uint8_t>* sram, std::vector<uint8_t> pack);

    /** Power on: the cartridge's own map, then the registers' reset. */
    void powerOn();
    /** The RESET button (Mesen's BsxCart::Reset): the registers and the map they give. */
    void reset();

    /** The cartridge's side: -1 for open bus. `openBus` is the bus's value before the read. */
    int read(uint8_t bank, uint16_t a, uint8_t openBus, bool peek);
    void write(uint8_t bank, uint16_t a, uint8_t v);

    /** $2188-$219F: -1 when it's not the Satellaview's (the B bus as usual). */
    int satRead(uint8_t r, uint64_t master, uint64_t masterHz, bool peek);
    /** True when the Satellaview took the write. */
    bool satWrite(uint8_t r, uint8_t v, uint64_t master, uint64_t masterHz);

    /** The time the Satellaview gives from reset (seconds since 1970; -1: the host's clock), and where the streams' files are. */
    void setDate(int64_t date) { m_customDate = date; }
    void setStreamDir(const std::string& dir) { m_streamDir = dir; }

    BsxPack* pack() { return m_pack.get(); }
    std::vector<uint8_t>& psram() { return m_psram; }
    bool reg(int i) const { return m_regs[i & 15]; }

private:
    enum Kind : uint8_t { NONE, ROM, SRAM, PACK, PSRAM, REGS };
    struct Page { uint8_t kind = NONE; uint32_t index = 0; };

    uint32_t pagesOf(Kind k) const;
    /** Mesen's MemoryMappings::RegisterHandler over a kind's 4 KB pages (none: nothing changes). */
    void map(uint8_t b0, uint8_t b1, uint16_t a0, uint16_t a1, Kind k, uint16_t increment = 0, uint16_t start = 0);
    /** A hole (Mesen's null handler). */
    void unmap(uint8_t b0, uint8_t b1, uint16_t a0, uint16_t a1);
    void updateMap();
    void processClocks(uint64_t master, uint64_t masterHz);
    void satReset();

    const std::vector<uint8_t>* m_rom;
    std::vector<uint8_t>* m_sram;
    std::unique_ptr<BsxPack> m_pack;
    std::vector<uint8_t> m_psram;
    Page m_map[0x1000];
    bool m_regs[16] = {}, m_dirtyRegs[16] = {};
    bool m_dirty = false;

    BsxStream m_stream[2];
    uint8_t m_streamReg = 0, m_extOutput = 0xFF;
    int64_t m_customDate = -1;
    std::string m_streamDir;
    uint64_t m_prevMaster = 0;
};

}  // namespace snes
