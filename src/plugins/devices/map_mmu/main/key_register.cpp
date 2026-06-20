#include "key_register.h"
#include "libmem/main/ibus.h"

// Knock sequence lookup table: (first byte, second byte) -> personality mode
struct KnockSequence {
    uint8_t first;
    uint8_t second;
    IopersonalityMode mode;
};

static const KnockSequence KNOCK_SEQUENCES[] = {
    { 0x00, 0x00, IopersonalityMode::C64 },
    { 0xA5, 0x96, IopersonalityMode::C65 },
    { 0x47, 0x53, IopersonalityMode::MEGA65 },
    { 0x45, 0x54, IopersonalityMode::ETHERNET },
};

static const int KNOCK_SEQUENCE_COUNT = sizeof(KNOCK_SEQUENCES) / sizeof(KNOCK_SEQUENCES[0]);

KeyRegister::KeyRegister()
    : m_state(KeyState::WAITING_FIRST)
    , m_firstByte(0)
    , m_lastWritten(0)
    , m_currentPersonality(IopersonalityMode::C64)
{
}

KeyRegister::~KeyRegister()
{
}

bool KeyRegister::isValidSequence(uint8_t first, uint8_t second, IopersonalityMode& mode) const
{
    for (int i = 0; i < KNOCK_SEQUENCE_COUNT; ++i) {
        if (KNOCK_SEQUENCES[i].first == first && KNOCK_SEQUENCES[i].second == second) {
            mode = KNOCK_SEQUENCES[i].mode;
            return true;
        }
    }
    return false;
}

bool KeyRegister::ioRead(IBus* bus, uint32_t addr, uint8_t* val)
{
    if (addr != 0xD02F) return false;
    // $D02F read returns last written value (or 0 if nothing written)
    *val = m_lastWritten;
    return true;
}

bool KeyRegister::ioWrite(IBus* bus, uint32_t addr, uint8_t val)
{
    if (addr != 0xD02F) return false;
    m_lastWritten = val;

    // In hypervisor mode, KEY writes are accepted but do not change personality
    if (m_isHypervisor && m_isHypervisor()) {
        return true;
    }

    if (m_state == KeyState::WAITING_FIRST) {
        // Store first byte and wait for second
        m_firstByte = val;
        m_state = KeyState::WAITING_SECOND;
        return true;
    }

    // m_state == WAITING_SECOND
    // Check if (m_firstByte, val) is a valid sequence
    IopersonalityMode newMode;
    if (isValidSequence(m_firstByte, val, newMode)) {
        if (newMode != m_currentPersonality) {
            m_currentPersonality = newMode;
            if (m_personalityCallback) {
                m_personalityCallback(newMode);
            }
        }
    }

    // Reset state regardless of validity
    m_state = KeyState::WAITING_FIRST;
    return true;
}

void KeyRegister::reset()
{
    m_state = KeyState::WAITING_FIRST;
    m_firstByte = 0;
    m_lastWritten = 0;
    m_currentPersonality = IopersonalityMode::C64;
}

void KeyRegister::tick(uint64_t cycles)
{
    // No clocking needed for KEY register
}
