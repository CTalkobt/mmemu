#pragma once

// Atari 2600 TIA building blocks: the five movable objects, playfield,
// background, the register-write delay queue, the latched (INPT4/5) and
// analog (INPT0-3) inputs, the audio channels and a frame manager.
//
// After Stella's "new TIA" (src/emucore/tia/: Player, Missile, Ball,
// Playfield, Background, DelayQueue, DelayQueueMember, DrawCounterDecodes,
// LatchedInput, AnalogReadout, AudioChannel, Audio and
// frame-manager/FrameManager), by Christian Speckner (DirtyHairy), Stephen
// Anthony and the Stella team, GPL-2.0-or-later. Left out: the debugger
// hooks (toggles, debug colours, position setters), the developer-setting
// TIA quirks (inverted phase clock, short late HMOVE, late RESPx, delayed
// PF/colour/swap writes: always off, as in Stella's default), PAL colour
// loss, the line cache and the frame manager's jitter emulation.

#include <array>
#include <cstdint>

namespace tiacore {

static constexpr uint32_t H_PIXEL = 160;          ///< visible colour clocks per line
static constexpr uint32_t H_CLOCKS = 228;         ///< colour clocks per line
static constexpr uint32_t H_BLANK_CLOCKS = 68;    ///< H_CLOCKS - H_PIXEL
static constexpr uint32_t CYCLE_CLOCKS = 3;       ///< colour clocks per CPU cycle

// ---------------------------------------------------------------------------
// Collision masks (TIAConstants.hxx CollisionMask). Every object's .collision
// is either ALL (it draws this clock) or ~self & ~VISIBLE (silent); the AND of
// all six then has exactly the bits of the pairs that both draw.
// ---------------------------------------------------------------------------
namespace cmask {
static constexpr uint16_t NONE      = 0;
static constexpr uint16_t PLAYER0   = 0b0111110000000000;
static constexpr uint16_t PLAYER1   = 0b0100001111000000;
static constexpr uint16_t MISSILE0  = 0b0010001000111000;
static constexpr uint16_t MISSILE1  = 0b0001000100100110;
static constexpr uint16_t BALL      = 0b0000100010010101;
static constexpr uint16_t PLAYFIELD = 0b0000010001001011;
static constexpr uint16_t VISIBLE   = 0x8000;
static constexpr uint16_t ALL       = 0xFFFF;
/** The "silent" mask for an object. */
constexpr uint16_t disabled(uint16_t self) { return (uint16_t)(~self & ~VISIBLE); }
}  // namespace cmask

/**
 * NUSIZ decode tables (DrawCounterDecodes): for each of the 8 NUSIZ modes,
 * the counter values that start a copy (1 = the main copy, 2/3 = others).
 */
class DrawCounterDecodes {
public:
    static const DrawCounterDecodes& get();
    const uint8_t* const* playerDecodes() const { return m_playerDecodes; }
    const uint8_t* const* missileDecodes() const { return m_missileDecodes; }

private:
    DrawCounterDecodes();
    const uint8_t* m_playerDecodes[8]{};
    const uint8_t* m_missileDecodes[8]{};
    uint8_t m_decodes0[160]{}, m_decodes1[160]{}, m_decodes2[160]{};
    uint8_t m_decodes3[160]{}, m_decodes4[160]{}, m_decodes6[160]{};
};

/**
 * Fixed-length ring of pending register writes (DelayQueue / DelayQueueMember):
 * push() schedules a write `delay` colour clocks ahead; execute() runs the ones
 * due now and moves on one slot. An empty queue stands still.
 */
class DelayQueue {
public:
    static constexpr unsigned LENGTH = 16;
    static constexpr unsigned CAPACITY = 16;

    void reset();
    /** Returns false (and drops the write) if delay >= LENGTH or the slot is full. */
    bool push(uint8_t address, uint8_t value, uint8_t delay);
    bool empty() const { return m_pendingCount == 0; }

    template <typename T>
    void execute(T executor) {
        if (m_pendingCount == 0) return;
        Member& cur = m_members[m_index];
        m_pendingCount -= cur.size;
        for (uint8_t i = 0; i < cur.size; ++i) executor(cur.entries[i].address, cur.entries[i].value);
        cur.size = 0;
        m_index = (uint8_t)((m_index + 1) % LENGTH);
    }

private:
    struct Entry { uint8_t address = 0; uint8_t value = 0; };
    struct Member { std::array<Entry, CAPACITY> entries{}; uint8_t size = 0; };
    std::array<Member, LENGTH> m_members{};
    uint8_t m_index = 0;
    uint16_t m_pendingCount = 0;
};

// ---------------------------------------------------------------------------
// Player (P0/P1)
// ---------------------------------------------------------------------------
/**
 * A player sprite (Player.hxx/.cxx): position counter (0-159), NUSIZ decode,
 * render counter, 8-bit pattern shifter with 1x/2x/4x width, HMOVE motion,
 * REFP, VDEL (old/new pattern swapped by the other player's GRP write).
 */
class Player {
public:
    explicit Player(uint16_t collisionMask) : m_collisionMaskDisabled(cmask::disabled(collisionMask)) {}

    void reset();
    void grp(uint8_t pattern);
    void hmp(uint8_t value) { m_hmmClocks = (uint8_t)((value >> 4) ^ 0x08); }
    void nusiz(uint8_t value, bool hblank);
    void resp(uint8_t counter);
    void refp(uint8_t value);
    void vdelp(uint8_t value);
    void setColor(uint8_t color) { m_color = color & 0xFE; }
    void startMovement() { isMoving = true; }
    void nextLine() { updateCollisionFromState(); }
    void shufflePatterns();

    uint8_t getColor() const { return m_color; }
    bool isOn() const { return (collision & cmask::VISIBLE) != 0; }
    /** FSTOB: main copy, pattern pixel 4 — the RESMP lock point. */
    bool isDrawingMainCopyAt4() const { return m_isRendering && m_sampleCounter == 4 && m_copy == 1; }
    /** The counter value at the last RESP (Player::getRespClock). */
    uint8_t getRespClock() const;
    uint8_t counter() const { return m_counter; }
    uint8_t grpOld() const { return m_patternOld; }
    uint8_t grpNew() const { return m_patternNew; }

    /** One HMOVE clock (every 4th colour clock while moving). */
    void movementTick(uint32_t clock, bool hblank) {
        if (isMoving) {
            if (clock == m_hmmClocks) isMoving = false;
            else if (hblank) tick();
        }
    }
    /** CLKP during HBLANK: latch the output without advancing the counter. */
    void tickClkpInHblank() { updateCollisionFromState(); }

    void tick() {
        updateCollisionFromState();

        if (m_decodes[m_counter]) {
            m_isRendering = true;
            m_sampleCounter = 0;
            m_renderCounter = RENDER_COUNTER_OFFSET;
            m_copy = (int8_t)m_decodes[m_counter];
        } else if (m_isRendering) {
            ++m_renderCounter;
            if (m_divider == 1) {
                if (m_renderCounter > 0) ++m_sampleCounter;
                if (m_renderCounter >= 0 && m_dividerChangeCounter >= 0 && m_dividerChangeCounter-- == 0)
                    setDivider(m_dividerPending);
            } else {
                if (m_renderCounter > 1 && (((uint32_t)(m_renderCounter - 1) & (uint32_t)(m_divider - 1)) == 0))
                    ++m_sampleCounter;
                if (m_renderCounter > 0 && m_dividerChangeCounter >= 0 && m_dividerChangeCounter-- == 0)
                    setDivider(m_dividerPending);
            }
            if (m_sampleCounter > 7) m_isRendering = false;
        }

        if (++m_counter >= H_PIXEL) m_counter = 0;
    }

    uint16_t collision = cmask::NONE;
    bool isMoving = false;

private:
    static constexpr int8_t RENDER_COUNTER_OFFSET = -5;

    void updateCollisionFromState() {
        if (!m_isRendering || m_renderCounter < m_renderCounterTripPoint)
            collision = m_collisionMaskDisabled;
        else
            collision = (m_pattern & (1u << m_sampleCounter)) ? cmask::ALL : m_collisionMaskDisabled;
    }
    void updatePattern();
    void setDivider(uint8_t divider) {
        m_divider = divider;
        m_renderCounterTripPoint = divider == 1 ? 0 : 1;
    }

    uint16_t m_collisionMaskDisabled;
    uint8_t m_color = 0;
    uint8_t m_hmmClocks = 0;
    uint8_t m_counter = 0;
    bool m_isRendering = false;
    int8_t m_renderCounter = 0;
    int8_t m_renderCounterTripPoint = 0;
    int8_t m_copy = 1;
    uint8_t m_divider = 1;
    uint8_t m_dividerPending = 1;
    uint8_t m_sampleCounter = 0;
    int8_t m_dividerChangeCounter = -1;
    const uint8_t* m_decodes = nullptr;
    uint8_t m_decodesOffset = 0;
    uint8_t m_patternOld = 0;
    uint8_t m_patternNew = 0;
    uint8_t m_pattern = 0;
    bool m_isReflected = false;
    bool m_isDelaying = false;
};

// ---------------------------------------------------------------------------
// Missile (M0/M1)
// ---------------------------------------------------------------------------
/** A missile (Missile.hxx/.cxx): width 1/2/4/8, NUSIZ copies, ENAM, RESMP lock to its player. */
class Missile {
public:
    explicit Missile(uint16_t collisionMask) : m_collisionMaskDisabled(cmask::disabled(collisionMask)) {}

    void reset();
    void enam(uint8_t value);
    void hmm(uint8_t value) { m_hmmClocks = (uint8_t)((value >> 4) ^ 0x08); }
    void resm(uint8_t counter, bool hblank);
    void resmp(uint8_t value);
    void nusiz(uint8_t value);
    void setColor(uint8_t color) { m_color = color & 0xFE; }
    void startMovement() { isMoving = true; }
    void nextLine() {
        m_isVisible = m_isRendering && (m_renderCounter >= 0);
        updateCollision();
    }

    uint8_t getColor() const { return m_color; }
    bool isOn() const { return (collision & cmask::VISIBLE) != 0; }
    uint8_t counter() const { return m_counter; }

    void movementTick(uint8_t clock, uint8_t hclock, bool hblank) {
        if (isMoving) {
            if (clock == m_hmmClocks) isMoving = false;
            else if (hblank) tick(hclock, false);
        }
    }
    void tickClkpInHblank() {
        m_isVisible = m_isRendering && m_renderCounter >= 0;
        updateCollision();
    }

    void tick(uint8_t hclock, bool isReceivingRegularClock = true) {
        m_isVisible = m_isRendering &&
            (m_renderCounter >= 0 ||
             (isMoving && isReceivingRegularClock && m_renderCounter == -1 && m_width < 4 &&
              ((hclock + 1) % 4 == 3)));
        updateCollision();

        if (m_decodes[m_counter] && !m_resmp) {
            m_isRendering = true;
            m_renderCounter = RENDER_COUNTER_OFFSET;
            m_copy = (int8_t)m_decodes[m_counter];
        } else if (m_isRendering) {
            if (m_renderCounter == -1) {
                // The "starfield" effect (HMOVE during the visible line)
                if (isMoving && isReceivingRegularClock) {
                    switch ((hclock + 1) % 4) {
                        case 3:
                            m_effectiveWidth = m_width == 1 ? 2 : m_width;
                            if (m_width < 4) ++m_renderCounter;
                            break;
                        case 2:
                            m_effectiveWidth = 0;
                            break;
                        default:
                            m_effectiveWidth = m_width;
                            break;
                    }
                } else {
                    m_effectiveWidth = m_width;
                }
            }
            if (++m_renderCounter >= (int8_t)(isMoving ? m_effectiveWidth : m_width)) m_isRendering = false;
        }

        if (++m_counter >= H_PIXEL) m_counter = 0;
    }

    /** A visible-line clock, then the RESMP lock against its player. */
    void tick(uint8_t hclock, const Player& player) {
        tick(hclock);
        if (m_resmp && player.isDrawingMainCopyAt4()) m_counter = player.getRespClock();
    }

    uint16_t collision = cmask::NONE;
    bool isMoving = false;

private:
    static constexpr int8_t RENDER_COUNTER_OFFSET = -4;

    void updateCollision() {
        collision = (m_isVisible && m_isEnabled) ? cmask::ALL : m_collisionMaskDisabled;
    }
    void updateEnabled() {
        m_isEnabled = m_enam && !m_resmp;
        updateCollision();
    }

    uint16_t m_collisionMaskDisabled;
    bool m_isEnabled = false;
    bool m_enam = false;
    uint8_t m_resmp = 0;
    uint8_t m_hmmClocks = 0;
    uint8_t m_counter = 0;
    uint8_t m_width = 1;
    uint8_t m_effectiveWidth = 1;
    bool m_isRendering = false;
    bool m_isVisible = false;
    int8_t m_renderCounter = 0;
    int8_t m_copy = 1;
    const uint8_t* m_decodes = nullptr;
    uint8_t m_decodesOffset = 0;
    uint8_t m_color = 0;
};

// ---------------------------------------------------------------------------
// Ball
// ---------------------------------------------------------------------------
/** The ball (Ball.hxx/.cxx): width from CTRLPF, ENABL with VDELBL (old/new swapped by GRP1). */
class Ball {
public:
    explicit Ball(uint16_t collisionMask) : m_collisionMaskDisabled(cmask::disabled(collisionMask)) {}

    void reset();
    void enabl(uint8_t value);
    void hmbl(uint8_t value) { m_hmmClocks = (uint8_t)((value >> 4) ^ 0x08); }
    void resbl(uint8_t counter);
    void ctrlpf(uint8_t value);
    void vdelbl(uint8_t value);
    void setColor(uint8_t color) { m_color = color & 0xFE; }
    void startMovement() { isMoving = true; }
    void nextLine() {
        m_signalActive = m_isRendering && m_renderCounter >= 0;
        updateCollision();
    }
    void shuffleStatus();

    uint8_t getColor() const { return m_color; }
    bool isOn() const { return (collision & cmask::VISIBLE) != 0; }
    uint8_t counter() const { return m_counter; }

    void movementTick(uint32_t clock, bool hblank) {
        m_lastMovementTick = m_counter;
        if (isMoving) {
            if (clock == m_hmmClocks) isMoving = false;
            else if (hblank) tick(false);
        }
    }
    void tickClkpInHblank() {
        m_signalActive = m_isRendering && m_renderCounter >= 0;
        updateCollision();
    }

    void tick(bool isReceivingRegularClock = true) {
        m_signalActive = m_isRendering && m_renderCounter >= 0;
        updateCollision();

        const bool starfieldEffect = isMoving && isReceivingRegularClock;

        if (m_counter == 156) {
            m_isRendering = true;
            m_renderCounter = RENDER_COUNTER_OFFSET;

            const uint8_t starfieldDelta = (uint8_t)((m_counter + H_PIXEL - m_lastMovementTick) % 4);
            if (starfieldEffect && starfieldDelta == 3 && m_width < 4) ++m_renderCounter;

            switch (starfieldDelta) {
                case 3:
                    m_effectiveWidth = m_width == 1 ? 2 : m_width;
                    break;
                case 2:
                    m_effectiveWidth = 0;
                    break;
                default:
                    m_effectiveWidth = m_width;
                    break;
            }
        } else if (m_isRendering &&
                   ++m_renderCounter >= (int8_t)(starfieldEffect ? m_effectiveWidth : m_width)) {
            m_isRendering = false;
        }

        if (++m_counter >= H_PIXEL) m_counter = 0;
    }

    uint16_t collision = cmask::NONE;
    bool isMoving = false;

private:
    static constexpr int8_t RENDER_COUNTER_OFFSET = -4;

    void updateCollision() {
        collision = (m_signalActive && m_isEnabled) ? cmask::ALL : m_collisionMaskDisabled;
    }
    void updateEnabled() {
        m_isEnabled = m_isDelaying ? m_isEnabledOld : m_isEnabledNew;
        updateCollision();
    }

    uint16_t m_collisionMaskDisabled;
    uint8_t m_color = 0;
    bool m_isEnabledOld = false;
    bool m_isEnabledNew = false;
    bool m_isEnabled = false;
    bool m_isDelaying = false;
    bool m_signalActive = false;
    uint8_t m_hmmClocks = 0;
    uint8_t m_counter = 0;
    uint8_t m_width = 1;
    uint8_t m_effectiveWidth = 1;
    uint8_t m_lastMovementTick = 0;
    bool m_isRendering = false;
    int8_t m_renderCounter = 0;
};

// ---------------------------------------------------------------------------
// Playfield and background
// ---------------------------------------------------------------------------
/** The 20-bit playfield (Playfield.hxx/.cxx): PF0/PF1/PF2, reflect, score mode colours. */
class Playfield {
public:
    explicit Playfield(uint16_t collisionMask) : m_collisionMaskDisabled(cmask::disabled(collisionMask)) {}

    void reset();
    void pf0(uint8_t value);
    void pf1(uint8_t value);
    void pf2(uint8_t value);
    void ctrlpf(uint8_t value);
    void setColor(uint8_t color) { m_objectColor = color & 0xFE; applyColors(); }
    void setColorP0(uint8_t color) { m_colorP0 = color & 0xFE; applyColors(); }
    void setColorP1(uint8_t color) { m_colorP1 = color & 0xFE; applyColors(); }
    void nextLine() { collision = m_collisionMaskDisabled; }

    uint8_t getColor() const { return m_x < H_PIXEL / 2 ? m_colorLeft : m_colorRight; }
    bool isOn() const { return (collision & cmask::VISIBLE) != 0; }

    /** One clock at visible x (0-159); the playfield changes every 4 clocks. */
    void tick(uint32_t x) {
        m_x = x;

        // Reflection is latched only at the centre and the left edge
        if (m_x == H_PIXEL / 2 - 1 || m_x == 0) m_refp = m_reflected;

        if (x & 0x03) return;

        uint32_t currentPixel;
        if (m_pattern == 0) {
            currentPixel = 0;
        } else if (x < H_PIXEL / 2 - 1) {
            currentPixel = m_pattern & (1u << (x >> 2));
        } else if (m_refp) {
            const uint32_t col = x >> 2;
            currentPixel = (col <= 39) ? (m_pattern & (1u << (39u - col))) : 0;
        } else {
            currentPixel = m_pattern & (1u << ((x >> 2) - 20u));
        }

        collision = currentPixel ? cmask::ALL : m_collisionMaskDisabled;
    }

    uint16_t collision = cmask::NONE;

private:
    void applyColors();

    uint16_t m_collisionMaskDisabled;
    uint8_t m_colorLeft = 0, m_colorRight = 0;
    uint8_t m_colorP0 = 0, m_colorP1 = 0;
    uint8_t m_objectColor = 0;
    bool m_scoreMode = false;
    uint32_t m_pattern = 0;
    bool m_reflected = false;
    bool m_refp = false;
    uint8_t m_pf0 = 0, m_pf1 = 0, m_pf2 = 0;
    uint32_t m_x = 0;
};

/** COLUBK (Background.hxx/.cxx). */
class Background {
public:
    void reset() { m_color = 0; }
    void setColor(uint8_t color) { m_color = color & 0xFE; }
    uint8_t getColor() const { return m_color; }

private:
    uint8_t m_color = 0;
};

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------
/** INPT4/INPT5 with the VBLANK bit 6 latch (LatchedInput.hxx/.cxx). */
class LatchedInput {
public:
    void reset() { m_modeLatched = false; m_latchedValue = 0; }
    void vblank(uint8_t value);
    /** Read with pinState true while the button is pressed (pin low); bit 7 = 0 when pressed. */
    uint8_t inpt(bool pinState);
    /** inpt() without updating the latch. */
    uint8_t peek(bool pinState) const;

private:
    bool m_modeLatched = false;
    uint8_t m_latchedValue = 0;
};

/**
 * INPT0-3 paddle RC circuit (AnalogReadout.hxx/.cxx): the capacitor charges
 * through the pot to 5 V; VBLANK bit 7 dumps it to ground. Bit 7 reads 1 once
 * it passes the comparator threshold. Timestamps are colour clocks.
 */
class AnalogReadout {
public:
    enum class ConnectionType : uint8_t { GROUND = 0, VCC = 1, DISCONNECTED = 2 };
    struct Connection {
        ConnectionType type = ConnectionType::DISCONNECTED;
        uint32_t resistance = 0;
        bool operator==(const Connection& o) const {
            if (type == ConnectionType::DISCONNECTED) return o.type == ConnectionType::DISCONNECTED;
            return type == o.type && resistance == o.resistance;
        }
        bool operator!=(const Connection& o) const { return !(*this == o); }
    };
    static constexpr uint32_t MAX_POT_RESISTANCE = 1000000;

    AnalogReadout() { setConsoleTiming(false); }
    void reset(uint64_t timestamp, bool pal);
    void vblank(uint8_t value, uint64_t timestamp);
    uint8_t inpt(uint64_t timestamp);
    /** inpt() without advancing the stored charge. */
    uint8_t peek(uint64_t timestamp) const;
    void update(Connection connection, uint64_t timestamp);

private:
    void setConsoleTiming(bool pal);
    double chargeAt(uint64_t timestamp) const;
    void updateCharge(uint64_t timestamp);

    double m_uThresh = 0.0;
    double m_u = 0.0;
    Connection m_connection{};
    uint64_t m_timestamp = 0;
    double m_clockFreq = 0.0;
    bool m_isDumped = false;

    static constexpr double R0 = 1.8e3;     // series resistor in the TIA input
    static constexpr double C = 68e-9;      // timing capacitor
    static constexpr double R_DUMP = 50;    // dump resistor
    static constexpr double U_SUPP = 5;     // supply voltage
    static constexpr double TRIPPOINT_LINES = 379;
};

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
/** One TIA audio channel: 5-bit divider, 4-bit pulse and 5-bit noise counters (AudioChannel). */
class AudioChannel {
public:
    void reset();
    void phase0();
    void phase1();
    uint8_t actualVolume() const { return (uint8_t)((m_pulseCounter & 0x01) * m_audv); }
    void audc(uint8_t value) { m_audc = value & 0x0F; }
    void audf(uint8_t value) { m_audf = value & 0x1F; }
    void audv(uint8_t value) { m_audv = value & 0x0F; }

private:
    uint8_t m_audc = 0, m_audf = 0, m_audv = 0;
    bool m_clockEnable = false;
    bool m_noiseFeedback = false;
    bool m_noiseCounterBit4 = false;
    bool m_pulseCounterHold = false;
    uint8_t m_divCounter = 0;
    uint8_t m_pulseCounter = 0;
    uint8_t m_noiseCounter = 0;
};

/**
 * The two channels and their audio clock (Audio.hxx/.cxx): phase 0 at colour
 * clocks 10 and 82 of a line, phase 1 at 38 and 150; each phase 1 produces a
 * sample (two per line), the channel volumes averaged since the last one and
 * mixed through Stella's resistor-network table.
 */
class Audio {
public:
    Audio();
    void reset();
    void tick(uint32_t colorClocks);
    AudioChannel& channel0() { return m_channel0; }
    AudioChannel& channel1() { return m_channel1; }
    /** The last sample produced, mixed mono, 0.0-1.0. */
    float lastSample() const { return m_lastSample; }
    /** Samples produced since reset. */
    uint64_t sampleCount() const { return m_sampleCount; }

private:
    void createSample();

    uint8_t m_counter = 0;
    AudioChannel m_channel0, m_channel1;
    uint32_t m_sumChannel0 = 0, m_sumChannel1 = 0, m_sumCt = 0;
    std::array<int16_t, 0x1e + 1> m_mixingTableSum{};
    float m_lastSample = 0.0f;
    uint64_t m_sampleCount = 0;
};

// ---------------------------------------------------------------------------
// Frame manager
// ---------------------------------------------------------------------------
/**
 * Stella's FrameManager without jitter: waits for a VSYNC pulse of at least
 * two lines (or gives up after maxLinesVsync lines), then counts ystart lines
 * and renders `height` lines into the frame. A frame completes when the state
 * machine enters waitForFrameStart (VSYNC end, or the bail-out).
 */
class FrameManager {
public:
    static constexpr uint32_t FRAME_SIZE_NTSC = 262;
    static constexpr uint32_t FRAME_SIZE_PAL = 312;
    static constexpr uint32_t BASE_HEIGHT_NTSC = 228;
    static constexpr uint32_t BASE_HEIGHT_PAL = 274;
    static constexpr uint32_t YSTART_NTSC = 23;
    static constexpr uint32_t YSTART_PAL = 32;
    static constexpr uint32_t MAX_LINES_VSYNC = 50;

    void setPal(bool pal);
    void reset();
    /** Called at each new line; returns true when a frame has just completed. */
    bool nextLine();
    /** VSYNC bit 1 written; returns true when a frame has just completed. */
    bool setVsync(bool vsync);
    void setVblank(bool vblank) { m_vblank = vblank; }

    bool vblank() const { return m_vblank; }
    bool vsync() const { return m_vsync; }
    bool isRendering() const { return m_state == State::FRAME; }
    uint32_t getY() const { return m_y; }
    uint32_t height() const { return m_height; }
    uint32_t frameLines() const { return m_frameLines; }
    uint32_t currentFrameTotalLines() const { return m_currentFrameTotalLines; }
    uint32_t lastFrameLines() const { return m_currentFrameFinalLines; }
    uint64_t totalFrames() const { return m_totalFrames; }
    /** Lines not drawn in the frame that just completed (FrameManager::missingScanlines). */
    int32_t missingScanlines() const;

private:
    enum class State : uint8_t { WAIT_FOR_VSYNC_START, WAIT_FOR_VSYNC_END, WAIT_FOR_FRAME_START, FRAME };
    /** Returns true when the change completes a frame. */
    bool setState(State s);

    State m_state = State::WAIT_FOR_VSYNC_START;
    bool m_vsync = false, m_vblank = false;
    uint32_t m_currentFrameTotalLines = 0, m_currentFrameFinalLines = 0;
    uint64_t m_totalFrames = 0;
    uint32_t m_lineInState = 0;
    uint32_t m_vsyncLineCount = 0;
    uint32_t m_y = 0, m_lastY = 0;
    bool m_vsyncPending = false;
    uint32_t m_vsyncPendingLines = 0;
    uint32_t m_frameLines = FRAME_SIZE_NTSC;
    uint32_t m_height = BASE_HEIGHT_NTSC;
    uint32_t m_yStart = YSTART_NTSC;
};

}  // namespace tiacore
