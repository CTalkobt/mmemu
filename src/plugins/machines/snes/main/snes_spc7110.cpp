// The SPC7110 after Mesen2 (Core/SNES/Coprocessors/SPC7110/Spc7110.cpp,
// Spc7110Decomp.cpp, Rtc4513.cpp), GPL-3.0, Sour and the Mesen contributors.
// The decompressor is bsnes' (byuu; neviksti's original, talarubi's
// optimised version).

#include "snes_spc7110.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <ctime>

namespace snes {

namespace {

struct ModelState { uint8_t probability, next[2]; };
const ModelState EVOLUTION[53] = {
    {0x5a, { 1, 1}}, {0x25, { 2, 6}}, {0x11, { 3, 8}},
    {0x08, { 4,10}}, {0x03, { 5,12}}, {0x01, { 5,15}},

    {0x5a, { 7, 7}}, {0x3f, { 8,19}}, {0x2c, { 9,21}},
    {0x20, {10,22}}, {0x17, {11,23}}, {0x11, {12,25}},
    {0x0c, {13,26}}, {0x09, {14,28}}, {0x07, {15,29}},
    {0x05, {16,31}}, {0x04, {17,32}}, {0x03, {18,34}},
    {0x02, { 5,35}},

    {0x5a, {20,20}}, {0x48, {21,39}}, {0x3a, {22,40}},
    {0x2e, {23,42}}, {0x26, {24,44}}, {0x1f, {25,45}},
    {0x19, {26,46}}, {0x15, {27,25}}, {0x11, {28,26}},
    {0x0e, {29,26}}, {0x0b, {30,27}}, {0x09, {31,28}},
    {0x08, {32,29}}, {0x07, {33,30}}, {0x05, {34,31}},
    {0x04, {35,33}}, {0x04, {36,33}}, {0x03, {37,34}},
    {0x02, {38,35}}, {0x02, { 5,36}},

    {0x58, {40,39}}, {0x4d, {41,47}}, {0x43, {42,48}},
    {0x3b, {43,49}}, {0x34, {44,50}}, {0x2e, {45,51}},
    {0x29, {46,44}}, {0x25, {24,45}},

    {0x56, {48,47}}, {0x4f, {49,47}}, {0x47, {50,48}},
    {0x41, {51,49}}, {0x3c, {52,50}}, {0x37, {43,51}},
};

enum : uint32_t { MPS = 0, LPS = 1 };
enum : uint32_t { HALF = 0x55, MAX = 0xFF };

// The inverse Morton transform: unpack big-endian packed pixels, odd bits in the low half, even bits in the high
uint32_t deinterleave(uint64_t data, uint32_t bits) {
    data = data & ((1ull << bits) - 1);
    data = 0x5555555555555555ull & (data << bits | data >> 1);
    data = 0x3333333333333333ull & (data | data >> 1);
    data = 0x0f0f0f0f0f0f0f0full & (data | data >> 2);
    data = 0x00ff00ff00ff00ffull & (data | data >> 4);
    data = 0x0000ffff0000ffffull & (data | data >> 8);
    return (uint32_t)(data | data >> 16);
}

// Move a nibble of the most-recently-used list to the front
uint64_t moveToFront(uint64_t list, uint32_t nibble) {
    for (uint64_t n = 0, mask = ~15ull; n < 64; n += 4, mask <<= 4) {
        if ((list >> n & 15) != nibble) continue;
        return (list & mask) + (list << 4 & ~mask) + nibble;
    }
    return list;
}

int64_t systemSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

void Spc7110::Decomp::init(const Spc7110* spc, uint32_t mode, uint32_t origin) {
    std::memset(context, 0, sizeof context);
    bpp = 1u << mode;
    offset = origin;
    bits = 8;
    range = MAX + 1;
    input = spc->dataRom(offset++);
    input = (uint16_t)(input << 8 | spc->dataRom(offset++));
    output = 0;
    pixels = 0;
    colormap = 0xfedcba9876543210ull;
}

void Spc7110::Decomp::decode(const Spc7110* spc) {
    for (uint32_t pixel = 0; pixel < 8; ++pixel) {
        uint64_t map = colormap;
        uint32_t diff = 0;
        if (bpp > 1) {
            const uint32_t pa = (uint32_t)(bpp == 2 ? (pixels >> 2) & 3 : (pixels >> 0) & 15);
            const uint32_t pb = (uint32_t)(bpp == 2 ? (pixels >> 14) & 3 : (pixels >> 28) & 15);
            const uint32_t pc = (uint32_t)(bpp == 2 ? (pixels >> 16) & 3 : (pixels >> 32) & 15);
            if (pa != pb || pb != pc) {
                const uint32_t match = pa ^ pb ^ pc;
                diff = 4;                          // all three differ
                if ((match ^ pc) == 0) diff = 3;   // a == b
                if ((match ^ pb) == 0) diff = 2;   // c == a
                if ((match ^ pa) == 0) diff = 1;   // b == c
            }
            colormap = moveToFront(colormap, pa);
            map = moveToFront(map, pc);
            map = moveToFront(map, pb);
            map = moveToFront(map, pa);
        }

        for (uint32_t plane = 0; plane < bpp; ++plane) {
            const uint32_t bit = bpp > 1 ? 1u << plane : 1u << (pixel & 3);
            const uint32_t history = (bit - 1) & output;
            uint32_t set = 0;
            if (bpp == 1) set = pixel >= 4;
            else if (bpp == 2) set = diff;
            if (plane >= 2 && history <= 1) set = diff;

            Context& ctx = context[set][bit + history - 1];
            const ModelState& model = EVOLUTION[ctx.prediction];
            const uint8_t lpsOffset = (uint8_t)(range - model.probability);
            const bool symbol = input >= (lpsOffset << 8);   // only the top byte matters
            output = (uint8_t)(output << 1 | ((uint8_t)symbol ^ ctx.swap));
            if (symbol == MPS) {
                range = lpsOffset;
            } else {
                range = (uint16_t)(range - lpsOffset);
                input = (uint16_t)(input - (lpsOffset << 8));
            }
            while (range <= MAX / 2) {                        // renormalise
                ctx.prediction = model.next[symbol];
                range <<= 1;
                input <<= 1;
                if (--bits == 0) {
                    bits = 8;
                    input = (uint16_t)(input + spc->dataRom(offset++));
                }
            }
            if (symbol == LPS && model.probability > HALF) ctx.swap ^= 1;
        }

        uint32_t index = output & ((1u << bpp) - 1);
        if (bpp == 1) index ^= (uint32_t)(pixels >> 15 & 1);
        pixels = pixels << bpp | (map >> 4 * index & 15);
    }

    if (bpp == 1) result = (uint32_t)pixels;
    else if (bpp == 2) result = deinterleave(pixels, 16);
    else if (bpp == 4) result = deinterleave(deinterleave(pixels, 32), 32);
}

Spc7110::Spc7110(const std::vector<uint8_t>* rom, std::vector<uint8_t>* sram, bool rtc)
    : clock(systemSeconds), m_rom(rom), m_sram(sram), m_rtc(rtc) {
    // Past 6 MB the program ROM gets a second megabyte at $40-$4F (Mesen's non-strict mapping)
    const uint32_t size = (uint32_t)rom->size();
    if (size >= 0x600000) m_dataRomSize = size - 0x200000;
    else m_dataRomSize = size > 0x100000 ? size - 0x100000 : 0;
    reset();
}

void Spc7110::reset() {
    // The clock has its own battery: reset leaves it alone
    m_directoryBase = 0; m_directoryIndex = 0; m_targetOffset = 0; m_dataLengthCounter = 0;
    m_skipBytes = 0; m_decompFlags = 0; m_decompMode = 0; m_srcAddress = 0; m_decompOffset = 0; m_decompStatus = 0;
    std::memset(m_decompBuffer, 0, sizeof m_decompBuffer);
    m_dividend = 0; m_multiplier = 0; m_divisor = 0; m_result = 0; m_remainder = 0; m_aluState = 0; m_aluFlags = 0;
    m_sramEnabled = 0;
    m_banks[0] = 0; m_banks[1] = 1; m_banks[2] = 2;
    m_dataRomSizeReg = 0;
    m_readBase = 0; m_readOffset = 0; m_readStep = 0; m_readMode = 0; m_readBuffer = 0;
    m_decomp = Decomp();
    m_rtcEnabled = 0; m_rtcMode = -1; m_rtcIndex = -1;
}

uint8_t Spc7110::dataRom(uint32_t addr) const {
    const uint32_t configSize = 0x100000u << (m_dataRomSizeReg & 0x03);
    if (addr >= std::min(configSize, m_dataRomSize)) return 0;
    return (*m_rom)[0x100000 + addr];
}

int Spc7110::cpuRead(uint8_t bank, uint16_t a, bool peek) {
    const auto& rom = *m_rom;
    if (rom.empty()) return -1;
    if (bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) {
        if (a >= 0x4800 && a <= 0x4842) return regRead(a, peek);
        if (a >= 0x6000 && a < 0x8000) {
            if (!m_sram || m_sram->empty()) return -1;
            return (*m_sram)[(((bank & 0x3F) << 13) | (a & 0x1FFF)) % m_sram->size()];
        }
        if (a >= 0x8000) return rom[((uint32_t)(bank & 0x3F) << 16 | a) % rom.size()];
        return -1;
    }
    if (bank == 0x50) return regRead(0x4800, peek);
    if (bank == 0x58) return regRead(0x4808, peek);
    if (bank >= 0x40 && bank < 0x50) {
        if (rom.size() < 0x600000) return -1;
        return rom[(0x600000 + ((uint32_t)(bank & 0x0F) << 16 | a)) % rom.size()];
    }
    if (bank >= 0xC0 && bank < 0xD0) return rom[((uint32_t)(bank & 0x0F) << 16 | a) % rom.size()];
    if (bank >= 0xD0) {
        // Three 1 MB windows on the data ROM
        if (m_dataRomSize < 0x1000) return -1;
        const uint32_t window = ((uint32_t)m_banks[(bank - 0xD0) >> 4] << 20) % (m_dataRomSize & ~0xFFFu);
        return rom[(0x100000 + window + ((uint32_t)(bank & 0x0F) << 16 | a)) % rom.size()];
    }
    return -1;
}

bool Spc7110::cpuWrite(uint8_t bank, uint16_t a, uint8_t v) {
    if (bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) {
        if (a >= 0x4800 && a <= 0x4842) { regWrite(a, v); return true; }
        if (a >= 0x6000 && a < 0x8000) {
            if (m_sram && !m_sram->empty()) (*m_sram)[(((bank & 0x3F) << 13) | (a & 0x1FFF)) % m_sram->size()] = v;
            return true;
        }
        return a >= 0x8000;
    }
    if (bank == 0x50 || bank == 0x58) { regWrite(bank == 0x50 ? 0x4800 : 0x4808, v); return true; }
    return bank >= 0xC0;
}

uint8_t Spc7110::regRead(uint16_t a, bool peek) {
    switch (a) {
    // Decompression
    case 0x4800:
        if (peek) return 0;
        --m_dataLengthCounter;
        return readDecompressedByte();
    case 0x4801: return (uint8_t)m_directoryBase;
    case 0x4802: return (uint8_t)(m_directoryBase >> 8);
    case 0x4803: return (uint8_t)(m_directoryBase >> 16);
    case 0x4804: return m_directoryIndex;
    case 0x4805: return (uint8_t)m_targetOffset;
    case 0x4806: return (uint8_t)(m_targetOffset >> 8);
    case 0x4807: return m_skipBytes;
    case 0x4808: return 0;
    case 0x4809: return (uint8_t)m_dataLengthCounter;
    case 0x480A: return (uint8_t)(m_dataLengthCounter >> 8);
    case 0x480B: return m_decompFlags;
    case 0x480C: return m_decompStatus;
    // The data port
    case 0x4810: {
        const uint8_t v = m_readBuffer;
        if (!peek) incrementPosition4810();
        return v;
    }
    case 0x4811: return (uint8_t)m_readBase;
    case 0x4812: return (uint8_t)(m_readBase >> 8);
    case 0x4813: return (uint8_t)(m_readBase >> 16);
    case 0x4814: return (uint8_t)m_readOffset;
    case 0x4815: return (uint8_t)(m_readOffset >> 8);
    case 0x4816: return (uint8_t)m_readStep;
    case 0x4817: return (uint8_t)(m_readStep >> 8);
    case 0x4818: return m_readMode;
    case 0x481A:
        if (!peek && (m_readMode & 0x60) == 0x60) incrementPosition();
        return 0;
    // The ALU
    case 0x4820: return (uint8_t)m_dividend;
    case 0x4821: return (uint8_t)(m_dividend >> 8);
    case 0x4822: return (uint8_t)(m_dividend >> 16);
    case 0x4823: return (uint8_t)(m_dividend >> 24);
    case 0x4824: return (uint8_t)m_multiplier;
    case 0x4825: return (uint8_t)(m_multiplier >> 8);
    case 0x4826: return (uint8_t)m_divisor;
    case 0x4827: return (uint8_t)(m_divisor >> 8);
    case 0x4828: return (uint8_t)m_result;
    case 0x4829: return (uint8_t)(m_result >> 8);
    case 0x482A: return (uint8_t)(m_result >> 16);
    case 0x482B: return (uint8_t)(m_result >> 24);
    case 0x482C: return (uint8_t)m_remainder;
    case 0x482D: return (uint8_t)(m_remainder >> 8);
    case 0x482E: return m_aluFlags;
    case 0x482F: return m_aluState;
    // The map
    case 0x4830: return m_sramEnabled;
    case 0x4831: return m_banks[0];
    case 0x4832: return m_banks[1];
    case 0x4833: return m_banks[2];
    case 0x4834: return m_dataRomSizeReg;
    case 0x4840: case 0x4841: case 0x4842:
        return m_rtc && !peek ? rtcRead(a) : 0;
    default: return 0;
    }
}

void Spc7110::regWrite(uint16_t a, uint8_t v) {
    switch (a) {
    // Decompression
    case 0x4801: m_directoryBase = (m_directoryBase & 0xFFFF00) | v; break;
    case 0x4802: m_directoryBase = (m_directoryBase & 0xFF00FF) | (v << 8); break;
    case 0x4803: m_directoryBase = (m_directoryBase & 0x00FFFF) | (v << 16); break;
    case 0x4804: m_directoryIndex = v; loadEntryHeader(); break;
    case 0x4805: m_targetOffset = (uint16_t)((m_targetOffset & 0xFF00) | v); break;
    case 0x4806: m_targetOffset = (uint16_t)((m_targetOffset & 0x00FF) | (v << 8)); beginDecompression(); break;
    case 0x4807: m_skipBytes = v; break;
    case 0x4809: m_dataLengthCounter = (uint16_t)((m_dataLengthCounter & 0xFF00) | v); break;
    case 0x480A: m_dataLengthCounter = (uint16_t)((m_dataLengthCounter & 0x00FF) | (v << 8)); break;
    case 0x480B: m_decompFlags = v & 0x03; break;
    // The data port
    case 0x4811: m_readBase = (m_readBase & 0xFFFF00) | v; break;
    case 0x4812: m_readBase = (m_readBase & 0xFF00FF) | (v << 8); break;
    case 0x4813: m_readBase = (m_readBase & 0x00FFFF) | (v << 16); fillReadBuffer(); break;
    case 0x4814:
        m_readOffset = (uint16_t)((m_readOffset & 0xFF00) | v);
        if ((m_readMode & 0x60) == 0x20) incrementPosition();
        break;
    case 0x4815:
        m_readOffset = (uint16_t)((m_readOffset & 0x00FF) | (v << 8));
        if (m_readMode & 0x02) fillReadBuffer();
        if ((m_readMode & 0x60) == 0x40) incrementPosition();
        break;
    case 0x4816: m_readStep = (uint16_t)((m_readStep & 0xFF00) | v); break;
    case 0x4817: m_readStep = (uint16_t)((m_readStep & 0x00FF) | (v << 8)); break;
    case 0x4818: m_readMode = v & 0x7F; fillReadBuffer(); break;
    // The ALU
    case 0x4820: m_dividend = (m_dividend & 0xFFFFFF00) | v; break;
    case 0x4821: m_dividend = (m_dividend & 0xFFFF00FF) | ((uint32_t)v << 8); break;
    case 0x4822: m_dividend = (m_dividend & 0xFF00FFFF) | ((uint32_t)v << 16); break;
    case 0x4823: m_dividend = (m_dividend & 0x00FFFFFF) | ((uint32_t)v << 24); break;
    case 0x4824: m_multiplier = (uint16_t)((m_multiplier & 0xFF00) | v); break;
    case 0x4825:
        m_multiplier = (uint16_t)((m_multiplier & 0x00FF) | (v << 8));
        // 16 x 16 = 32; unsigned, Mesen multiplies the whole 32-bit dividend
        if (m_aluFlags & 0x01) m_result = (uint32_t)((int32_t)(int16_t)m_dividend * (int16_t)m_multiplier);
        else m_result = m_dividend * m_multiplier;
        m_aluState = (uint8_t)((m_aluState | 0x01) & 0x7F);
        break;
    case 0x4826: m_divisor = (uint16_t)((m_divisor & 0xFF00) | v); break;
    case 0x4827:
        m_divisor = (uint16_t)((m_divisor & 0x00FF) | (v << 8));
        // 32 / 16 = 32 remainder 16; dividing by zero gives 0 remainder the dividend's low word
        if (m_aluFlags & 0x01) {
            const int32_t dividend = (int32_t)m_dividend;
            const int16_t divisor = (int16_t)m_divisor;
            if (divisor != 0 && !(dividend == INT32_MIN && divisor == -1)) {
                m_result = (uint32_t)(dividend / divisor);
                m_remainder = (uint16_t)(dividend % divisor);
            } else if (divisor != 0) {
                m_result = (uint32_t)dividend;     // INT32_MIN / -1 overflows; the quotient wraps
                m_remainder = 0;
            } else {
                m_result = 0;
                m_remainder = (uint16_t)dividend;
            }
        } else if (m_divisor != 0) {
            m_result = m_dividend / m_divisor;
            m_remainder = (uint16_t)(m_dividend % m_divisor);
        } else {
            m_result = 0;
            m_remainder = (uint16_t)m_dividend;
        }
        m_aluState &= 0x7F;
        break;
    case 0x482E: m_aluFlags = v & 0x01; break;
    // The map
    case 0x4830: m_sramEnabled = v & 0x87; break;
    case 0x4831: m_banks[0] = v & 0x07; break;
    case 0x4832: m_banks[1] = v & 0x07; break;
    case 0x4833: m_banks[2] = v & 0x07; break;
    case 0x4834: m_dataRomSizeReg = v & 0x07; break;
    case 0x4840: case 0x4841: case 0x4842:
        if (m_rtc) rtcWrite(a, v);
        break;
    default: break;
    }
}

void Spc7110::fillReadBuffer() {
    int32_t offset = (m_readMode & 0x02) ? m_readOffset : 0;
    if (m_readMode & 0x08) offset = (int16_t)offset;
    m_readBuffer = dataRom(m_readBase + (uint32_t)offset);
}

void Spc7110::incrementPosition() {
    m_readBase += (m_readMode & 0x08) ? (uint32_t)(int32_t)(int16_t)m_readOffset : m_readOffset;
    fillReadBuffer();
}

void Spc7110::incrementPosition4810() {
    int32_t step = (m_readMode & 0x01) ? m_readStep : 1;
    if (m_readMode & 0x04) step = (int16_t)step;
    if (m_readMode & 0x10) m_readOffset = (uint16_t)(m_readOffset + step);
    else m_readBase = (m_readBase + (uint32_t)step) & 0xFFFFFF;
    fillReadBuffer();
}

void Spc7110::loadEntryHeader() {
    const uint32_t at = m_directoryBase + m_directoryIndex * 4u;
    m_decompMode = dataRom(at) & 0x03;
    m_srcAddress = (uint32_t)dataRom(at + 1) << 16 | (uint32_t)dataRom(at + 2) << 8 | dataRom(at + 3);
}

void Spc7110::beginDecompression() {
    if (m_decompMode == 3) return;
    m_decomp.init(this, m_decompMode, m_srcAddress);
    m_decomp.decode(this);
    uint32_t seek = (m_decompFlags & 0x02) ? m_targetOffset : 0;
    while (seek--) m_decomp.decode(this);
    m_decompStatus |= 0x80;
    m_decompOffset = 0;
}

uint8_t Spc7110::readDecompressedByte() {
    if (!(m_decompStatus & 0x80)) return 0;
    const uint32_t bpp = m_decomp.bpp;
    if (m_decompOffset == 0) {
        // Eight rows of a tile at a time, in SNES planar order
        for (int i = 0; i < 8; ++i) {
            const uint32_t r = m_decomp.result;
            switch (bpp) {
            case 1: m_decompBuffer[i] = (uint8_t)r; break;
            case 2: m_decompBuffer[i * 2] = (uint8_t)r; m_decompBuffer[i * 2 + 1] = (uint8_t)(r >> 8); break;
            case 4:
                m_decompBuffer[i * 2] = (uint8_t)r; m_decompBuffer[i * 2 + 1] = (uint8_t)(r >> 8);
                m_decompBuffer[i * 2 + 16] = (uint8_t)(r >> 16); m_decompBuffer[i * 2 + 17] = (uint8_t)(r >> 24);
                break;
            }
            uint32_t seek = (m_decompFlags & 0x01) ? m_skipBytes : 1;
            while (seek--) m_decomp.decode(this);
        }
    }
    const uint8_t v = m_decompBuffer[m_decompOffset++];
    m_decompOffset &= bpp * 8 - 1;
    return v;
}

// The RTC-4513, as far as Mesen goes: no 30-second adjust, 12-hour mode, calendar or wrap flags

std::vector<uint8_t> Spc7110::rtcState() const {
    std::vector<uint8_t> d(m_rtcRegs, m_rtcRegs + 16);
    for (int i = 7; i >= 0; --i) d.push_back((uint8_t)((uint64_t)m_rtcLastTime >> (i * 8)));
    return d;
}

void Spc7110::setRtcState(const std::vector<uint8_t>& d) {
    if (d.size() != 24) return;
    std::memcpy(m_rtcRegs, d.data(), 16);
    uint64_t t = 0;
    for (int i = 16; i < 24; ++i) t = t << 8 | d[i];
    m_rtcLastTime = (int64_t)t;
}

void Spc7110::rtcUpdate() {
    uint8_t* r = m_rtcRegs;
    if (r[0xF] & 0x01) { r[0] = 0; r[1] = 0; }               // reset: the seconds go to zero
    const int64_t now = clock();
    if (m_rtcLastTime == 0) m_rtcLastTime = now;         // first use without a saved clock
    const int64_t elapsed = now - m_rtcLastTime;
    if (elapsed <= 0) return;
    if ((r[0xF] & 0x03) || (r[0xD] & 0x01)) { m_rtcLastTime = now; return; }   // stopped, reset or held

    const int month = r[8] + (r[9] & 0x01) * 10;
    const int year = r[10] + r[11] * 10;
    if (month == 0) { m_rtcLastTime = now; return; }
    std::tm tm = {};
    tm.tm_sec = r[0] + (r[1] & 0x07) * 10;
    tm.tm_min = r[2] + (r[3] & 0x07) * 10;
    tm.tm_hour = r[4] + (r[5] & 0x03) * 10;
    tm.tm_mday = r[6] + (r[7] & 0x03) * 10;
    tm.tm_mon = month - 1;
    tm.tm_year = (year >= 90 ? 0 : 100) + year;
    // UTC on both sides, so daylight saving can't move the clock (Mesen uses local time)
    const std::time_t t = timegm(&tm);
    if (t == -1) { m_rtcLastTime = now; return; }
    // The day of the week can be set to anything; keep the game's offset from the real one
    const int dowGap = tm.tm_wday - (r[12] & 0x07);
    const std::time_t nt = t + (std::time_t)elapsed;
    std::tm n = {};
    gmtime_r(&nt, &n);
    r[0] = (uint8_t)(n.tm_sec % 10); r[1] = (uint8_t)(n.tm_sec / 10);
    r[2] = (uint8_t)(n.tm_min % 10); r[3] = (uint8_t)(n.tm_min / 10);
    r[4] = (uint8_t)(n.tm_hour % 10); r[5] = (uint8_t)(n.tm_hour / 10);
    r[6] = (uint8_t)(n.tm_mday % 10); r[7] = (uint8_t)(n.tm_mday / 10);
    r[8] = (uint8_t)((n.tm_mon + 1) % 10); r[9] = (uint8_t)((n.tm_mon + 1) / 10);
    int y = n.tm_year + 1900;
    y -= y >= 2000 ? 2000 : 1900;
    r[10] = (uint8_t)(y % 10); r[11] = (uint8_t)(y / 10);
    const int dow = n.tm_wday - dowGap;
    r[12] = (uint8_t)(dow < 0 ? dow + 7 : dow % 7);
    m_rtcLastTime = now;
}

uint8_t Spc7110::rtcRead(uint16_t a) {
    rtcUpdate();
    if (a == 0x4841 && m_rtcMode == 0x0C) {               // read mode
        const uint8_t i = (uint8_t)m_rtcIndex;
        m_rtcIndex = (int8_t)((m_rtcIndex + 1) & 0x0F);
        return m_rtcRegs[i & 0x0F];
    }
    if (a == 0x4842) return 0x80;                          // ready
    return 0;
}

void Spc7110::rtcWrite(uint16_t a, uint8_t v) {
    rtcUpdate();
    if (a == 0x4840) {
        m_rtcEnabled = v;
        if (!(v & 0x01)) {
            m_rtcMode = -1;
            m_rtcIndex = -1;
            m_rtcRegs[0x0F] &= 0x06;                       // reset and test go off
        }
    } else if (a == 0x4841) {
        if (m_rtcMode == -1) m_rtcMode = (int8_t)(v & 0x0F);
        else if (m_rtcIndex == -1) m_rtcIndex = (int8_t)(v & 0x0F);
        else if (m_rtcMode == 0x03) {                      // write mode
            const uint8_t i = (uint8_t)m_rtcIndex;
            m_rtcIndex = (int8_t)((m_rtcIndex + 1) & 0x0F);
            m_rtcRegs[i & 0x0F] = v & 0x0F;
        }
    }
}

}  // namespace snes
