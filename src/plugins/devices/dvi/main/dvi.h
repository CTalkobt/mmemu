#pragma once

#include "libmem/main/ibus.h"
#include "plugins/devices/wd17xx/main/wd17xx.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Z80;

/**
 * The Tandy Disk/Video Interface (26-3806), from its service manual (the
 * findings are written up in refs/dvi/FINDINGS.md): a box on the Model
 * 100/102/200's system bus with its own Z80, one or two 5.25" drives and a
 * 40/80-column composite video output. mmsim's own code.
 *
 * The box (4 MHz uPD780C):
 *  - Memory: 0000-07FF the 1 KB boot ROM (M40; the 1 KB repeats) in bank 0
 *    or RAM1 (2 KB) in bank 1; 0800-0FFF RAM2 (2 KB); C000-DFFF the
 *    attribute RAM (2 KB, 2 bits: reads give bits 7-2 set); E000-FFFF the
 *    character RAM (2 KB). The rest reads $FF.
 *  - I/O (A7 = 0; A6-A5 pick the group, anything else mirrors):
 *    00-1F the HD46505 (R/W = A0, RS = A1: OUT 00 selects a register,
 *    OUT 02 writes it, IN 03 reads it); 20-3F the STS port (read: bits 0-3
 *    the 8255's port B, 4 not driven, 5 VSRET, 6 IBF, 7 MOTOR ON = the FDC's
 *    HLD; write: 0 80 columns, 1 bank 1, 4 drive 0, 5 drive 1, 6 HLDEN, 7
 *    the cable's pin 32); 40-5F the WD1793 (A1 A0 the register; 40-4F wait
 *    until DRQ or INTRQ before the access, 50-5F don't); 60-7F the 8255's
 *    handshake (IN = /ACK: port A's output latch; OUT = /STB: into its input
 *    latch).
 *  - /INT = the 8255's /OBF (a byte from the portable is waiting); /NMI =
 *    the FDC's INTRQ; HLDEN holds the Z80 (BUSRQ) while the display is
 *    drawn, so it runs only in vertical retrace.
 *  - The FDC: double density always (DDEN grounded), READY and HLT tied
 *    high, the drives' motors on HLD. Disks: 40 tracks x 18 sectors x 256
 *    bytes, one side (a raw 184320-byte image is read as JVC).
 *  - The CRTC's frame: R0-R9 as programmed; a character is 1 us in 40
 *    columns, 0.5 us in 80; VRAM is MA0-MA10; the character ROM (M17)
 *    takes the code x 8 + RA0-RA2 (A11 = 0: its first 2 KB), D7 is the
 *    left dot; RA3 = 1 rows are blank.
 *
 * The portable's side: the 8255 at its I/O ports 80H-83H (the system bus's
 * Y0, so mirrored through 8FH): 80 port A, 81 port B, 82 port C, 83 the
 * control word. The portable sets mode 2 on group A ($C1); port C reads
 * /OBF (7), INTE1 (6), IBF (5), INTE2 (4), INTRA (3) and PC0-PC2 = the
 * box's reset line (0 while it runs: how the ROM tells a DVI is there).
 */
class Dvi {
public:
    static constexpr double Z80_HZ = 4000000.0;
    static constexpr int OUT_W = 640;          ///< 80 x 8 dots (40 columns: 16 a character)
    static constexpr int OUT_H = 400;          ///< 25 x 8 lines, each shown twice

    /** `rom`: the boot ROM (1-4 KB, the first 1 KB is used); `cg`: the
     *  character ROM (2-4 KB, the first 2 KB is used). */
    Dvi(std::vector<uint8_t> rom, std::vector<uint8_t> cg);
    ~Dvi();

    /** The portable's clock (advance()'s ticks). */
    void setHostClock(double hz) { m_hostHz = hz; }
    /** Power on (the box's reset). */
    void powerOn();
    /** Time passes on the portable (`ticks` of setHostClock()). */
    void advance(uint64_t ticks);

    // The portable's side (ports 80H-8FH; `port & 3`)
    uint8_t hostIn(uint8_t port);
    uint8_t hostPeek(uint8_t port) const;
    void hostOut(uint8_t port, uint8_t v);

    // Drives 0 and 1
    bool mountDisk(int drive, const std::string& path, std::string* err = nullptr);
    void ejectDisk(int drive);
    std::string diskPath(int drive) const;
    void flush();
    Wd17xx& fdc() { return m_fdc; }

    /** The video: OUT_W x OUT_H, 0xAARRGGBB, white on black. */
    void render(uint32_t* out) const;
    /** Character row `y` (0-24) as text, for tests: codes 20-7E as ASCII, others '.'. */
    std::string rowText(int y) const;
    bool cols80() const { return m_sts & 0x01; }
    uint8_t crtcReg(int r) const { return m_crtc[r & 15]; }
    uint8_t cram(uint16_t a) const { return m_cram[a & 0x7FF]; }
    uint8_t aram(uint16_t a) const { return m_aram[a & 0x7FF]; }
    Z80* cpu() const { return m_cpu.get(); }
    /** The box's own bus cycles, as its Z80 makes them (tests, the debugger). */
    uint8_t busRead(uint16_t a) const { return memRead(a); }
    void busWrite(uint16_t a, uint8_t v) { memWrite(a, v); }
    uint8_t portIn(uint8_t p) { return ioRead(p); }
    void portOut(uint8_t p, uint8_t v) { ioWrite(p, v); }
    /** The 8255's /OBF to the Z80's /INT: a byte from the portable waits. */
    bool intPending() const { return m_obf; }
    bool bank1() const { return m_sts & 0x02; }

    std::vector<uint8_t> saveState() const;
    bool loadState(const std::vector<uint8_t>& s);

private:
    class MemBus;
    class IoBus;
    friend class MemBus;
    friend class IoBus;

    uint8_t memRead(uint16_t a) const;
    void memWrite(uint16_t a, uint8_t v);
    uint8_t ioRead(uint16_t port);
    void ioWrite(uint16_t port, uint8_t v);
    void fdcWait();
    void tickFdc(uint64_t cycles);
    void stsWrite(uint8_t v);
    void updateInt();

    // Video timing (in Z80 cycles)
    uint64_t charCycles() const { return cols80() ? 2 : 4; }
    uint64_t lineCycles() const { return (uint64_t)(m_crtc[0] + 1) * charCycles(); }
    int frameLines() const;
    int displayLines() const;
    bool vsret() const;
    uint64_t cyclesToRetrace() const;

    std::vector<uint8_t> m_rom, m_cg;
    std::vector<uint8_t> m_ram1, m_ram2, m_aram, m_cram;
    std::unique_ptr<MemBus> m_mem;
    std::unique_ptr<IoBus> m_io;
    std::unique_ptr<Z80> m_cpu;
    Wd17xx m_fdc;

    double m_hostHz = 2457600.0;
    double m_frac = 0;
    uint64_t m_time = 0;            // Z80 cycles since power-on
    uint64_t m_fdcLead = 0;         // FDC cycles already run inside the current instruction
    uint64_t m_frameStart = 0;      // cycle the current video frame began

    uint8_t m_sts = 0;              // the STS write latch
    uint8_t m_crtcAddr = 0;
    uint8_t m_crtc[16] = {};

    // The 8255 (the portable is its CPU side)
    uint8_t m_ctrl = 0x9B;          // reset: mode 0, all ports input
    uint8_t m_outA = 0, m_inA = 0, m_portB = 0;
    bool m_obf = false;             // a byte from the portable waits (/OBF low)
    bool m_ibf = false;             // a byte for the portable waits
    bool m_inte1 = false, m_inte2 = false;
};
