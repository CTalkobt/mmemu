#pragma once

// The SPC7110 in Far East of Eden Zero, Momotarou Dentetsu Happy and
// Super Power League 4 (#15).
//
// After Mesen2 (Core/SNES/Coprocessors/SPC7110/), GPL-3.0, Sour and the
// Mesen contributors; the decompressor is bsnes' (byuu, after neviksti and
// talarubi). The first 1 MB of ROM is the program (HiROM at $00-$3F/$80-$BF
// $8000-$FFFF and $C0-$CF); the rest is data ROM, three 1 MB windows of it
// at $D0-$FF ($4831-$4833). The chip has:
//   - a decompressor ($4800-$480C), read at $4800 or anywhere in bank $50,
//     working from a directory of 4-byte entries (mode, source address);
//   - a data port ($4810-$481A) reading data ROM with a base, an offset and
//     a step;
//   - a multiply/divide unit ($4820-$482F);
//   - battery RAM at $6000-$7FFF;
//   - in Far East of Eden Zero, an Epson RTC-4513 clock ($4840-$4842).

#include <cstdint>
#include <functional>
#include <vector>

namespace snes {

class Spc7110 {
public:
    /** `rom` and `sram` belong to the cartridge. */
    Spc7110(const std::vector<uint8_t>* rom, std::vector<uint8_t>* sram, bool rtc);

    void reset();
    /** The S-CPU's view: -1 where the cartridge doesn't answer. */
    int cpuRead(uint8_t bank, uint16_t a, bool peek);
    /** True if the cartridge took the write. */
    bool cpuWrite(uint8_t bank, uint16_t a, uint8_t v);

    bool hasRtc() const { return m_rtc; }
    /** The clock's state for the battery file: 16 registers and the time they held (Mesen's .rtc layout). */
    std::vector<uint8_t> rtcState() const;
    void setRtcState(const std::vector<uint8_t>& data);
    /** The wall clock in seconds since 1970; tests replace it before first use. */
    std::function<int64_t()> clock;

    /** A byte of data ROM (offset from the end of the program ROM). */
    uint8_t dataRom(uint32_t addr) const;

private:
    // The decompressor (bsnes')
    struct Decomp {
        struct Context { uint8_t prediction = 0, swap = 0; };
        Context context[5][15];
        uint32_t bpp = 1, offset = 0, bits = 0;
        uint16_t range = 0, input = 0;
        uint8_t output = 0;
        uint64_t pixels = 0, colormap = 0;
        uint32_t result = 0;
        void init(const Spc7110* spc, uint32_t mode, uint32_t origin);
        void decode(const Spc7110* spc);
    };

    uint8_t regRead(uint16_t a, bool peek);
    void regWrite(uint16_t a, uint8_t v);
    uint8_t rtcRead(uint16_t a);
    void rtcWrite(uint16_t a, uint8_t v);
    void rtcUpdate();

    void fillReadBuffer();
    void incrementPosition();
    void incrementPosition4810();
    void loadEntryHeader();
    void beginDecompression();
    uint8_t readDecompressedByte();

    const std::vector<uint8_t>* m_rom;
    std::vector<uint8_t>* m_sram;
    bool m_rtc;
    uint32_t m_dataRomSize = 0;      // the data ROM actually present
    Decomp m_decomp;

    // Decompression
    uint32_t m_directoryBase = 0;
    uint8_t m_directoryIndex = 0;
    uint16_t m_targetOffset = 0, m_dataLengthCounter = 0;
    uint8_t m_skipBytes = 0, m_decompFlags = 0, m_decompMode = 0;
    uint32_t m_srcAddress = 0, m_decompOffset = 0;
    uint8_t m_decompStatus = 0;
    uint8_t m_decompBuffer[32] = {};
    // The ALU
    uint32_t m_dividend = 0;
    uint16_t m_multiplier = 0, m_divisor = 0;
    uint32_t m_result = 0;
    uint16_t m_remainder = 0;
    uint8_t m_aluState = 0, m_aluFlags = 0;
    // The map
    uint8_t m_sramEnabled = 0;
    uint8_t m_banks[3] = {0, 1, 2};
    uint8_t m_dataRomSizeReg = 0;
    // The data port
    uint32_t m_readBase = 0;
    uint16_t m_readOffset = 0, m_readStep = 0;
    uint8_t m_readMode = 0, m_readBuffer = 0;
    // The RTC-4513
    int64_t m_rtcLastTime = 0;
    uint8_t m_rtcEnabled = 0;
    int8_t m_rtcMode = -1, m_rtcIndex = -1;
    uint8_t m_rtcRegs[16] = {};
};

}  // namespace snes
