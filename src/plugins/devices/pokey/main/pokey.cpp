#include "pokey.h"
#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

bool POKEY::ioRead(IBus* /*bus*/, uint32_t addr, uint8_t* val) {
    uint32_t r = addr & 0x0F;
    if (r < 8) { *val = 228; return true; }   // POT0-7: paddles at the end of their travel
    switch (r) {
        case ALLPOT: *val = 0x00; break;      // all pot scans finished
        case KBCODE: *val = m_kbcode; break;
        case RANDOM:
            *val = (m_regs[SKCTL] & 0x03) == 0 ? 0xFF
                 : (m_regs[AUDCTL] & CTL_POLY9) ? (uint8_t)(m_poly9 & 0xFF)
                                                : (uint8_t)((m_poly17 >> 9) & 0xFF);
            break;
        case SERIN:  *val = m_serin; break;
        case IRQST: {
            uint8_t st = m_irqst;
            if (!m_txBusy && (m_irqen & IRQ_SERDONE)) st &= (uint8_t)~IRQ_SERDONE;
            else st |= IRQ_SERDONE;
            *val = st;
            break;
        }
        case SKSTAT: *val = m_skstat; break;
        default:     *val = 0xFF; break;
    }
    return true;
}

bool POKEY::ioWrite(IBus* /*bus*/, uint32_t addr, uint8_t val) {
    uint32_t r = addr & 0x0F;
    m_regs[r] = val;
    switch (r) {
        case STIMER:
            for (int ch = 0; ch < 4; ++ch) {
                reloadChannel(ch);
                m_ch[ch].output = false;
            }
            break;
        case SKRES:
            m_skstat |= 0xE0;   // clear framing, overrun and keyboard overrun errors
            break;
        case SEROUT:
            if (!m_txBusy) {
                // Idle: the byte moves into the shift register at the next bit
                // boundary, and only then is "output needed" raised (the OS
                // stores its checksum after this write, before that IRQ)
                m_txBusy = true;
                m_txLoading = true;
                m_txBuf = val;
                m_txBufFull = true;
                m_txHalf = false;
            } else {
                m_txBuf = val;
                m_txBufFull = true;
            }
            updateIrqs();
            break;
        case IRQEN:
            m_irqen = val;
            m_irqst |= (uint8_t)~val;   // disabled interrupts are cleared
            updateIrqs();
            break;
        case SKCTL:
            if ((val & 0x03) == 0) {    // reset/initialisation mode
                m_poly4 = 0x0F; m_poly5 = 0x1F; m_poly9 = 0x1FF; m_poly17 = 0x1FFFF;
            }
            break;
        default:
            break;
    }
    return true;
}

void POKEY::reset() {
    std::memset(m_regs, 0, sizeof(m_regs));
    for (auto& c : m_ch) c = Channel();
    m_filterLatch[0] = m_filterLatch[1] = false;
    m_div28 = m_div114 = 0;
    m_poly4 = 0x0F; m_poly5 = 0x1F; m_poly9 = 0x1FF; m_poly17 = 0x1FFFF;
    m_irqst = 0xFF;
    m_irqen = 0;
    m_kbcode = 0xFF;
    m_skstat = 0xFF;
    m_serin = 0xFF;
    m_txBusy = m_txBufFull = m_txHalf = m_txLoading = false;
    m_txBits = 0;
    m_sampleFrac = 0;
    m_sampleSum = 0.0;
    m_sampleCycles = 0;
    m_dcBlock = 0.0f;
    m_audioBufRead = m_audioBufWrite = m_audioBufCount = 0;
    updateIrqs();
}

// ---------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------

void POKEY::raiseIrq(uint8_t bit) {
    if (m_irqen & bit) {
        m_irqst &= (uint8_t)~bit;
        updateIrqs();
    }
}

void POKEY::updateIrqs() {
    if (!m_irqLine) return;
    uint8_t pending = (uint8_t)(~m_irqst & m_irqen);
    if (!m_txBusy && (m_irqen & IRQ_SERDONE)) pending |= IRQ_SERDONE;
    m_irqLine->set(pending != 0);
}

// ---------------------------------------------------------------------------
// Keyboard and serial port
// ---------------------------------------------------------------------------

void POKEY::keyDown(uint8_t code) {
    if (!(m_regs[SKCTL] & 0x02)) return;   // keyboard scanning off
    m_kbcode = code;
    m_skstat &= (uint8_t)~0x04;
    raiseIrq(IRQ_KEY);
}

void POKEY::keyUp() {
    m_skstat |= 0x04;
}

void POKEY::setShift(bool down) {
    if (down) m_skstat &= (uint8_t)~0x08; else m_skstat |= 0x08;
}

void POKEY::breakKey() {
    raiseIrq(IRQ_BREAK);
}

void POKEY::receiveSerial(uint8_t byte) {
    m_serin = byte;
    raiseIrq(IRQ_SERIN);
}

void POKEY::serialBit() {
    if (!m_txBusy) return;
    m_txHalf = !m_txHalf;
    if (m_txHalf) return;                  // two channel 4 underflows a bit
    if (m_txLoading) {                     // the first byte reaches the shift register
        m_txLoading = false;
        m_txShift = m_txBuf;
        m_txBufFull = false;
        m_txBits = 10;
        raiseIrq(IRQ_SEROUT);
        return;
    }
    if (--m_txBits > 0) return;
    if (m_serialOut) m_serialOut(m_txShift);
    if (m_txBufFull) {
        m_txShift = m_txBuf;
        m_txBufFull = false;
        m_txBits = 10;
        raiseIrq(IRQ_SEROUT);
    } else {
        m_txBusy = false;
    }
    updateIrqs();
}

// ---------------------------------------------------------------------------
// Audio and timers
// ---------------------------------------------------------------------------

void POKEY::reloadChannel(int ch) {
    uint8_t ctl = m_regs[AUDCTL];
    switch (ch) {
        case 0: m_ch[0].counter = m_regs[AUDF1] + ((ctl & CTL_CLK1_17) ? 4 : 1); break;
        case 1:
            m_ch[1].counter = (ctl & CTL_CH12_16)
                ? m_regs[AUDF1] + m_regs[AUDF2] * 256 + ((ctl & CTL_CLK1_17) ? 7 : 1)
                : m_regs[AUDF2] + 1;
            break;
        case 2: m_ch[2].counter = m_regs[AUDF3] + ((ctl & CTL_CLK3_17) ? 4 : 1); break;
        default:
            m_ch[3].counter = (ctl & CTL_CH34_16)
                ? m_regs[AUDF3] + m_regs[AUDF4] * 256 + ((ctl & CTL_CLK3_17) ? 7 : 1)
                : m_regs[AUDF4] + 1;
            break;
    }
}

void POKEY::clockPolys() {
    uint32_t b4 = ((m_poly4 >> 3) ^ (m_poly4 >> 2)) & 1;
    m_poly4 = ((m_poly4 << 1) | b4) & 0x0F;
    uint32_t b5 = ((m_poly5 >> 4) ^ (m_poly5 >> 2)) & 1;
    m_poly5 = ((m_poly5 << 1) | b5) & 0x1F;
    uint32_t b9 = ((m_poly9 >> 8) ^ (m_poly9 >> 4)) & 1;
    m_poly9 = ((m_poly9 << 1) | b9) & 0x1FF;
    uint32_t b17 = ((m_poly17 >> 16) ^ (m_poly17 >> 11)) & 1;
    m_poly17 = ((m_poly17 << 1) | b17) & 0x1FFFF;
}

void POKEY::underflow(int ch) {
    if (ch == 0) raiseIrq(IRQ_TIMER1);
    else if (ch == 1) raiseIrq(IRQ_TIMER2);
    else if (ch == 3) raiseIrq(IRQ_TIMER4);

    // Distortion: the 5-bit poly gates changes, then pure tone, 4-bit or 17/9-bit poly
    uint8_t audc = m_regs[AUDC1 + ch * 2];
    if ((audc & 0x80) || (m_poly5 & 1)) {
        Channel& c = m_ch[ch];
        if (audc & 0x20)      c.output = !c.output;
        else if (audc & 0x40) c.output = m_poly4 & 1;
        else                  c.output = ((m_regs[AUDCTL] & CTL_POLY9) ? m_poly9 : m_poly17) & 1;
    }

    // High-pass filters: channel 3 samples channel 1, channel 4 samples channel 2
    if (ch == 2 && (m_regs[AUDCTL] & CTL_FILT13)) m_filterLatch[0] = m_ch[0].output;
    if (ch == 3) {
        if (m_regs[AUDCTL] & CTL_FILT24) m_filterLatch[1] = m_ch[1].output;
        serialBit();
    }
}

float POKEY::mixOutput() const {
    int sum = 0;
    for (int ch = 0; ch < 4; ++ch) {
        uint8_t audc = m_regs[AUDC1 + ch * 2];
        int vol = audc & 0x0F;
        if (!vol) continue;
        bool out = m_ch[ch].output;
        if (ch == 0 && (m_regs[AUDCTL] & CTL_FILT13)) out = out != m_filterLatch[0];
        if (ch == 1 && (m_regs[AUDCTL] & CTL_FILT24)) out = out != m_filterLatch[1];
        if ((audc & 0x10) || out) sum += vol;   // volume-only: the level itself
    }
    return (float)sum / 60.0f;
}

void POKEY::step() {
    const uint8_t ctl = m_regs[AUDCTL];
    const bool running = (m_regs[SKCTL] & 0x03) != 0;
    {
        if (running) clockPolys();

        bool tick64 = ++m_div28 >= 28;
        if (tick64) m_div28 = 0;
        bool tick15 = ++m_div114 >= 114;
        if (tick15) m_div114 = 0;
        bool base = (ctl & CTL_CLK_15) ? tick15 : tick64;

        // Channels 1 and 2 (joined: one 16-bit counter on channel 1's clock)
        bool clk1 = (ctl & CTL_CLK1_17) || base;
        if (ctl & CTL_CH12_16) {
            if (clk1 && --m_ch[1].counter <= 0) { reloadChannel(1); underflow(1); }
        } else {
            if (clk1 && --m_ch[0].counter <= 0) { reloadChannel(0); underflow(0); }
            if (base && --m_ch[1].counter <= 0) { reloadChannel(1); underflow(1); }
        }
        // Channels 3 and 4
        bool clk3 = (ctl & CTL_CLK3_17) || base;
        if (ctl & CTL_CH34_16) {
            if (clk3 && --m_ch[3].counter <= 0) { reloadChannel(3); underflow(3); }
        } else {
            if (clk3 && --m_ch[2].counter <= 0) { reloadChannel(2); underflow(2); }
            if (base && --m_ch[3].counter <= 0) { reloadChannel(3); underflow(3); }
        }
    }
}

void POKEY::tick(uint64_t cycles) {
    if (m_slave) return;                    // the first POKEY clocks this one
    for (uint64_t i = 0; i < cycles; ++i) {
        step();
        float level = mixOutput();
        if (m_second) {
            m_second->step();
            level = (level + m_second->mixOutput()) * 0.5f;
        }
        // Audio: average the cycles of each output sample
        m_sampleSum += level;
        ++m_sampleCycles;
        m_sampleFrac += (uint32_t)m_sampleRate;
        if (m_sampleFrac >= m_clockHz) {
            m_sampleFrac -= m_clockHz;
            float s = (float)(m_sampleSum / m_sampleCycles);
            m_sampleSum = 0.0;
            m_sampleCycles = 0;
            m_dcBlock += (s - m_dcBlock) * 0.002f;   // ~14 Hz high-pass
            if (m_audioBufCount < AUDIO_BUF) {
                m_audioBuf[m_audioBufWrite] = std::max(-1.0f, std::min(1.0f, s - m_dcBlock));
                m_audioBufWrite = (m_audioBufWrite + 1) % AUDIO_BUF;
                ++m_audioBufCount;
            }
        }
    }
}

int POKEY::pullSamples(float* buffer, int maxSamples) {
    int n = std::min(maxSamples, m_audioBufCount);
    for (int i = 0; i < n; ++i) {
        buffer[i] = m_audioBuf[m_audioBufRead];
        m_audioBufRead = (m_audioBufRead + 1) % AUDIO_BUF;
    }
    m_audioBufCount -= n;
    return n;
}
