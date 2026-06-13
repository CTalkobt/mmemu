#include "mega65_io_stub.h"
#include <cstring>

Mega65IoStub::Mega65IoStub() {
    reset();
}

void Mega65IoStub::reset() {
    std::memset(m_regs, 0, sizeof(m_regs));
    std::memset(m_colorRam, 0, sizeof(m_colorRam));

    // $D600: UART/VDC status — bit 7 = READY (data available / transmit ready)
    m_regs[0x00] = 0x80;

    // $D630-$D631: FPGA date stamp (days since 2020-01-01) — pick a reasonable value
    m_regs[0x30] = 0x00;
    m_regs[0x31] = 0x08; // ~2048 days ≈ 2025

    // $D632-$D635: FPGA firmware ID — "MMSM" (our emulator tag)
    m_regs[0x32] = 'M';
    m_regs[0x33] = 'M';
    m_regs[0x34] = 'S';
    m_regs[0x35] = 'M';

    // $D629: Model ID — MEGA65 R3 = $03
    m_regs[0x29] = 0x03;

    // $D67F: Hypervisor status — 'U' (user mode) or 'H' (hypervisor)
    // This is handled by HypervisorRegs, but as fallback return 'U'
    m_regs[0x7F] = 'U';
}

bool Mega65IoStub::ioRead(IBus* /*bus*/, uint32_t addr, uint8_t* val) {
    // F011 FDC registers $D080-$D09F (stub — no real floppy emulation)
    if (addr >= 0xD080 && addr <= 0xD09F) {
        uint8_t off = addr & 0x1F;
        switch (off) {
            case 0x02: *val = 0x00; return true; // Status A: not busy, no errors
            case 0x03: *val = 0x08; return true; // Status B: disk inserted
            default:   *val = 0x00; return true;
        }
    }

    // Colour RAM $D800-$DBFF
    if (addr >= 0xD800 && addr <= 0xDBFF) {
        *val = m_colorRam[addr - 0xD800];
        return true;
    }

    // $D600-$D6FF system registers
    if (addr >= 0xD600 && addr <= 0xD6FF) {
        // Skip ranges handled by dedicated handlers:
        // $D640-$D67F: HypervisorRegs
        // $D680-$D6FF: SdCardDevice
        if (addr >= 0xD640 && addr <= 0xD67F) return false;
        if (addr >= 0xD680 && addr <= 0xD693) return false; // SdCardDevice

        uint8_t off = addr & 0xFF;
        *val = m_regs[off];
        return true;
    }

    return false;
}

bool Mega65IoStub::ioWrite(IBus* /*bus*/, uint32_t addr, uint8_t val) {
    // Colour RAM $D800-$DBFF
    if (addr >= 0xD800 && addr <= 0xDBFF) {
        m_colorRam[addr - 0xD800] = val;
        return true;
    }

    // $D600-$D6FF system registers
    if (addr >= 0xD600 && addr <= 0xD6FF) {
        // Skip ranges handled by dedicated handlers
        if (addr >= 0xD640 && addr <= 0xD67F) return false;
        if (addr >= 0xD680 && addr <= 0xD693) return false; // SdCardDevice

        uint8_t off = addr & 0xFF;

        // $D610: keyboard buffer — write clears (returns 0 on next read)
        if (addr == 0xD610) { m_regs[off] = 0x00; return true; }

        m_regs[off] = val;
        return true;
    }

    return false;
}
