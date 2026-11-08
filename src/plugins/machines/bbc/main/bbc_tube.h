#pragma once

#include "libmem/main/ibus.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace bbc {

/**
 * The Tube ULA, after b-em (tube.c, GPL-2.0-or-later): four FIFOs between
 * the BBC (the host, &FEE0-&FEE7) and a second processor (the parasite,
 * &FEF8-&FEFF on its side). Register 1 parasite-to-host holds 24 bytes;
 * register 3 one or two (flag V). The control register (host &FEE0 writes)
 * sets or clears flags Q (host IRQ from register 4), I and J (parasite IRQ
 * from registers 1 and 4), M (parasite NMI from register 3), V, P (holds
 * the parasite in reset) and T (clears the FIFOs).
 */
class TubeUla {
public:
    static constexpr uint8_t STAT_Q = 0x01, STAT_I = 0x02, STAT_J = 0x04, STAT_M = 0x08;
    static constexpr uint8_t STAT_V = 0x10, STAT_P = 0x20, STAT_T = 0x40, STAT_S = 0x80;
    static constexpr int PH1_SIZE = 24;

    std::function<void()> onParasiteReset;      // P cleared: the parasite restarts
    std::function<void()> onParasiteStatusRead; // the boot ROM leaves the memory map

    void reset();
    uint8_t hostRead(uint16_t addr);
    void hostWrite(uint16_t addr, uint8_t v);
    uint8_t parasiteRead(uint16_t addr);
    void parasiteWrite(uint16_t addr, uint8_t v);

    bool hostIrq() const { return m_hostIrq; }
    bool parasiteIrq() const { return m_parIrq & 1; }
    bool parasiteNmi() const { return m_parIrq & 2; }
    bool parasiteHeld() const { return m_r1stat & STAT_P; }
    uint8_t control() const { return m_r1stat; }

private:
    enum : uint8_t { SPACE = 0x40, DATA = 0x80, BOTH = 0xC0 };
    void resetMost();
    void update();

    uint8_t m_ph1[PH1_SIZE] = {}, m_ph2 = 0, m_ph3[2] = {}, m_ph4 = 0, m_phl = 0;
    uint8_t m_hp1 = 0, m_hp2 = 0, m_hp3[2] = {}, m_hp4 = 0, m_hpl = 0;
    uint8_t m_hstat[4] = {}, m_pstat[4] = {}, m_r1stat = 0;
    int m_ph1tail = 0, m_ph1head = 0, m_ph1count = 0, m_ph3pos = 1, m_hp3pos = 0;
    bool m_hostIrq = false;
    int m_parIrq = 0;
};

/**
 * The 6502 Second Processor's memory (b-em 6502tube.c): 64 KB of RAM, the
 * 2 KB boot ROM over &F000-&FFFF until the parasite first reads the Tube's
 * status (&FEF8), the Tube's parasite registers at &FEF8-&FEFF.
 */
class TubeParasiteBus : public IBus {
public:
    TubeParasiteBus(TubeUla* tube, std::vector<uint8_t> rom);

    void reset() override { m_romIn = true; }
    void powerOn();
    bool romIn() const { return m_romIn; }

    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "tube"; }
    uint8_t read8(uint32_t addr) override;
    uint8_t peek8(uint32_t addr) override;
    void write8(uint32_t addr, uint8_t v) override;
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

private:
    BusConfig m_cfg;
    TubeUla* m_tube;
    std::vector<uint8_t> m_rom, m_ram;
    bool m_romIn = true;
};

/**
 * The Z80 Second Processor's memory (b-em z80.c): 64 KB of RAM, the 4 KB
 * client ROM over $0000-$0FFF until the Z80 first runs code above $7FFF
 * (romOut(), from the scheduler).
 */
class Z80ParasiteBus : public IBus {
public:
    explicit Z80ParasiteBus(std::vector<uint8_t> rom);
    void reset() override { m_romIn = true; }
    void powerOn();
    void romOut() { m_romIn = false; }
    bool romIn() const { return m_romIn; }

    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "tube"; }
    uint8_t read8(uint32_t addr) override;
    uint8_t peek8(uint32_t addr) override;
    void write8(uint32_t addr, uint8_t v) override;
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

private:
    BusConfig m_cfg;
    std::vector<uint8_t> m_rom, m_ram;
    bool m_romIn = true;
};

/** The Z80's I/O space: the Tube's parasite registers on every port (A0-A2). */
class Z80TubeIo : public IBus {
public:
    explicit Z80TubeIo(TubeUla* tube) : m_tube(tube) { m_cfg = {16, 8, BusRole::IO_PORT, true, 0xFFFF}; }
    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "tube-io"; }
    uint8_t read8(uint32_t addr) override { return m_tube->parasiteRead((uint16_t)addr); }
    uint8_t peek8(uint32_t) override { return 0xFF; }
    void write8(uint32_t addr, uint8_t v) override { m_tube->parasiteWrite((uint16_t)addr, v); }
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

private:
    BusConfig m_cfg;
    TubeUla* m_tube;
};

}  // namespace bbc
