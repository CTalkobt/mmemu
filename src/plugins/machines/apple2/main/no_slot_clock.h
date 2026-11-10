#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>

namespace apple2 {

/**
 * The No-Slot Clock (Dallas DS1216 SmartWatch), after AppleWin's
 * NoSlotClock.cpp: a socket under a ROM that watches its accesses.
 *
 * - A0 is a data bit and A2 says which way: an access with A2 clear writes
 *   A0 to the clock, one with A2 set reads a bit back in D0.
 * - 64 writes matching the pattern $5CA33AC55CA33AC5 (low bit first) open
 *   the clock register; a mismatch closes it until the next read with A2 set.
 * - The register then reads out 64 bits: BCD hundredths, seconds, minutes,
 *   hours, day of the week (1 = Sunday), date, month and year, low nibble
 *   first.
 * - While it's closed the ROM answers as usual.
 *
 * Under the IIe's internal $C3/$C8 ROM, or the II/II+'s $F8 ROM.
 */
class NoSlotClock {
public:
    static constexpr uint64_t INIT_SEQUENCE = 0x5CA33AC55CA33AC5ull;

    /** The time it reads (host local time unless set, e.g. by a test). */
    std::function<std::tm()> now = []() {
        const std::time_t t = std::time(nullptr);
        std::tm out{};
        localtime_r(&t, &out);
        return out;
    };
    /** Hundredths of a second (0-99). */
    std::function<int()> hundredths = []() {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
        return (int)((ms % 1000) / 10);
    };

    /** Power-on: the pattern starts over (the RESET key doesn't reach it). */
    void reset() {
        m_cmpMask = 1;
        m_open = false;
        m_writeOk = true;
    }

    /**
     * An access under the ROM. For a read, true means the clock answers:
     * `data`'s bit 0 is its bit (the rest 0, as AppleWin has it).
     */
    bool access(uint16_t addr, bool write, uint8_t& data) {
        if (addr & 0x04) {
            uint8_t bit = 0;
            const bool answered = readBit(bit);
            if (write || !answered) return false;
            data = bit;
            return true;
        }
        writeBit(addr & 1);
        return false;
    }

    bool open() const { return m_open; }

private:
    bool readBit(uint8_t& data) {
        if (!m_open) {
            m_cmpMask = 1;
            m_writeOk = true;
            return false;
        }
        data = (m_clock & m_clockMask) ? 1 : 0;
        if (next(m_clockMask)) m_open = false;
        return true;
    }

    void writeBit(int bit) {
        if (!m_writeOk) return;
        if (!m_open) {
            if (((INIT_SEQUENCE & m_cmpMask) != 0) == (bit != 0)) {
                if (next(m_cmpMask)) {
                    m_open = true;
                    fill();
                }
            } else {
                m_writeOk = false;
            }
        } else if (next(m_clockMask)) {
            m_open = false;
        }
    }

    static bool next(uint64_t& mask) {
        mask <<= 1;
        if (mask) return false;
        mask = 1;
        return true;
    }

    void fill() {
        const std::tm t = now();
        const int centis = hundredths();
        const int fields[] = {centis, t.tm_sec, t.tm_min, t.tm_hour, t.tm_wday + 1, t.tm_mday, t.tm_mon + 1, t.tm_year % 100};
        m_clock = 0;
        int bit = 0;
        for (int f : fields) {
            const int nibbles[] = {f % 10, f / 10};
            for (int n : nibbles)
                for (int i = 0; i < 4; ++i, ++bit)
                    if ((n >> i) & 1) m_clock |= 1ull << bit;
        }
        m_clockMask = 1;
    }

    uint64_t m_cmpMask = 1;
    uint64_t m_clock = 0, m_clockMask = 1;
    bool m_open = false, m_writeOk = true;
};

}  // namespace apple2
