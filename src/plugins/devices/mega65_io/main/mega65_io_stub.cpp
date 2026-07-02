#include "mega65_io_stub.h"
#include <cstring>

Mega65IoStub::Mega65IoStub() {
    reset();
}

void Mega65IoStub::reset() {
    std::memset(m_regs, 0, sizeof(m_regs));
    std::memset(m_colorRam, 0, sizeof(m_colorRam));
    m_keyQueue.clear();
    m_currentMods = 0;

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

    // $D6C5: FPGA reconfiguration status — bit 0 = FPGA has been configured
    // HYPPO's dont_launch_flash_menu checks this to branch to
    // fpga_has_been_reconfigured path (normalboot → go64)
    m_regs[0xC5] = 0x01;

    // $D67F: Hypervisor status — 'U' (user mode) or 'H' (hypervisor)
    // This is handled by HypervisorRegs, but as fallback return 'U'
    m_regs[0x7F] = 'U';
}

void Mega65IoStub::pushKey(uint8_t ascii, uint8_t petscii, uint8_t mods) {
    m_keyQueue.push_back({ascii, petscii, mods});
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

    // Colour RAM window ($D800-$DFFF)
    if (addr >= 0xD800 && addr <= 0xDFFF) {
        bool cram2k = m_cram2kQuery ? m_cram2kQuery() : false;
        if (addr <= 0xDBFF || cram2k) {
            *val = m_colorRam[addr - 0xD800];
            return true;
        }
    }

    // $D600-$D6FF system registers
    if (addr >= 0xD600 && addr <= 0xD6FF) {
        // Skip ranges handled by dedicated handlers:
        // $D640-$D67F: HypervisorRegs
        // $D680-$D693: SdCardDevice
        if (addr >= 0xD640 && addr <= 0xD67F) return false;
        if (addr >= 0xD680 && addr <= 0xD693) return false;

        // --- Keyboard buffer registers ---

        // $D60A: bit 7 = queue non-empty, bits 6-0 = modifier state at event
        if (addr == 0xD60A) {
            if (!m_keyQueue.empty()) {
                *val = 0x80 | (m_keyQueue.front().mods & 0x7F);
            } else {
                *val = 0x00;
            }
            return true;
        }

        // $D610: ASCII key at top of queue (0 if empty)
        if (addr == 0xD610) {
            *val = m_keyQueue.empty() ? 0x00 : m_keyQueue.front().ascii;
            return true;
        }

        // $D611: Current modifier key state (immediate, not buffered)
        if (addr == 0xD611) {
            *val = m_currentMods;
            return true;
        }

        // $D619: PETSCII key at top of queue (0 if empty)
        if (addr == 0xD619) {
            *val = m_keyQueue.empty() ? 0x00 : m_keyQueue.front().petscii;
            return true;
        }

        uint8_t off = addr & 0xFF;
        *val = m_regs[off];
        return true;
    }

    return false;
}

bool Mega65IoStub::ioWrite(IBus* /*bus*/, uint32_t addr, uint8_t val) {
    // Colour RAM window ($D800-$DFFF)
    if (addr >= 0xD800 && addr <= 0xDFFF) {
        bool cram2k = m_cram2kQuery ? m_cram2kQuery() : false;
        if (addr <= 0xDBFF || cram2k) {
            m_colorRam[addr - 0xD800] = val;
            return true;
        }
    }

    // $D600-$D6FF system registers
    if (addr >= 0xD600 && addr <= 0xD6FF) {
        // Skip ranges handled by dedicated handlers
        if (addr >= 0xD640 && addr <= 0xD67F) return false;
        if (addr >= 0xD680 && addr <= 0xD693) return false;

        // $D60A: writing 0 to bit 7 flushes the keyboard queue
        if (addr == 0xD60A) {
            if (!(val & 0x80)) flushKeyQueue();
            return true;
        }

        // $D610: write clears top of queue (advance to next event)
        if (addr == 0xD610) {
            if (!m_keyQueue.empty()) m_keyQueue.erase(m_keyQueue.begin());
            return true;
        }

        // $D619: write also clears top of queue (same event)
        if (addr == 0xD619) {
            if (!m_keyQueue.empty()) m_keyQueue.erase(m_keyQueue.begin());
            return true;
        }

        uint8_t off = addr & 0xFF;
        m_regs[off] = val;
        return true;
    }

    return false;
}
