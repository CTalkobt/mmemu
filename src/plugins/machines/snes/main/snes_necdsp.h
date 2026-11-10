#pragma once

// The NEC uPD7725 in DSP-1/1B/2/3/4 cartridges, and the uPD96050 in the
// ST010 and ST011 (#15).
//
// After Mesen2 (Core/SNES/Coprocessors/DSP/NecDsp.cpp), GPL-3.0, Sour and the
// Mesen contributors. A 16-bit fixed-point DSP running its own mask-ROM
// program (2048 24-bit words) with a 1024-word data ROM and 256 words of RAM,
// at 7.6 MHz. The SNES sees two registers: DR (data, 8 or 16 bits through a
// byte latch) and SR (status; the high byte, with RQM in bit 7). The firmware
// is not part of the cartridge's ROM: it is loaded from a file (Mesen's
// names, roms/snes/dsp1b.rom...) or found at the end of the ROM image.
//
// The uPD96050 is the same machine, larger: 16384 program words, 2048 data
// ROM words, 2048 words of RAM (battery-backed, and also mapped for the
// SNES), an 8-level stack; at 11 MHz in the ST010, 22 MHz in the ST011.
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
    // The uPD7725
    static constexpr uint32_t PROGRAM_SIZE = 0x1800;   // 2048 words of 3 bytes
    static constexpr uint32_t DATA_SIZE = 0x800;       // 1024 words of 2 bytes
    static constexpr double CLOCK_HZ = 7600000.0;
    // The uPD96050
    static constexpr uint32_t PROGRAM_SIZE_96050 = 0xC000;   // 16384 words
    static constexpr uint32_t DATA_SIZE_96050 = 0x1000;      // 2048 words

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

    NecDsp() { setModel(false, CLOCK_HZ); }
    /** Choose the uPD96050 (true) or the uPD7725, and its clock; before load(). */
    void setModel(bool upd96050, double hz);
    bool isUpd96050() const { return m_96050; }
    uint32_t programSize() const { return m_96050 ? PROGRAM_SIZE_96050 : PROGRAM_SIZE; }
    uint32_t dataSize() const { return m_96050 ? DATA_SIZE_96050 : DATA_SIZE; }

    /** The firmware: programSize() bytes of program then dataSize() bytes of data (little-endian words). */
    bool load(const std::vector<uint8_t>& firmware, std::string& err);
    void reset();

    /** Run to master clock `master` (21.477 MHz). */
    void runTo(uint64_t master, uint64_t masterHz);
    /** Run `n` instructions (tests). */
    void step(int n);

    /** The SNES side: `sr` selects SR (else DR). Reads of DR have side effects. */
    uint8_t read(bool sr);
    void write(bool sr, uint8_t value);

    /** The data RAM (256 or 2048 words). */
    std::vector<uint16_t>& dataRam() { return m_ram; }

    const State& state() const { return m_s; }
    uint64_t cycles() const { return m_cycles; }

private:
    void exec(uint32_t op);
    void alu(uint32_t op, uint8_t fn, uint16_t source);
    void updateDp(uint32_t op);
    void jump(uint32_t op);
    void load(uint8_t dest, uint16_t value);
    uint16_t source(uint8_t src);
    uint16_t ram(uint16_t a) const { return m_ram[a & m_ramMask]; }

    bool m_96050 = false;
    double m_hz = CLOCK_HZ;
    uint16_t m_progMask = 0x7FF, m_dataMask = 0x3FF, m_ramMask = 0xFF;
    uint8_t m_stackMask = 3;
    std::vector<uint32_t> m_prog;     // 2048 or 16384 opcodes
    std::vector<uint16_t> m_data;     // 1024 or 2048 words
    std::vector<uint16_t> m_ram = std::vector<uint16_t>(256);
    uint16_t m_stack[8] = {};
    State m_s;
    uint64_t m_cycles = 0;
    bool m_rqmWait = false;
};

}  // namespace snes
