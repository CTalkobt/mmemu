// MC6883 SAM, after the data sheet; the cycle stretching on rate changes
// and the video counter's divider chain as XRoar models them (mc6883.c,
// GPL-3.0-or-later, after Stewart Orchard). mmsim's own implementation.

#include "mc6883.h"
#include <cstring>

namespace {

enum { DIV1, DIV2, DIV3, DIV12 };
const int Y_DIV[8] = {DIV12, DIV1, DIV3, DIV1, DIV2, DIV1, DIV1, DIV1};
const int X_DIV[8] = {DIV1, DIV3, DIV1, DIV2, DIV1, DIV1, DIV1, DIV1};
const int HS_CLEAR[8] = {4, 3, 4, 3, 4, 3, 4, 0};     // bits 1-4, 1-3, or none (DMA)

// The address multiplexer by memory size (data sheet figure 6): row mask,
// column shift and mask, the address bit that picks RAS1 (4K, 16K)
const uint16_t ROW_MASK[4] = {0x7F, 0x7F, 0xFF, 0xFF};
const uint8_t COL_SHIFT[4] = {6, 7, 8, 8};
const uint16_t COL_MASK[4] = {0x3F, 0x7F, 0xFF, 0xFF};
const uint16_t RAS1_BIT[4] = {0x1000, 0x4000, 0, 0};

template <class T> void put(std::vector<uint8_t>& v, const T& x) {
    const auto* p = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), p, p + sizeof(T));
}
template <class T> void get(const uint8_t*& p, const uint8_t* end, T& x) {
    if (p + sizeof(T) > end) return;
    std::memcpy(&x, p, sizeof(T));
    p += sizeof(T);
}

}  // namespace

Mc6883::Mc6883() {
    // input, modulus, output bit
    const struct { int from; uint16_t mod, mask; } init[NUM_COUNTERS] = {
        {C_B4, 2048, 0},     // B15-B5
        {C_YDIV3, 4, 2},     // Y /4 (after /3: /12)
        {C_B4, 3, 2},        // Y /3
        {C_B4, 2, 1},        // Y /2
        {C_B3_0, 2, 1},      // B4
        {C_B3_0, 3, 2},      // X /3
        {C_B3_0, 2, 1},      // X /2
        {C_GROUND, 16, 8},   // B3-B0 (clocked by the VDG)
        {C_GROUND, 1, 0},    // ground
    };
    for (int i = 0; i < NUM_COUNTERS; ++i) {
        m_c[i].from = init[i].from;
        m_c[i].mod = init[i].mod;
        m_c[i].outMask = init[i].mask;
    }
    m_vPrev = 7;           // so the first update() wires the dividers
    reset();
}

void Mc6883::reset() {
    setReg(0);
    fsync(true);
    m_runningFast = false;
    m_extendSlow = false;
}

uint16_t Mc6883::reg() const {
    return (uint16_t)((m_v & 7) | ((m_f & 0xFE00) >> 6) | (m_p ? 0x400 : 0) | (m_r << 11) | (m_m << 13) | (m_ty ? 0x8000 : 0));
}

void Mc6883::setReg(uint16_t v) {
    m_v = v & 7;
    m_f = (uint16_t)((v << 6) & 0xFE00);
    m_p = (v >> 10) & 1;
    m_r = (v >> 11) & 3;
    m_m = (v >> 13) & 3;
    m_ty = (v >> 15) & 1;
    update();
}

uint8_t Mc6883::select(uint16_t a, bool write) const {
    const bool ffxx = (a >> 8) == 0xFF;
    if (ffxx) {
        switch ((a >> 5) & 7) {
            case 0: return S_IO0;
            case 1: return S_IO1;
            case 2: return S_IO2;
            case 7: return S_ROM1;           // the vectors, from the $A000 ROM
            default: return S_NONE;
        }
    }
    if (!(a & 0x8000)) return write ? S_NONE : S_RAM;
    if (m_ty && !write) return S_RAM;
    switch (a >> 13) {
        case 4: return S_ROM0;
        case 5: return S_ROM1;
        default: return S_CTS;
    }
}

void Mc6883::translate(uint16_t a, uint16_t& row, uint16_t& col, uint8_t& bank, bool cpu) const {
    const bool bank0 = !(a & m_ras1Bit);
    row = (uint16_t)((a & m_rowMask) | (bank0 ? m_ras1 : 0));
    col = (uint16_t)(((a >> m_colShift) & m_colMask) | (bank0 ? m_ras1 : 0) | (cpu ? m_pageBit : 0));
    bank = (m_ras1 && (a & m_ras1Bit)) ? 1 : 0;
}

Mc6883::Cycle Mc6883::decode(uint16_t a, bool write) const {
    Cycle c;
    const bool ffxx = (a >> 8) == 0xFF;
    c.s = select(a, write);
    c.ram = !(a & 0x8000) || (m_ty && !ffxx);
    c.write = c.ram && write;
    if (c.ram) translate(a, c.row, c.col, c.bank, true);
    return c;
}

Mc6883::Cycle Mc6883::access(uint16_t a, bool write) {
    Cycle c = decode(a, write);
    const bool ffxx = (a >> 8) == 0xFF;
    const unsigned io = (a >> 5) & 7;
    if (c.ram) {
        m_lastRow = c.row;
        m_lastCol = c.col;
    }

    // Rate: R1 fast always; R0 alone fast except for RAM and $FF00-$FF1F
    const bool fast = (m_r & 2) || (m_r == 1 && !(c.ram || (ffxx && io == 0)));
    if (!m_runningFast) {
        c.ticks = fast ? 15 : 16;
        m_runningFast = fast;
    } else if (!fast) {
        c.ticks = m_extendSlow ? 25 : 17;     // back in step with the VDG's slots
        m_extendSlow = false;
        m_runningFast = false;
    } else {
        c.ticks = 8;
        m_extendSlow = !m_extendSlow;
    }

    // $FFC0-$FFDF: set or clear a register bit
    if (write && ffxx && io == 6) {
        const unsigned bit = (a >> 1) & 15;
        const bool on = a & 1;
        if ((a <= 0xFFC5 || (a >= 0xFFDA && a <= 0xFFDD)) && onVideoChange) onVideoChange();
        uint16_t r = reg();
        r = on ? (uint16_t)(r | (1u << bit)) : (uint16_t)(r & ~(1u << bit));
        setReg(r);
    }
    return c;
}

void Mc6883::setCounter(int i, int value) {
    Counter& k = m_c[i];
    k.value = (uint16_t)value;
    k.out = (value & k.outMask) != 0;
    for (int j = 0; j < C_B3_0; ++j)
        if (m_c[j].from == i) clockCounter(j);
}

void Mc6883::clockCounter(int i) {
    Counter& k = m_c[i];
    const bool in = m_c[k.from].out;
    if (in == k.in) return;
    k.in = in;
    if (!in) setCounter(i, (k.value + 1) % k.mod);      // counts on a falling edge
}

void Mc6883::update() {
    const int oldY = Y_DIV[m_vPrev], oldX = X_DIV[m_vPrev];
    const int newY = Y_DIV[m_v], newX = X_DIV[m_v];
    m_clear = HS_CLEAR[m_v];
    m_vPrev = m_v;

    if (newY != oldY) {
        // A divider switched out mid-count can clock the next stage: the
        // SAM's "glitches" (the old input briefly routed via ground or B4)
        if (newY == DIV12 && oldY == DIV3) { m_c[C_B15_5].from = C_GROUND; clockCounter(C_B15_5); }
        else if (newY == DIV12 && oldY == DIV2) { m_c[C_B15_5].from = C_B4; clockCounter(C_B15_5); }
        else if (newY == DIV3 && oldY == DIV12) { m_c[C_B15_5].from = C_GROUND; clockCounter(C_B15_5); }
        else if (newY == DIV2 && oldY == DIV12) { m_c[C_B15_5].from = C_B4; clockCounter(C_B15_5); }
        m_c[C_B15_5].from = newY == DIV12 ? C_YDIV4 : newY == DIV3 ? C_YDIV3 : newY == DIV2 ? C_YDIV2 : C_B4;
        clockCounter(C_YDIV2);
        clockCounter(C_YDIV3);
        clockCounter(C_YDIV4);
        clockCounter(C_B15_5);
    }
    if (newX != oldX) {
        if ((newX == DIV3 && oldX == DIV2) || (newX == DIV2 && oldX == DIV3)) {
            m_c[C_B4].from = C_GROUND;
            clockCounter(C_B4);
        }
        m_c[C_B4].from = newX == DIV3 ? C_XDIV3 : newX == DIV2 ? C_XDIV2 : C_B3_0;
        clockCounter(C_XDIV2);
        clockCounter(C_XDIV3);
        clockCounter(C_B4);
    }

    m_rowMask = ROW_MASK[m_m];
    m_colShift = COL_SHIFT[m_m];
    m_colMask = COL_MASK[m_m];
    m_ras1Bit = RAS1_BIT[m_m];
    if (m_m < 2) {          // 4K and 16K: two banks; RAS1 by A12 / A14
        m_pageBit = 0;
        m_ras1 = 0x80;
    } else {                // 64K: one bank; P1 selects the upper 32K
        m_pageBit = m_p ? 0x80 : 0;
        m_ras1 = 0;
    }
}

void Mc6883::hsync(bool level) {
    if (level) return;
    if (m_clear == 4) {
        // B1-B4 cleared
        m_c[C_B3_0].value = 0;
        m_c[C_B3_0].out = false;
        m_c[C_XDIV3].in = false;
        m_c[C_XDIV2].in = false;
        m_c[C_B4].in = false;
        m_c[C_B4].value = 0;
        m_c[C_B4].out = false;
        clockCounter(C_YDIV2);
        clockCounter(C_YDIV3);
        clockCounter(C_YDIV4);
        clockCounter(C_B15_5);
    } else if (m_clear == 3) {
        // B1-B3 cleared
        m_c[C_B3_0].value = 0;
        m_c[C_B3_0].out = false;
        clockCounter(C_XDIV2);
        clockCounter(C_XDIV3);
        clockCounter(C_B4);
    }
}

void Mc6883::fsync(bool level) {
    if (!level) return;
    for (int i : {C_B3_0, C_XDIV2, C_XDIV3, C_B4, C_YDIV2, C_YDIV3, C_YDIV4, C_B15_5}) {
        m_c[i].in = false;
        m_c[i].value = 0;
        m_c[i].out = false;
    }
    m_c[C_B15_5].value = (uint16_t)(m_f >> 5);
}

uint16_t Mc6883::videoAddress() const {
    return (uint16_t)((m_c[C_B15_5].value << 5) | (m_c[C_B4].value << 4) | m_c[C_B3_0].value);
}

int Mc6883::videoBytes(int n, uint16_t& row, uint16_t& col) {
    const int b3_0 = m_c[C_B3_0].value;
    if (m_r & 2) {
        // Fast: no time for the VDG, which sees whatever the CPU addressed
        row = m_lastRow;
        col = m_lastCol;
    } else {
        uint8_t bank;
        translate(videoAddress(), row, col, bank, false);
    }
    if (b3_0 + n < 16) {
        setCounter(C_B3_0, b3_0 + n);
        return n;
    }
    // To the 16-byte boundary: B3's fall clocks the X divider
    n = 16 - b3_0;
    setCounter(C_B3_0, 15);
    setCounter(C_B3_0, 0);
    return n;
}

std::vector<uint8_t> Mc6883::saveState() const {
    std::vector<uint8_t> v;
    put(v, reg());
    put(v, m_vPrev); put(v, m_runningFast); put(v, m_extendSlow); put(v, m_lastRow); put(v, m_lastCol);
    for (const auto& c : m_c) { put(v, c.value); put(v, c.in); put(v, c.out); put(v, c.from); }
    return v;
}

void Mc6883::loadState(const std::vector<uint8_t>& s) {
    const uint8_t* p = s.data();
    const uint8_t* e = p + s.size();
    uint16_t r = 0;
    get(p, e, r);
    m_vPrev = r & 7;            // no divider re-wiring: the counters come next
    setReg(r);
    get(p, e, m_vPrev); get(p, e, m_runningFast); get(p, e, m_extendSlow); get(p, e, m_lastRow); get(p, e, m_lastCol);
    for (auto& c : m_c) { get(p, e, c.value); get(p, e, c.in); get(p, e, c.out); get(p, e, c.from); }
}
