#pragma once

#include "libdevices/main/iba_source.h"

#include "libdevices/main/io_handler.h"
#include "libdevices/main/isignal_line.h"
#include <cstdint>
#include <string>
#include <vector>


/**
 * Commodore RAM Expansion Unit (1700, 1764, 1750, and the larger "1750 XL"
 * style units up to 16 MB): the 8726 REC at I/O2 ($DF00-$DF0A, mirrored
 * every 32 bytes to $DFFF) and its DRAM. After VICE's src/c64/cart/reu.c
 * (GPL-2.0-or-later, the VICE team).
 *
 * | Reg | |
 * |-----|--|
 * | 0 | status (read clears bits 5-7 and the IRQ): IRQ, end of block, verify error, 256K chips, version 0 |
 * | 1 | command: bit 7 execute, 5 autoload, 4 no $FF00 trigger, 0-1 the type (C64 to REU, REU to C64, swap, verify) |
 * | 2-3 | the C64 address |
 * | 4-6 | the REU address and bank (bits 3-7 of the bank read 1) |
 * | 7-8 | the length (0 is 64 KB) |
 * | 9 | IRQ mask: bit 7 on, 6 end of block, 5 verify error |
 * | 10 | address control: bit 7 fix the C64 address, 6 fix the REU address |
 *
 * - A command with execute and bit 4 set starts at once; with bit 4 clear it
 *   waits for a write to $FF00 (any write to that address, whatever is mapped).
 * - The DMA takes the bus: the machine's scheduler calls busMasterCycle()
 *   before each CPU step, one byte a cycle (a swap takes two), through the
 *   CPU's memory map (I/O and ROM included; not the 6510's port at $00/$01).
 * - Sizes (VICE): the REC's addresses wrap at 512 KB (128 KB on the 1700);
 *   a 1764's 256 KB leave the upper half unbacked (reads float, writes are
 *   lost). Above 512 KB the bank's upper bits are a latch straight to the
 *   DRAM, so the units wrap at their size.
 * - The RAM survives a reset (as a RAM disk does), not a new install.
 */
class Reu : public IOHandler {
public:
    explicit Reu(uint32_t sizeBytes = 512 * 1024);

    /** 128K, 256K, 512K, 1M, 2M, 4M, 8M or 16M; false otherwise. Clears the RAM. */
    bool setSize(uint32_t bytes);
    uint32_t size() const { return (uint32_t)m_ram.size(); }
    /** The RAM as a VICE .reu image (raw); load one (its size picks the unit's). */
    bool loadImage(const std::string& path, std::string& err);
    bool saveImage(const std::string& path, std::string& err) const;
    std::vector<uint8_t>& ram() { return m_ram; }

    /** A write to $FF00 (the C128's MMU register snoops it too). */
    void ff00Write();
    bool dmaActive() const { return m_active; }

    // IOHandler
    void setName(const std::string& n) override { m_name = n; }
    const char* name() const override { return m_name.c_str(); }
    uint32_t baseAddr() const override { return 0xDF00; }
    uint32_t addrMask() const override { return 0xFF; }
    void setIrqLine(ISignalLine* l) override { m_irq = l; }
    void setDmaBus(IBus* b) override { m_bus = b; }
    bool ioRead(IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioPeek(IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override;
    void reset() override;
    void tick(uint64_t) override {}
    int busMasterCycle() override;
    void setBaSource(IBaSource* ba) override { m_ba = ba; }
    /** The C64's RAM, for a C64C's DMA read of I/O that returns the RAM under it. */
    void setRamData(const uint8_t* ram) override { m_ram64 = ram; }
    void setBusMasterSync(std::function<void(int)> sync) override { m_sync = std::move(sync); }
    bool setOption(const std::string& key, const std::string& value, std::string& err) override;
    bool saveDeviceImage(const std::string& path, std::string& err) override { return saveImage(path, err); }
    void getDeviceInfo(DeviceInfo& out) const override;

    /** The registers without side effects (tests, the debugger). */
    uint8_t reg(int r) const;

private:
    enum : uint8_t {
        ST_IRQ = 0x80, ST_EOB = 0x40, ST_VERIFY = 0x20, ST_256K = 0x10,
        CMD_EXEC = 0x80, CMD_AUTOLOAD = 0x20, CMD_NO_FF00 = 0x10,
        INT_ON = 0x80, INT_EOB = 0x40, INT_VERIFY = 0x20, INT_UNUSED = 0x1F,
        AC_FIX_C64 = 0x80, AC_FIX_REU = 0x40, AC_UNUSED = 0x3F,
    };

    void start();
    void finish(uint16_t host, uint32_t reuAddr, int len, uint8_t status);
    void setIrq(bool on) { if (m_irq) m_irq->set(on); }
    uint32_t step(uint32_t a) const;
    uint8_t ramRead(uint32_t a) const;
    void ramWrite(uint32_t a, uint8_t v);

    std::string m_name = "REU";
    std::vector<uint8_t> m_ram;
    IBus* m_bus = nullptr;
    IBaSource* m_ba = nullptr;          // the VIC-II: DMA waits while BA is low
    int m_baWrites = 0;                 // writes let through since BA fell
    uint8_t baHistory() const;          // BA for the REU, bit n = n cycles ago
    // A C64C ("glue" = "c64c", Denise isC64C): a read made while BA is low
    // returns the RAM under I/O
    bool m_c64cGlue = false;
    const uint8_t* m_ram64 = nullptr;
    uint8_t hostRead(bool baLow);
    // Swap: a read cycle and a write cycle in turn (Denise Reu::swap)
    bool m_swapRead = false, m_firstByte = true;
    uint8_t m_swapReu = 0, m_swapC64 = 0;
    std::function<void(int)> m_sync;    // the machine up to an access's cycle
    void syncTo(int cycle) { if (m_sync) m_sync(cycle); }
    ISignalLine* m_irq = nullptr;

    // How the size wires the REC to the DRAM (VICE rec_options)
    uint32_t m_wrap = 0x80000, m_dramWrap = 0x80000;
    uint8_t m_bankUnused = 0xF8, m_statusPreset = ST_256K;

    // The REC's registers and their shadows (autoload)
    uint8_t m_status = 0, m_command = CMD_NO_FF00, m_bank = 0, m_bankShadow = 0;
    uint8_t m_intMask = INT_UNUSED, m_addrCtl = AC_UNUSED;
    uint16_t m_host = 0, m_hostShadow = 0, m_reuAddr = 0, m_reuShadow = 0;
    uint16_t m_len = 0xFFFF, m_lenShadow = 0xFFFF;
    bool m_armed = false;                   // waiting for $FF00

    // The transfer under way
    bool m_active = false;
    int m_type = 0, m_left = 0, m_hostStep = 1, m_reuStep = 1;
    uint16_t m_dHost = 0;
    uint32_t m_dReu = 0;
    uint8_t m_floating = 0xFF;
};
