// The NEC PC-8201A's differences from the Model 100: its banked memory,
// the ports at $80-$AF and the cassette modulator, after MAME
// (kyocera.cpp pc8201_state, Curt Coder; BSD-3-Clause) and VirtualT (io.c,
// memory.c; BSD). mmsim's own code.

#include "m100_system.h"
#include <algorithm>
#include <cstring>

namespace m100 {

namespace {
constexpr uint64_t CMT_DIVIDER = 128;      // 4.9152 MHz / 2 / 128: the 19200 Hz modulator clock
}

uint8_t System::accessPc8201(uint16_t a, bool write, uint8_t v) {
    // RAM banks 2 and 3: 32 KB each past the internal 32 KB (a 64 or 96 KB machine)
    auto banked = [&](int bank, uint16_t off) -> uint8_t* {
        const size_t at = (size_t)(bank - 1) * 0x8000 + off;
        return m_ram.size() > 0x8000 && at < m_ram.size() ? &m_ram[at] : nullptr;
    };
    uint8_t* r = nullptr;
    if (a < 0x8000) {
        switch (m_bankReg & 3) {
        case 0: return write ? 0 : m_rom[a];
        case 1:
            if (write) return 0;
            return m_opt.empty() ? 0xFF : m_opt[a];      // the option ROM (an empty socket reads $FF)
        default: r = banked(m_bankReg & 3, a); break;
        }
    } else {
        const int bank = (m_bankReg >> 2) & 3;
        if (bank == 0) {
            // The internal RAM, ending at $FFFF (16 KB: from $C000)
            const size_t internal = std::min<size_t>(m_ram.size(), 0x8000);
            const size_t base = 0x10000 - internal;
            if (a >= base) r = &m_ram[a - base];
        } else if (bank > 1) {
            r = banked(bank, (uint16_t)(a - 0x8000));
        }
    }
    if (!r) return 0xFF;
    if (write) *r = v;
    return *r;
}

bool System::inPc8201(uint8_t port, uint8_t& v) {
    switch (port & 0xF0) {
    case 0x80:          // the ROM cassette ($8C: its data); none is fitted
    case 0x90:          // write only
        v = 0xFF;
        return true;
    case 0xA0:          // the bank register, the serial select from $90
        v = (uint8_t)((m_scp & 0xC0) | (m_bankReg & 0x0F));
        return true;
    default:
        return false;
    }
}

bool System::outPc8201(uint8_t port, uint8_t v) {
    switch (port & 0xF0) {
    case 0x80:
        switch (port & 0x0C) {
        case 0x00:      // ROMAH: 0 A16, 1 ROM select
            m_casRomAddr = (uint32_t)((v & 1) << 16) | (m_casRomAddr & 0xFFFF);
            m_casRomSel = v & 2;
            break;
        case 0x04: m_casRomAddr = (m_casRomAddr & 0x1FF00) | v; break;                   // ROMAL: A0-A7
        case 0x08: m_casRomAddr = (m_casRomAddr & 0x100FF) | ((uint32_t)v << 8); break;  // ROMAM: A8-A15
        default: break;
        }
        return true;
    case 0x90: {
        // 3 REMOTE (the cassette motor), 4 TSTB (the RTC's strobe), 5 PSTB (the
        // printer's: a byte goes as it rises, VirtualT), 6-7 the serial select
        const uint8_t old = m_scp;
        m_scp = v;
        tape.setMotor(v & 0x08);
        rtc.setStb(v & 0x10);
        if ((v & 0x20) && !(old & 0x20)) printer.write(m_pa);
        return true;
    }
    case 0xA0:
        m_bankReg = v & 0x0F;
        return true;
    case 0xE0:          // the keyboard is read only here
        return true;
    default:
        return false;
    }
}

void System::cmtClock(uint64_t ticks) {
    // MAME cmt_tick: the clock counts 9600 Hz half periods; the output
    // toggles every eight (1200 Hz), and SOD high adds a rise half way
    // (2400 Hz). SOD is sampled on the even counts.
    m_cmtAcc += ticks;
    while (m_cmtAcc >= CMT_DIVIDER) {
        m_cmtAcc -= CMT_DIVIDER;
        bool out = m_cmtOut;
        ++m_cmtDiv;
        if (m_cmtDiv & 1) {
            if ((m_cmtDiv & 7) == 1) out = !out;
            else if ((m_cmtDiv & 7) == 5 && m_cmtSod) out = true;
        } else {
            m_cmtSod = m_sod;
        }
        if (out != m_cmtOut) {
            m_cmtOut = out;
            tape.output(out ? 63 : 0);
        }
    }
}

void System::putPc8201(std::vector<uint8_t>& v) const {
    const uint8_t b[] = {m_scp, (uint8_t)m_casRomSel, (uint8_t)m_sod, (uint8_t)m_cmtSod, (uint8_t)m_cmtOut, m_cmtDiv,
                         (uint8_t)m_casRomAddr, (uint8_t)(m_casRomAddr >> 8), (uint8_t)(m_casRomAddr >> 16)};
    v.insert(v.end(), b, b + sizeof b);
    for (int i = 0; i < 8; ++i) v.push_back((uint8_t)(m_cmtAcc >> (8 * i)));
}

void System::getPc8201(const uint8_t*& p) {
    m_scp = p[0];
    m_casRomSel = p[1];
    m_sod = p[2];
    m_cmtSod = p[3];
    m_cmtOut = p[4];
    m_cmtDiv = p[5];
    m_casRomAddr = (uint32_t)p[6] | (uint32_t)p[7] << 8 | (uint32_t)p[8] << 16;
    p += 9;
    m_cmtAcc = 0;
    for (int i = 0; i < 8; ++i) m_cmtAcc |= (uint64_t)*p++ << (8 * i);
}

}  // namespace m100
