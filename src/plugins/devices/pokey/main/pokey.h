#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/isignal_line.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * POKEY (Potentiometer and Keyboard Integrated Circuit), Atari 8-bit.
 *
 * Clocked once per CPU cycle (1.79 MHz NTSC):
 *   - Four audio channels on the 64 kHz or 15 kHz base clock (AUDCTL bit 0)
 *     or 1.79 MHz (channels 1 and 3), joinable into two 16-bit channels,
 *     with the 4/5/9/17-bit polynomial distortions, volume-only mode and the
 *     high-pass filters (channel 1 by 3, 2 by 4)
 *   - Timers: channels 1, 2 and 4 raise IRQs on underflow (IRQEN/IRQST)
 *   - Keyboard: KBCODE, the key IRQ, SKSTAT key-down and shift; BREAK IRQ
 *   - Serial output: SEROUT bytes are shifted out at channel 4's rate, with
 *     the "output needed" and "transmission done" IRQs, and passed to
 *     setSerialOut(); serial input bytes come in through receiveSerial()
 *   - RANDOM from the 17- or 9-bit polynomial; pots read as 228
 */
class POKEY : public IOHandler, public IAudioOutput {
public:
    POKEY() : m_name("POKEY"), m_baseAddr(0xD200) { POKEY::reset(); }
    ~POKEY() override = default;

    void setClockHz(uint32_t hz) override  { m_clockHz = hz; }
    void setSampleRate(int hz)             { m_sampleRate = hz; }
    void setIrqLine(ISignalLine* line) override { m_irqLine = line; }

    // IOHandler
    const char* name()     const override { return m_name.c_str(); }
    void        setName(const std::string& name) override { m_name = name; }
    uint32_t    baseAddr() const override { return m_baseAddr; }
    uint32_t    addrMask() const override { return 0x000F; }
    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t  val) override;
    void reset()  override;
    void tick(uint64_t cycles) override;
    std::vector<std::string> deviceAliases() const override { return {"POKEY", "ATARI_POKEY"}; }

    // IAudioOutput
    int nativeSampleRate() const override { return m_sampleRate; }
    int pullSamples(float* buffer, int maxSamples) override;

    // -----------------------------------------------------------------------
    // Keyboard
    // -----------------------------------------------------------------------
    /** A key press: the 6-bit key code plus $40 shift, $80 control. */
    void keyDown(uint8_t code);
    /** All keys released. */
    void keyUp();
    /** SHIFT held (SKSTAT bit 3), independent of other keys. */
    void setShift(bool down);
    /** The BREAK key (IRQ bit 7). */
    void breakKey();

    // -----------------------------------------------------------------------
    // Serial port (SIO)
    // -----------------------------------------------------------------------
    /** Called with each byte shifted out of SEROUT. */
    void setSerialOut(std::function<void(uint8_t)> cb) { m_serialOut = std::move(cb); }
    /** A byte arriving on the serial input: SERIN and the "input ready" IRQ. */
    void receiveSerial(uint8_t byte);

    enum Reg {
        AUDF1 = 0x00, AUDC1 = 0x01, AUDF2 = 0x02, AUDC2 = 0x03,
        AUDF3 = 0x04, AUDC3 = 0x05, AUDF4 = 0x06, AUDC4 = 0x07,
        AUDCTL = 0x08,
        STIMER = 0x09,   // read: KBCODE
        SKRES  = 0x0A,   // read: RANDOM
        POTGO  = 0x0B,
        SEROUT = 0x0D,   // read: SERIN
        IRQEN  = 0x0E,   // read: IRQST
        SKCTL  = 0x0F    // read: SKSTAT
    };
    // Read addresses
    static constexpr int ALLPOT = 0x08, KBCODE = 0x09, RANDOM = 0x0A,
                         SERIN = 0x0D, IRQST = 0x0E, SKSTAT = 0x0F;

    // AUDCTL bits
    static constexpr uint8_t CTL_POLY9   = 0x80;
    static constexpr uint8_t CTL_CLK1_17 = 0x40;
    static constexpr uint8_t CTL_CLK3_17 = 0x20;
    static constexpr uint8_t CTL_CH12_16 = 0x10;
    static constexpr uint8_t CTL_CH34_16 = 0x08;
    static constexpr uint8_t CTL_FILT13  = 0x04;
    static constexpr uint8_t CTL_FILT24  = 0x02;
    static constexpr uint8_t CTL_CLK_15  = 0x01;

    // IRQ bits (IRQEN / IRQST, IRQST active low)
    static constexpr uint8_t IRQ_TIMER1  = 0x01;
    static constexpr uint8_t IRQ_TIMER2  = 0x02;
    static constexpr uint8_t IRQ_TIMER4  = 0x04;
    static constexpr uint8_t IRQ_SERDONE = 0x08;   ///< serial transmission finished (a level)
    static constexpr uint8_t IRQ_SEROUT  = 0x10;   ///< serial output data needed
    static constexpr uint8_t IRQ_SERIN   = 0x20;   ///< serial input data ready
    static constexpr uint8_t IRQ_KEY     = 0x40;
    static constexpr uint8_t IRQ_BREAK   = 0x80;

private:
    struct Channel {
        int  counter = 1;
        bool output = false;
    };
    void clockPolys();
    void underflow(int ch);
    void reloadChannel(int ch);
    void raiseIrq(uint8_t bit);
    void updateIrqs();
    void serialBit();
    float mixOutput() const;

    std::string  m_name;
    uint32_t     m_baseAddr;
    ISignalLine* m_irqLine = nullptr;

    uint8_t  m_regs[16] = {};
    Channel  m_ch[4];
    bool     m_filterLatch[2] = {false, false};   // high-pass flip-flops (ch1, ch2)
    int      m_div28 = 0, m_div114 = 0;           // 64 kHz and 15 kHz base clocks

    uint32_t m_poly4 = 0x0F, m_poly5 = 0x1F, m_poly9 = 0x1FF, m_poly17 = 0x1FFFF;

    uint8_t  m_irqst = 0xFF;
    uint8_t  m_irqen = 0;
    uint8_t  m_kbcode = 0xFF;
    uint8_t  m_skstat = 0xFF;
    uint8_t  m_serin = 0xFF;

    // Serial transmitter: shift register and its holding buffer
    bool     m_txBusy = false;
    int      m_txBits = 0;       // bits left in the frame (10: start, 8 data, stop)
    uint8_t  m_txShift = 0;
    bool     m_txBufFull = false;
    uint8_t  m_txBuf = 0;
    bool     m_txHalf = false;   // a bit is two channel 4 underflows
    std::function<void(uint8_t)> m_serialOut;

    // Audio
    uint32_t m_clockHz = 1789773;
    int      m_sampleRate = 44100;
    uint64_t m_sampleFrac = 0;
    double   m_sampleSum = 0.0;
    uint32_t m_sampleCycles = 0;
    float    m_dcBlock = 0.0f;   // removes POKEY's unipolar output's DC
    static constexpr int AUDIO_BUF = 8192;
    float    m_audioBuf[AUDIO_BUF] = {};
    int      m_audioBufRead = 0, m_audioBufWrite = 0, m_audioBufCount = 0;
};
