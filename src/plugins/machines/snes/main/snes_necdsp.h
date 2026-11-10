#pragma once

// The NEC uPD7725 in DSP-1/1B/2/3/4 cartridges (#15).
//
// After Mesen2 (Core/SNES/Coprocessors/DSP/NecDsp.cpp), GPL-3.0, Sour and the
// Mesen contributors. A 16-bit fixed-point DSP running its own mask-ROM
// program (2048 24-bit words) with a 1024-word data ROM and 256 words of RAM,
// at 7.6 MHz. The SNES sees two registers: DR (data, 8 or 16 bits through a
// byte latch) and SR (status; the high byte, with RQM in bit 7). The firmware
// is not part of the cartridge's ROM: it is loaded from a file (Mesen's
// names, roms/snes/dsp1b.rom...) or found at the end of the ROM image.
//
// The DSP runs lazily: it is brought up to the main CPU's time whenever its
// registers are read or written, and a firmware loop waiting for RQM sleeps
// until the next access.

#include <cstdint>
#include <string>
#include <vector>

namespace snes {

class NecDsp {
public:
    static constexpr uint32_t PROGRAM_SIZE = 0x1800;   // 2048 words of 3 bytes
    static constexpr uint32_t DATA_SIZE = 0x800;       // 1024 words of 2 bytes
    static constexpr double CLOCK_HZ = 7600000.0;

    struct Flags { bool c = false, z = false, ov0 = false, ov1 = false, s0 = false, s1 = false; };
    struct State {
        uint16_t a = 0, b = 0;
        Flags fa, fb;
        uint16_t tr = 0, trb = 0, pc = 0, rp = 0, dp = 0, dr = 0, sr = 0;
        uint16_t k = 0, l = 0, m = 0, n = 0;
        uint16_t so = 0, si = 0;      // serial ports: not connected
        uint8_t sp = 0;
    };

    // SR bits
    static constexpr uint16_t SR_RQM = 0x8000, SR_DRS = 0x1000, SR_DRC = 0x0400;

    /** The firmware: PROGRAM_SIZE bytes of program then DATA_SIZE bytes of data (little-endian words). */
    bool load(const std::vector<uint8_t>& firmware, std::string& err);
    void reset();

    /** Run to master clock `master` (21.477 MHz). */
    void runTo(uint64_t master, uint64_t masterHz);
    /** Run `n` instructions (tests). */
    void step(int n);

    /** The SNES side: `sr` selects SR (else DR). Reads of DR have side effects. */
    uint8_t read(bool sr);
    void write(bool sr, uint8_t value);

    const State& state() const { return m_s; }
    uint64_t cycles() const { return m_cycles; }

private:
    void exec(uint32_t op);
    void alu(uint32_t op, uint8_t fn, uint16_t source);
    void updateDp(uint32_t op);
    void jump(uint32_t op);
    void load(uint8_t dest, uint16_t value);
    uint16_t source(uint8_t src);
    uint16_t ram(uint16_t a) const { return m_ram[a & 0xFF]; }

    std::vector<uint32_t> m_prog;     // 2048 opcodes
    std::vector<uint16_t> m_data;     // 1024 words
    uint16_t m_ram[256] = {};
    uint16_t m_stack[4] = {};
    State m_s;
    uint64_t m_cycles = 0;
    bool m_rqmWait = false;
};

}  // namespace snes
