#pragma once

#include "io_handler.h"
#include <cstdint>
#include <functional>

// I/O personality modes
enum class IopersonalityMode {
    C64 = 0,
    C65 = 1,
    MEGA65 = 2,
    ETHERNET = 3
};

// KEY register at $D02F - I/O personality knock sequence handler
// Uses a two-byte knock sequence to select personality:
// $00 $00 -> C64
// $A5 $96 -> C65
// $47 $53 -> MEGA65
// $45 $54 -> Ethernet
class KeyRegister : public IOHandler {
public:
    KeyRegister();
    virtual ~KeyRegister();

    const char* name()     const override { return "KEY"; }
    uint32_t    baseAddr() const override { return 0xD02F; }
    uint32_t    addrMask() const override { return 0x0000; }

    bool ioRead(IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override;
    void reset() override;
    void tick(uint64_t cycles) override;

    // Callback when personality changes
    void setPersonalityChangeCallback(std::function<void(IopersonalityMode)> cb) {
        m_personalityCallback = cb;
    }

    IopersonalityMode getCurrentPersonality() const { return m_currentPersonality; }

private:
    enum class KeyState { WAITING_FIRST, WAITING_SECOND };

    KeyState m_state;
    uint8_t m_firstByte;
    uint8_t m_lastWritten;
    IopersonalityMode m_currentPersonality;
    std::function<void(IopersonalityMode)> m_personalityCallback;

    // Check if (first, second) forms a valid knock sequence
    bool isValidSequence(uint8_t first, uint8_t second, IopersonalityMode& mode) const;
};
