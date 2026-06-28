#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/isignal_line.h"
#include <cstdint>
#include <string>
#include <vector>

/**
 * POKEY (Potentiometer and Keyboard Encoder) for Atari 8-bit.
 * 
 * Handles audio synthesis (4 channels), timers, keyboard scanning, 
 * potentiometers, and serial I/O.
 */
class POKEY : public IOHandler, public IAudioOutput {
public:
    POKEY() : m_name("POKEY"), m_baseAddr(0xD200) { POKEY::reset(); }
    ~POKEY() override = default;

    void setClockHz(uint32_t hz) override  { m_clockHz = hz; }
    void setSampleRate(int hz)             { m_sampleRate = hz; }
    void setIrqLine(ISignalLine* line) override { m_irqLine = line; }

    // -----------------------------------------------------------------------
    // IOHandler interface
    // -----------------------------------------------------------------------

    const char* name()     const override { return m_name.c_str(); }
    void        setName(const std::string& name) override { m_name = name; }
    uint32_t    baseAddr() const override { return m_baseAddr; }
    uint32_t    addrMask() const override { return 0x000F; } // 16 registers

    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t  val) override;
    void reset()  override;
    void tick(uint64_t cycles) override;

    std::vector<std::string> deviceAliases() const override { return {"POKEY", "ATARI_POKEY"}; }

    // -----------------------------------------------------------------------
    // IAudioOutput interface
    // -----------------------------------------------------------------------

    int nativeSampleRate() const override { return m_sampleRate; }
    int pullSamples(float* buffer, int maxSamples) override;

    // -----------------------------------------------------------------------
    // Registers ($D200–$D20F)
    // -----------------------------------------------------------------------
    enum Reg {
        AUDF1   = 0x00, AUDC1 = 0x01,
        AUDF2   = 0x02, AUDC2 = 0x03,
        AUDF3   = 0x04, AUDC3 = 0x05,
        AUDF4   = 0x06, AUDC4 = 0x07,
        AUDCTL  = 0x08,
        STIMER  = 0x09, // Write: Start Timers; Read: ALLPOT
        SKRES   = 0x0A, // Write: Reset Ser; Read: KBCODE
        POTGO   = 0x0B, // Write: Start Pot; Read: RANDOM
        SEROUT  = 0x0D, // Write: Ser Out; Read: SERIN
        IRQEN   = 0x0E, // Write: IRQ Enable; Read: IRQST
        SKCTL   = 0x0F  // Write: Ser Ctrl; Read: SKSTAT
    };

    // AUDCTL bits
    static constexpr uint8_t CTL_POLY9   = 0x80; // 9-bit poly vs 17-bit poly
    static constexpr uint8_t CTL_CLK1_17 = 0x40; // Clock Ch1 at 1.79MHz
    static constexpr uint8_t CTL_CLK3_17 = 0x20; // Clock Ch3 at 1.79MHz
    static constexpr uint8_t CTL_CH12_16 = 0x10; // Join Ch1+Ch2
    static constexpr uint8_t CTL_CH34_16 = 0x08; // Join Ch3+Ch4
    static constexpr uint8_t CTL_FILT13  = 0x04; // Filter Ch1 with Ch3
    static constexpr uint8_t CTL_FILT24  = 0x02; // Filter Ch2 with Ch4
    static constexpr uint8_t CTL_CLK_15  = 0x01; // 15kHz vs 64kHz

private:
    struct Channel {
        uint32_t counter = 0;
        uint32_t divider = 0;
        bool     output  = false;
        
        uint8_t  audf = 0;
        uint8_t  audc = 0;
    };

    void synthesize(uint64_t cycles);
    void updatePoly();
    void updateIrqs();

    std::string m_name;
    uint32_t    m_baseAddr;
    ISignalLine* m_irqLine = nullptr;

    uint8_t     m_regs[16];
    Channel     m_channels[4];
    
    uint32_t    m_poly4  = 1;
    uint32_t    m_poly5  = 1;
    uint32_t    m_poly9  = 1;
    uint32_t    m_poly17 = 1;

    uint32_t    m_clockHz    = 1789773; // NTSC Atari
    int         m_sampleRate = 44100;
    uint64_t    m_lastCycles = 0;
    uint64_t    m_sampleFrac = 0;

    // Interrupt state
    uint8_t     m_irqst = 0xFF; // Active low bits
    uint8_t     m_irqen = 0x00;

    // Keyboard state
    uint8_t     m_kbcode = 0xFF;
    uint8_t     m_skstat = 0xFF;

    // Ring buffer
    static constexpr int AUDIO_BUF = 8192;
    float   m_audioBuf[AUDIO_BUF] = {};
    int     m_audioBufRead  = 0;
    int     m_audioBufWrite = 0;
    int     m_audioBufCount = 0;
};
