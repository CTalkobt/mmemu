#pragma once

// What plugs into the SNES's two controller ports (#15), after Mesen2
// (Core/SNES/Input/, GPL-3.0, Sour and the Mesen contributors): the pad, the
// mouse, the Super Scope and the multitap. Each is a shift register clocked
// by reads of $4016/$4017 (or the auto joypad read), latched when OUT0
// ($4016 bit 0, or the auto read's strobe) falls; while OUT0 is high it
// keeps reloading. Bits come out first-to-last from bit 15 (bit 31 for the
// mouse), so a pad's 16 bits are in $4218/$4219's layout. Mesen has no
// Justifier; it follows bsnes (sfc/controller/justifier, GPL-3.0, byuu).

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>

namespace snes {

class InputDevice {
public:
    virtual ~InputDevice() = default;
    /** OUT0: falling, the device latches its state. */
    virtual void strobe(bool on) {
        const bool was = m_strobe;
        m_strobe = on;
        if (was && !on) latch();
    }
    /** A read of its port: bit 0 D0, bit 1 D1. `wrio`: $4201 (the multitap's select). */
    virtual uint8_t read(uint8_t wrio) = 0;
    /** Mesen's RefreshStateBuffer. */
    virtual void latch() = 0;

protected:
    bool m_strobe = false;
};

/** The pad: 12 buttons ($4218/$4219's layout, B in bit 15), then 0000, then 1s. */
class Pad : public InputDevice {
public:
    uint16_t buttons = 0;
    uint8_t read(uint8_t) override {
        if (m_strobe) latch();
        const uint8_t v = (uint8_t)(m_buf >> 15);
        m_buf = (uint16_t)(m_buf << 1 | 1);
        return v;
    }
    void latch() override { m_buf = buttons; }

private:
    uint16_t m_buf = 0;
};

/** The mouse: 32 bits (0, buttons/sensitivity/signature, Y, X); a read while strobed steps the sensitivity. */
class Mouse : public InputDevice {
public:
    /** The host's movement (accumulated until the mouse is next latched) and buttons (bit 0 left, bit 1 right). */
    void move(int dx, int dy) { m_dx += dx; m_dy += dy; }
    uint8_t buttons = 0;
    uint8_t sensitivity() const { return m_sensitivity; }

    uint8_t read(uint8_t) override {
        if (m_strobe) {
            latch();
            m_sensitivity = (uint8_t)((m_sensitivity + 1) % 3);
        }
        const uint8_t v = (uint8_t)(m_buf >> 31);
        m_buf = m_buf << 1 | 1;
        return v;
    }
    void latch() override {
        static constexpr uint8_t LUT[2][8] = {{0, 1, 2, 3, 8, 10, 12, 21}, {0, 1, 4, 9, 12, 20, 24, 28}};
        int dx = m_dx, dy = m_dy;
        m_dx = m_dy = 0;
        // The direction bits keep the last way moved
        if (dx) m_left = dx < 0 ? 0x80 : 0;
        if (dy) m_up = dy < 0 ? 0x80 : 0;
        dx = std::min(std::abs(dx), 127);
        dy = std::min(std::abs(dy), 127);
        if (m_sensitivity > 0) {
            dx = LUT[m_sensitivity - 1][std::min(7, dx)];
            dy = LUT[m_sensitivity - 1][std::min(7, dy)];
        }
        const uint8_t b2 = (uint8_t)(0x01 | (m_sensitivity & 3) << 4 | ((buttons & 1) ? 0x40 : 0) | ((buttons & 2) ? 0x80 : 0));
        m_buf = (uint32_t)b2 << 16 | (uint32_t)(dy | m_up) << 8 | (uint32_t)(dx | m_left);
    }

private:
    uint32_t m_buf = 0;
    int m_dx = 0, m_dy = 0;
    uint8_t m_sensitivity = 0, m_up = 0, m_left = 0;
};

/** The Super Scope: fire, cursor, turbo, pause, -, -, offscreen, -, then 1s. */
class SuperScope : public InputDevice {
public:
    enum : uint8_t { FIRE = 1, CURSOR = 2, TURBO = 4, PAUSE = 8 };
    /** Where it points, in dots and lines; outside the picture it's offscreen. */
    int x = 128, y = 112;
    uint8_t buttons = 0;
    bool onScreen() const { return x >= 0 && y >= 0 && x < 256 && y < 240; }

    uint8_t read(uint8_t) override {
        if (m_strobe) latch();
        const uint8_t v = (uint8_t)(m_buf >> 15);
        m_buf = (uint16_t)(m_buf << 1 | 1);
        return v;
    }
    void latch() override {
        // Turbo toggles; fire is once per press unless turbo is on; pause once per press (Mesen's ToByte)
        if (!(m_prev & TURBO) && (buttons & TURBO)) m_turbo = !m_turbo;
        uint16_t v = 0x00FF;
        if ((m_turbo || !(m_prev & FIRE)) && (buttons & FIRE)) v |= 0x8000;
        if (buttons & CURSOR) v |= 0x4000;
        if (m_turbo) v |= 0x2000;
        if (!(m_prev & PAUSE) && (buttons & PAUSE)) v |= 0x1000;
        if (!onScreen()) v |= 0x0200;
        m_buf = v;
        m_prev = buttons;
    }
    /** Where the PPU should latch its counters this frame (Mesen: offset to the cursor's centre); false for nowhere. */
    bool latchAt(int& dot, int& line) const {
        if (!onScreen() || !(buttons & (FIRE | CURSOR))) return false;
        dot = x + 10;
        line = std::max(0, y - 3);
        return true;
    }

private:
    uint16_t m_buf = 0;
    uint8_t m_prev = 0;
    bool m_turbo = false;
};

/**
 * Konami's Justifier, after bsnes: one gun (or two, the second chained to the
 * first). Each latch makes the other gun the active one, and the active gun
 * latches the PPU's counters every frame it points at the picture, with or
 * without the trigger. 32 bits: 12 zeros, the signature 1110 0101 0101, the
 * triggers, the starts, which gun is active, 000, then 1s.
 */
class Justifier : public InputDevice {
public:
    struct Gun {
        int x = 128, y = 112;
        bool trigger = false, start = false;
        bool onScreen() const { return x >= 0 && y >= 0 && x < 256 && y < 240; }
    };
    Gun guns[2];
    Justifier() { guns[1].x = guns[1].y = -1; }       // no second gun plugged in
    int active() const { return m_active; }

    void strobe(bool on) override {
        const bool was = m_strobe;
        m_strobe = on;
        if (was && !on) { m_active ^= 1; latch(); }   // bsnes toggles even with one gun
    }
    uint8_t read(uint8_t) override {
        if (m_strobe) latch();
        const uint8_t v = (uint8_t)(m_buf >> 31);
        m_buf = m_buf << 1 | 1;
        return v;
    }
    void latch() override {
        m_buf = 0x000E5500u | (uint32_t)guns[0].trigger << 7 | (uint32_t)guns[1].trigger << 6 |
                (uint32_t)guns[0].start << 5 | (uint32_t)guns[1].start << 4 | (uint32_t)m_active << 3;
    }
    /** Where the active gun has the PPU latch its counters (as the Super Scope); false for nowhere. */
    bool latchAt(int& dot, int& line) const {
        const Gun& g = guns[m_active];
        if (!g.onScreen()) return false;
        dot = g.x + 10;
        line = std::max(0, g.y - 3);
        return true;
    }

private:
    uint32_t m_buf = 0;
    int m_active = 0;
};

/** The multitap: four pads, two at a time by $4201's bit 6 (port 1) or 7 (port 2), on D0 and D1. */
class Multitap : public InputDevice {
public:
    explicit Multitap(int port) : m_port(port) {}
    std::array<Pad, 4> pads;

    uint8_t read(uint8_t wrio) override {
        const uint8_t select = (uint8_t)(0x80 >> (m_port == 0 ? 1 : 0));
        const int first = (wrio & select) ? 0 : 2;
        const uint8_t v = (uint8_t)((pads[first].read(wrio) & 1) | (pads[first + 1].read(wrio) & 1) << 1);
        return m_strobe ? 0x02 : v;     // strobed: D1 high, D0 low
    }
    void latch() override {}
    void strobe(bool on) override { InputDevice::strobe(on); for (auto& p : pads) p.strobe(on); }

private:
    int m_port;
};

}  // namespace snes
