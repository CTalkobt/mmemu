// The Super Game Boy's ICD2, after Mesen2 (Core/SNES/Coprocessors/SGB/SuperGameboy.cpp),
// GPL-3.0, Sour and the Mesen contributors.

#include "snes_sgb.h"

#include <cstring>

namespace snes {

namespace {
constexpr uint64_t SGB2_CRYSTAL_HZ = 20971520;
}

Sgb::Sgb(bool sgb2) : m_sgb2(sgb2) {
    m_gb.setModel(sgb2 ? gb::Gameboy::Model::Sgb2 : gb::Gameboy::Model::Sgb);
}

bool Sgb::loadCartridge(const std::vector<uint8_t>& rom, std::string& err) {
    if (!m_gb.load(rom, err)) return false;
    m_gb.powerOn(this);            // held until the BIOS lets it run (as Mesen)
    return true;
}

uint64_t Sgb::gbClockHz() const {
    static const uint32_t dividers[4] = {4, 5, 7, 9};
    const uint32_t div = dividers[m_control & 3];
    return (m_sgb2 ? SGB2_CRYSTAL_HZ : m_masterHz) / div;
}

uint64_t Sgb::gbTarget(uint64_t master) const {
    // SGB1: the master clock / divider; SGB2: its own crystal / divider, in master clock time
    const uint64_t delta = master - m_resetClock;
    if (!m_sgb2) return m_clockOffset + delta / m_divider;
    // delta * crystal / (divider * masterHz) without overflowing
    const uint64_t q = delta / m_masterHz, r = delta % m_masterHz;
    const uint64_t a = q * SGB2_CRYSTAL_HZ;
    return m_clockOffset + a / m_divider + ((a % m_divider) * m_masterHz + r * SGB2_CRYSTAL_HZ) / (m_divider * m_masterHz);
}

void Sgb::runTo(uint64_t master) {
    if (!(m_control & 0x80) || !m_gb.loaded()) return;
    m_gb.runTo(gbTarget(master));
}

void Sgb::updateClockRatio(uint64_t master) {
    static const uint32_t dividers[4] = {4, 5, 7, 9};
    const uint32_t div = dividers[m_control & 3];
    if (div != m_divider) {
        if (m_divider) runTo(master);
        m_clockOffset = m_gb.cycles();
        m_resetClock = master;
        m_divider = div;
        m_gb.apu().setSampleRate(32000.0, (double)gbClockHz());
    }
}

void Sgb::reset(uint64_t master, uint64_t masterHz) {
    m_masterHz = masterHz;
    m_control = 0x01;              // divider 5, the Game Boy held
    m_divider = 0;
    m_clockOffset = 0;
    m_resetClock = 0;
    updateClockRatio(master);
    std::memset(m_input, 0, sizeof(m_input));
    m_inputIndex = 0;
    m_listening = false;
    m_waitForHigh = true;
    m_packetReady = false;
    m_inputValue = 0;
    std::memset(m_packet, 0, sizeof(m_packet));
    m_packetByte = m_packetBit = m_packetBuffer = 0;
    m_lcdRowSelect = 0;
    m_readPosition = 0;
    std::memset(m_lcd, 0, sizeof(m_lcd));
    m_dcInL = m_dcInR = m_dcOutL = m_dcOutR = 0;
    m_lastL = m_lastR = 0;
    m_gb.apu().samples().clear();
}

uint8_t Sgb::playerCount() const {
    const uint8_t n = (uint8_t)(((m_control >> 4) & 3) + 1);
    return n >= 3 ? 4 : n;         // 2 and 3 both four players (as Mesen)
}

void Sgb::setInputIndex(uint8_t i) {
    m_gb.inputChange([&]() { m_inputIndex = i; });
}

void Sgb::setInputValue(uint8_t i, uint8_t v) {
    if (m_inputIndex == i) m_gb.inputChange([&]() { m_input[i] = v; });
    else m_input[i] = v;
}

uint8_t Sgb::read(uint16_t addr, uint64_t master, bool peek) {
    if (!peek) runTo(master);
    addr &= 0xF80F;
    if (addr >= 0x7000 && addr <= 0x700F) {
        if (!peek) m_packetReady = false;
        return m_packet[addr & 0x0F];
    }
    if (addr >= 0x7800 && addr <= 0x780F) {
        if (m_readPosition >= 320) {
            // $FF for 320-511, then back to 0
            if (!peek) m_readPosition = (m_readPosition + 1) & 0x1FF;
            return 0xFF;
        }
        const uint8_t* start = m_lcd[m_lcdRowSelect] + ((m_readPosition >> 1) & 7) * 160 + (m_readPosition >> 4) * 8;
        const int shift = m_readPosition & 1;
        uint8_t v = 0;
        for (int i = 0; i < 8; ++i) v |= (uint8_t)(((start[i] >> shift) & 1) << (7 - i));
        if (!peek) ++m_readPosition;
        return v;
    }
    switch (addr) {
    case 0x6000: return (uint8_t)((m_row & ~0x07) | m_bank);
    case 0x6002: return m_packetReady ? 1 : 0;
    case 0x600F: return 0x21;
    }
    return 0;
}

void Sgb::write(uint16_t addr, uint8_t v, uint64_t master) {
    runTo(master);
    addr &= 0xF80F;
    switch (addr) {
    case 0x6001:
        m_lcdRowSelect = v & 3;
        m_readPosition = 0;
        break;
    case 0x6003:
        if (!(m_control & 0x80) && (v & 0x80)) {
            // Out of reset: the Game Boy powers on
            m_clockOffset = 0;
            m_resetClock = master;
            if (m_gb.loaded()) m_gb.powerOn(this);
        }
        m_control = v;
        setInputIndex((uint8_t)(m_inputIndex % playerCount()));
        updateClockRatio(master);
        break;
    case 0x6004: setInputValue(0, v); break;
    case 0x6005: setInputValue(1, v); break;
    case 0x6006: setInputValue(2, v); break;
    case 0x6007: setInputValue(3, v); break;
    }
}

// ---------------------------------------------------------------------------
// What the Game Boy does
// ---------------------------------------------------------------------------

void Sgb::joypadWrite(uint8_t value) {
    if (m_inputValue == value) return;
    if (value == 0x00) {
        // The reset pulse: a packet starts
        m_waitForHigh = true;
        m_packetByte = m_packetBit = m_packetBuffer = 0;
    } else if (m_waitForHigh) {
        if (value == 0x10 || value == 0x20) {
            // Not 00 -> 30 -> 10/20 -> 30 ...: not a packet
            m_waitForHigh = false;
            m_listening = false;
        } else if (value == 0x30) {
            m_waitForHigh = false;
            m_listening = true;
        }
    } else if (m_listening) {
        if (value == 0x20) {
            // A 0 bit (or, after 128 bits, the stop bit)
            if (m_packetByte >= 16 && m_packetBit == 0) {
                m_packetReady = true;
                m_listening = false;
                ++m_packetCount;
            } else {
                m_packetBuffer &= (uint8_t)~(1 << m_packetBit);
            }
            if (++m_packetBit == 8) {
                m_packetBit = 0;
                if (m_packetByte < 16) m_packet[m_packetByte] = m_packetBuffer;
                m_packetBuffer = 0;
                ++m_packetByte;
            }
        } else if (value == 0x10) {
            // A 1 bit
            if (m_packetByte >= 16) {
                m_listening = false;          // no 1 for a stop bit
            } else {
                m_packetBuffer |= (uint8_t)(1 << m_packetBit);
                if (++m_packetBit == 8) {
                    m_packetBit = 0;
                    m_packet[m_packetByte] = m_packetBuffer;
                    m_packetBuffer = 0;
                    ++m_packetByte;
                }
            }
        }
        m_waitForHigh = m_listening;
    } else if (!(m_inputValue & 0x20) && (value & 0x20)) {
        // P15 rising outside a packet: the next player
        setInputIndex((uint8_t)((m_inputIndex + 1) % playerCount()));
    }
    m_inputValue = value;
}

void Sgb::hblank() {
    ++m_row;
    if ((m_row & 7) == 0) m_bank = (m_bank + 1) & 3;
}

void Sgb::vblank() { m_row = 0; }

void Sgb::lcdPixel(uint8_t x, uint8_t shade) {
    if (x < 160) m_lcd[m_bank][(m_row & 7) * 160 + x] = shade;
}

void Sgb::mix(int32_t& left, int32_t& right) {
    const bool running = (m_control & 0x80) && m_gb.loaded();
    auto& q = m_gb.apu().samples();
    if (running) m_gb.apu().run();                 // its sound up to where it has run
    if (q.size() >= 2) {
        m_lastL = q.front(); q.pop_front();
        m_lastR = q.front(); q.pop_front();
        // Far more queued than the DSP takes (the Game Boy ran on its own): drop to catch up
        while (q.size() > 2 * 256) { q.pop_front(); q.pop_front(); }
    } else if (!running) {
        m_lastL = m_lastR = 0;
    }                                              // else a sample short: the last one again
    // The DC blocker (the Game Boy's output sits off zero when its DACs are on)
    const double l = m_lastL, r = m_lastR;
    const double ol = l - m_dcInL + 0.999 * m_dcOutL, orr = r - m_dcInR + 0.999 * m_dcOutR;
    m_dcInL = l; m_dcInR = r; m_dcOutL = ol; m_dcOutR = orr;
    if (m_muted && m_muted()) return;
    left += (int32_t)ol;
    right += (int32_t)orr;
}

}  // namespace snes
