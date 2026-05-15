#include "crtc6545.h"
#include <cstring>

CRTC6545::CRTC6545() {
    CRTC6545::reset();
}

void CRTC6545::reset() {
    std::memset(m_regs, 0, sizeof(m_regs));
    m_addressReg = 0;
    m_statusReg = 0;
    m_hCount = 0;
    m_vCount = 0;
    m_ra = 0;
    m_ma = 0;
    m_maStart = 0;
    m_hSync = false;
    m_vSync = false;
    m_dispEnable = false;
}

bool CRTC6545::ioRead(IBus*, uint32_t addr, uint8_t* val) {
    if ((addr & ~0x0001u) != baseAddr()) return false;
    if ((addr & 1) == 0) {
        // Address register is usually write-only on 6845, but some 6545 allow reading status
        *val = m_statusReg;
    } else {
        // Data register
        if (m_addressReg >= 14 && m_addressReg <= 17) {
            *val = m_regs[m_addressReg];
        } else {
            *val = 0; // Other registers are usually write-only
        }
    }
    return true;
}

bool CRTC6545::ioWrite(IBus*, uint32_t addr, uint8_t val) {
    if ((addr & ~0x0001u) != baseAddr()) return false;
    if ((addr & 1) == 0) {
        m_addressReg = val & 0x1F;
    } else {
        if (m_addressReg < 18) {
            m_regs[m_addressReg] = val;

            // Recalculate start address if R12 or R13 written
            if (m_addressReg == 12 || m_addressReg == 13) {
                m_maStart = (m_regs[12] << 8) | m_regs[13];
            }
        }
    }
    return true;
}

void CRTC6545::tick(uint64_t cycles) {
    for (uint64_t i = 0; i < cycles; ++i) {
        updateCounters();
    }
}

void CRTC6545::updateCounters() {
    // R0: Horizontal Total (characters)
    // R1: Horizontal Displayed
    // R2: Horizontal Sync Position
    // R3: Sync Widths (Horiz/Vert)
    // R4: Vertical Total (char rows)
    // R5: Vertical Total Adjust (scanlines)
    // R6: Vertical Displayed (char rows)
    // R7: Vertical Sync Position (char rows)
    // R9: Max Scanline Address
    
    m_hCount++;
    if (m_hCount > m_regs[0]) {
        m_hCount = 0;
        
        m_ra++;
        if (m_ra > m_regs[9]) {
            m_ra = 0;
            
            m_vCount++;
            if (m_vCount > m_regs[4]) {
                m_vCount = 0;
                m_maStart = (m_regs[12] << 8) | m_regs[13];
            } else {
                // Move to next row start: maStart += R1 (displayed width)
                m_maStart += m_regs[1];
            }
        }
        m_ma = m_maStart;
    } else {
        m_ma++;
    }

    // Signals
    m_dispEnable = (m_hCount < m_regs[1]) && (m_vCount < m_regs[6]);
    m_hSync = (m_hCount >= m_regs[2]) && (m_hCount < (m_regs[2] + (m_regs[3] & 0x0F)));
    
    // Vertical sync: simplified, usually R7 is row pos
    m_vSync = (m_vCount == m_regs[7]);
}
