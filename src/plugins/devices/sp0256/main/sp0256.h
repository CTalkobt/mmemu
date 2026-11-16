#pragma once

#include <cstdint>
#include <functional>
#include <vector>

/**
 * The General Instrument SP0256 Narrator speech processor (the SP0256-AL2
 * allophone set): a microsequencer that interprets the bit-packed program
 * in its ROM and a 12-pole (six 2nd-order stages) LPC filter driven by a
 * pulse or noise source, one sample every 312 clocks (10 kHz from its
 * 3.12 MHz crystal).
 *
 * After MAME's sp0256 (Joseph Zbiciak, Tim Lindner; BSD-3-Clause; from
 * Zbiciak's jzIntv): the microsequencer's opcodes (RTS/SETPAGE, JMP, JSR,
 * SETMODE, the LOAD/SETMSB/DELTA data blocks, PAUSE), the data-format
 * tables, the coefficient quantisation table and the filter. The SPB640
 * FIFO is not modelled.
 *
 * The ROM is the chip's (2 KB for the AL2, mapped at $1000; the
 * allophone n's entry is at $1000 + 2n); it is copyrighted, so it is the
 * user's file. Without it every entry is a halt: the allophone is silent
 * and SBY rises again within a sample (LRQ / SBY still answer).
 *
 * Pins: ALD (load an address: the allophone, 0-63 on the AL2; dropped
 * while LRQ is low), LRQ (high: the load buffer is free), SBY (high:
 * standing by, nothing to say).
 */
class Sp0256 {
public:
    static constexpr int CLOCK_DIVIDER = 312;      ///< clocks per sample
    static constexpr double CLOCK_HZ = 3120000.0;  ///< the AL2's usual crystal

    Sp0256();
    /** The ROM image, mapped from $1000 (empty: no ROM, every entry a halt). */
    void setRom(const std::vector<uint8_t>& rom);
    bool hasRom() const { return m_hasRom; }

    void reset();
    /** ALD: the address byte (an allophone). Ignored while LRQ is low. */
    void ald(uint8_t data);
    bool lrq() const { return m_lrq; }
    bool sby() const { return m_sby; }

    /** The next sample (14-bit range, scaled to +-32764). */
    int16_t sample();
    /** The last sample made. */
    int16_t output() const { return m_out; }

    /** LRQ and SBY changing (the SSC's TMS7040 takes LRQ on INT1). */
    std::function<void(bool)> onLrq;
    std::function<void(bool)> onSby;

    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    struct Lpc12 {
        int rpt = -1, cnt = 0;
        uint32_t per = 0, rng = 1;
        int amp = 0;
        int16_t fCoef[6] = {}, bCoef[6] = {};
        int16_t z[6][2] = {};
        uint8_t r[16] = {};
        int interp = 0;
        /** Up to one sample into `out`; 0 if the repeat count ran out first. */
        int update(int16_t& out);
        void regdec();
    };

    uint32_t getb(int len);
    void micro();
    void setLrq(bool v);
    void setSby(bool v);

    std::vector<uint8_t> m_rom;     // 64 KB
    bool m_hasRom = false;
    Lpc12 m_filt;
    bool m_silent = true;
    bool m_lrq = true, m_sby = true;
    bool m_halted = true;
    uint32_t m_ald = 0, m_pc = 0, m_stack = 0, m_page = 0x1000 << 3;
    int m_mode = 0;
    int16_t m_out = 0;
};
