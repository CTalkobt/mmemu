#include "mouse_card.h"
#include "apple2_parts.h"
#include <cstdio>
#include <cstring>

namespace apple2 {

namespace {
// The 68705's commands (the high nibble of the first byte)
constexpr uint8_t MOUSE_SET = 0x00, MOUSE_READ = 0x10, MOUSE_SERV = 0x20, MOUSE_CLEAR = 0x30, MOUSE_POS = 0x40,
                  MOUSE_INIT = 0x50, MOUSE_CLAMP = 0x60, MOUSE_HOME = 0x70, MOUSE_TIME = 0x90;
// The status byte
constexpr uint8_t STAT_PREV_BUTTON1 = 0x01, STAT_INT_MOVEMENT = 0x02, STAT_INT_BUTTON = 0x04, STAT_INT_VBL = 0x08,
                  STAT_CURR_BUTTON1 = 0x10, STAT_MOVED = 0x20, STAT_PREV_BUTTON0 = 0x40, STAT_CURR_BUTTON0 = 0x80;
constexpr uint8_t STAT_INT_ALL = STAT_INT_VBL | STAT_INT_BUTTON | STAT_INT_MOVEMENT;
// The mode byte
constexpr uint8_t MODE_MOUSE_ON = 0x01, MODE_INT_ALL = STAT_INT_ALL;
}  // namespace

MouseCard* MouseCard::create(int slot, std::string& err) {
    std::vector<uint8_t> rom;
    if (!loadFile("roms/apple2/MouseInterface.rom", 0x800, rom, &err)) return nullptr;
    return new MouseCard(slot, std::move(rom));
}

MouseCard::MouseCard(int slot, std::vector<uint8_t> rom) : m_slot(slot), m_rom(std::move(rom)) {
    m_name = "Mouse Card (slot " + std::to_string(slot) + ")";
    reset();
}

void MouseCard::reset() {
    m_pra = m_ddra = m_cra = m_prb = m_ddrb = m_crb = 0;
    m_pa = 0;
    m_pb = 0x40;                            // PB6: the 68705 is ready
    m_ib = m_pb;
    m_ia = 0;
    m_mode = 0;
    m_nx = m_ny = m_ix = m_iy = 0;
    m_minX = m_minY = 0;
    m_maxX = m_maxY = 1023;
    m_btn[0] = m_btn[1] = false;
    clear();
    std::memset(m_buf, 0, sizeof m_buf);
    m_irq = false;
}

// --- The 6821 --------------------------------------------------------------

uint8_t MouseCard::piaRead(int rs) {
    switch (rs & 3) {
        case 0:
            if (!(m_cra & 0x04)) return m_ddra;
            m_cra &= 0x3F;                  // reading the port clears IRQA1/IRQA2
            return (uint8_t)((m_pra & m_ddra) | (m_ia & ~m_ddra));
        case 1: return m_cra;
        case 2:
            if (!(m_crb & 0x04)) return m_ddrb;
            m_crb &= 0x3F;
            return (uint8_t)((m_prb & m_ddrb) | (m_ib & ~m_ddrb));
        default: return m_crb;
    }
}

void MouseCard::piaWrite(int rs, uint8_t v) {
    switch (rs & 3) {
        case 0:
            if (m_cra & 0x04) m_pra = v;
            else if (m_ddra == v) return;
            else m_ddra = v;
            if (m_ddra) outA(m_pra & m_ddra);
            break;
        case 1: m_cra = v; break;
        case 2:
            if (m_crb & 0x04) m_prb = v;
            else if (m_ddrb == v) return;
            else m_ddrb = v;
            if (m_ddrb) outB(m_prb & m_ddrb);
            break;
        default: m_crb = v; break;
    }
}

int MouseCard::deviceIo(int reg, const uint8_t* wr, uint64_t) {
    if (wr) { piaWrite(reg, *wr); return 0; }
    return piaRead(reg);
}

int MouseCard::devicePeek(int reg) {
    switch (reg & 3) {
        case 0: return (m_cra & 0x04) ? (uint8_t)((m_pra & m_ddra) | (m_ia & ~m_ddra)) : m_ddra;
        case 1: return m_cra;
        case 2: return (m_crb & 0x04) ? (uint8_t)((m_prb & m_ddrb) | (m_ib & ~m_ddrb)) : m_ddrb;
        default: return m_crb;
    }
}

// --- The 68705 -------------------------------------------------------------

// Port B from the 6502: b5 falling, a byte written to the 68705; b4 falling,
// the next byte read from it. b1-b3 page the firmware.
void MouseCard::outB(uint8_t v) {
    const uint8_t diff = (uint8_t)((m_pb ^ v) & 0x3E);
    if (!diff) return;
    m_pb = (uint8_t)((m_pb & ~0x3E) | (v & 0x3E));
    if (diff & 0x20) {
        if (v & 0x20) {
            m_pb |= 0x80;                   // ready to take a byte
        } else {
            m_buf[m_bufPos++] = m_pa;
            if (m_bufPos == 1) command();
            if (m_bufPos == m_dataLen || m_bufPos > 7) {
                commit();
                m_bufPos = 0;
            }
            m_pb &= (uint8_t)~0x80;
            m_ib = m_pb;
        }
    }
    if (diff & 0x10) {
        if (v & 0x10) {
            m_pb &= (uint8_t)~0x40;         // getting the next byte ready
        } else {
            if (m_bufPos) ++m_bufPos;
            if (m_bufPos == m_dataLen || m_bufPos > 7) m_bufPos = 0;
            else m_ia = m_buf[m_bufPos];
            m_pb |= 0x40;
        }
    }
    m_ib = m_pb;
}

void MouseCard::command() {
    switch (m_buf[0] & 0xF0) {
        case MOUSE_SET:
            m_dataLen = 1;
            m_mode = m_buf[0] & 0x0F;
            break;
        case MOUSE_READ:
            m_dataLen = 6;
            m_state &= STAT_MOVED;
            m_nx = m_ix;
            m_ny = m_iy;
            if (m_btnRead[0]) m_state |= STAT_PREV_BUTTON0;
            if (m_btnRead[1]) m_state |= STAT_PREV_BUTTON1;
            m_btnRead[0] = m_btn[0];
            m_btnRead[1] = m_btn[1];
            if (m_btnRead[0]) m_state |= STAT_CURR_BUTTON0;
            if (m_btnRead[1]) m_state |= STAT_CURR_BUTTON1;
            m_buf[1] = (uint8_t)m_nx;
            m_buf[2] = (uint8_t)(m_nx >> 8);
            m_buf[3] = (uint8_t)m_ny;
            m_buf[4] = (uint8_t)(m_ny >> 8);
            m_buf[5] = m_state;
            m_state &= (uint8_t)~STAT_MOVED;
            break;
        case MOUSE_SERV:
            m_dataLen = 2;
            m_buf[1] = m_state & (uint8_t)~STAT_MOVED;   // why it interrupted
            m_irq = false;
            break;
        case MOUSE_CLEAR:
            clear();                        // the clamps stay (Fantavision)
            m_dataLen = 1;
            break;
        case MOUSE_POS: m_dataLen = 5; break;
        case MOUSE_INIT:
            m_dataLen = 3;
            m_buf[1] = 0xFF;
            break;
        case MOUSE_CLAMP: m_dataLen = 5; break;
        case MOUSE_HOME:
            m_dataLen = 1;
            m_ix = m_iy = 0;
            break;
        case MOUSE_TIME:
            switch (m_buf[0] & 0x0C) {
                case 0x00: m_dataLen = 1; break;
                case 0x04: m_dataLen = 3; break;
                case 0x08: m_dataLen = 2; break;
                default: m_dataLen = 4; break;
            }
            break;
        case 0xA0: m_dataLen = 2; break;
        default: m_dataLen = 1; break;
    }
    m_ia = m_buf[1];
}

void MouseCard::commit() {
    switch (m_buf[0] & 0xF0) {
        case MOUSE_CLAMP: {
            const int lo = (m_buf[3] << 8) | m_buf[1];
            const int hi = (m_buf[4] << 8) | m_buf[2];
            if (m_buf[0] & 1) setClampY(lo, hi);
            else setClampX(lo, hi);
            break;
        }
        case MOUSE_POS:
            m_nx = (m_buf[2] << 8) | m_buf[1];
            m_ny = (m_buf[4] << 8) | m_buf[3];
            m_ix = m_nx;
            m_iy = m_ny;
            break;
        case MOUSE_INIT:
            m_nx = m_ny = 0;
            setClampX(0, 1023);
            setClampY(0, 1023);
            m_ix = m_iy = 0;
            break;
        default: break;
    }
}

// Movement, a button or the vertical blank: an interrupt if the mode asks
void MouseCard::event(bool vbl) {
    uint8_t st = 0;
    if ((m_mode & STAT_INT_VBL) && vbl) st |= STAT_INT_VBL;
    if (m_mode & MODE_MOUSE_ON) {
        if (m_nx != m_ix || m_ny != m_iy) {
            st |= STAT_INT_MOVEMENT | STAT_MOVED;
            m_state |= STAT_MOVED;          // CopyII+ 9.1, ProTERM 3.1
        }
        if (m_btnRead[0] != m_btn[0] || m_btnRead[1] != m_btn[1]) st |= STAT_INT_BUTTON;
        st &= (uint8_t)((m_mode & MODE_INT_ALL) | STAT_MOVED);   // Contiki keeps "moved" for READMOUSE
    } else {
        st &= STAT_INT_VBL;                 // off: only the VBL (AppleWin GH#1138)
    }
    if (st & STAT_INT_ALL) {
        m_state |= st;
        m_irq = true;
    }
}

void MouseCard::clear() {
    m_bufPos = 0;
    m_dataLen = 1;
    m_state = 0;
    m_nx = m_ny = 0;
    m_btnRead[0] = m_btnRead[1] = false;
    m_ix = m_iy = 0;
}

void MouseCard::clampX() { m_ix = m_ix > m_maxX ? m_maxX : m_ix < m_minX ? m_minX : m_ix; }
void MouseCard::clampY() { m_iy = m_iy > m_maxY ? m_maxY : m_iy < m_minY ? m_minY : m_iy; }

void MouseCard::setClampX(int lo, int hi) {
    if (lo > hi) { hi = (lo + hi) & 0xFFFF; lo = 0; }
    m_minX = lo;
    m_maxX = hi;
    clampX();
}

void MouseCard::setClampY(int lo, int hi) {
    if (lo > hi) { hi = (lo + hi) & 0xFFFF; lo = 0; }
    m_minY = lo;
    m_maxY = hi;
    clampY();
}

void MouseCard::move(int dx, int dy) {
    if (!dx && !dy) return;
    m_ix += dx;
    clampX();
    m_iy += dy;
    clampY();
    event(false);
}

void MouseCard::setButtons(bool b0, bool b1) {
    if (b0 == m_btn[0] && b1 == m_btn[1]) return;
    m_btn[0] = b0;
    m_btn[1] = b1;
    event(false);
}

void MouseCard::getDeviceInfo(DeviceInfo& out) const {
    out.name = m_name;
    out.baseAddr = 0xC080u + (uint32_t)m_slot * 16u;
    out.addrMask = 0x03;
    char buf[160];
    std::snprintf(buf, sizeof buf, "x %d (%d-%d), y %d (%d-%d), mode $%02X, status $%02X%s", m_ix, m_minX, m_maxX, m_iy,
                  m_minY, m_maxY, m_mode, m_state, m_irq ? ", IRQ" : "");
    out.state.push_back({"mouse", buf});
}

}  // namespace apple2
