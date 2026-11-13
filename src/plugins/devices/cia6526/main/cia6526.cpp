#include <cstdlib>
#include "cia6526.h"
#include "include/util/logging.h"
#include <algorithm>
#include <cstring>
#include <sstream>
#include <vector>

// The old 6526's interrupt flag pipeline (VICE ciacore.c ifr_delay): each
// cycle the bits move one place left; ..0 is "this cycle"
static constexpr uint32_t IFR_ACK1   = 0x0001;  // countdown to clearing IR after a read
static constexpr uint32_t IFR_ACK0   = 0x0002;
static constexpr uint32_t IFR_ACK_1  = 0x0004;
static constexpr uint32_t IFR_ACK_2  = 0x0008;
static constexpr uint32_t IFR_D7SET1 = 0x0010;  // countdown to setting IR (bit 7)
static constexpr uint32_t IFR_D7SET0 = 0x0020;
static constexpr uint32_t IFR_D7SET_1 = 0x0040;
static constexpr uint32_t IFR_RAISE1 = 0x0100;  // countdown to raising the IRQ output
static constexpr uint32_t IFR_RAISE0 = 0x0200;
static constexpr uint32_t IFR_RAISE_1 = 0x0400;
static constexpr uint32_t IFR_READ0  = 0x1000;  // cycles since an ICR read
static constexpr uint32_t IFR_READ1  = 0x2000;
static constexpr uint32_t IFR_READ2  = 0x4000;
static constexpr uint32_t IFR_CLEAR  = IFR_ACK_2 | IFR_D7SET_1 | IFR_RAISE_1 | IFR_READ2;
static constexpr uint32_t IRQ_TBB    = 0x100;   // the timer B bug: drop TB on the next read

// The serial port's pipeline (VICE ciacore.c sdr_delay): ..0 is "this cycle";
// each cycle the bits move one place left
static constexpr uint32_t SDR_TOGGLE_CNT1 = 0x0002;   // CNT toggles
static constexpr uint32_t SDR_TOGGLE_CNT0 = 0x0004;
static constexpr uint32_t SDR_TOGGLE_CNT_1 = 0x0008;
static constexpr uint32_t SDR_NOGGLE_CNT1 = 0x0020;   // a timer A underflow too close to toggle
static constexpr uint32_t SDR_NOGGLE_CNT0 = 0x0040;
static constexpr uint32_t SDR_NOGGLE_CNT_1 = 0x0080;
static constexpr uint32_t SDR_SET_IRQ2  = 0x0200;     // the SP interrupt flag
static constexpr uint32_t SDR_SET_IRQ0  = 0x0800;
static constexpr uint32_t SDR_CNT0      = 0x1000;     // CNT's history
static constexpr uint32_t SDR_CNT1      = 0x2000;
static constexpr uint32_t SDR_CNT2      = 0x4000;
static constexpr uint32_t SDR_CNT3      = 0x8000;
static constexpr uint32_t SDR_SET1      = 0x00040000; // the shifter loads from SDR
static constexpr uint32_t SDR_SET0      = 0x00080000;
static constexpr uint32_t SDR_CLEAR     = 0x0010 | 0x0100 | SDR_CNT0 | 0x00010000 | 0x00100000;
static constexpr uint32_t SDR_ACTIVE    = 0x000F00FF | 0x0F00;
static constexpr uint32_t SDR_ALL_CNT   = SDR_CNT0 | SDR_CNT1 | SDR_CNT2 | SDR_CNT3;
static constexpr uint32_t SDR_ALL_TOGGLE = 0x000F;
static constexpr uint32_t SDR_ALL_NOGGLE = 0x00F0;
static constexpr uint8_t  CRA_SPMODE_OUT = 0x40;
#include <iomanip>

/** "cia" logger: timer starts at debug; ICR access and underflows at trace. */
static spdlog::logger* ciaLog() {
    static auto logger = LogRegistry::instance().getLogger("cia");
    return logger.get();
}

// Helper function to convert values to hex strings
static std::string toHex(uint16_t v, int width = 4) {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0') << std::setw(width) << v;
    return ss.str();
}

static std::string toHex(uint8_t v, int width = 2) {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0') << std::setw(width) << (int)v;
    return ss.str();
}

CIA6526::CIA6526(const std::string& name, uint32_t baseAddr)
    : m_name(name), m_baseAddr(baseAddr)
{
    CIA6526::reset();
}

bool CIA6526::setOption(const std::string& key, const std::string& value, std::string& err) {
    std::string v;
    for (char c : value) v += (char)toupper((unsigned char)c);
    if (key == "decode") {
        // The address window it answers in, registers repeating every 16
        // bytes: the C64 decodes a CIA through its whole page ("0xFF")
        char* end = nullptr;
        const unsigned long m = std::strtoul(value.c_str(), &end, 0);
        if (!end || *end || m < 0x0F || ((m + 1) & m)) { err = "decode is a window mask: 0x0F, 0x1F ... 0xFF"; return false; }
        m_decodeMask = (uint32_t)m;
        return true;
    }
    if (key != "model") { err = "the CIA's options are its model (6526, 6526A, mega65) and decode"; return false; }
    if (v == "6526" || v == "OLD")                        m_newModel = false, m_instantSdr = false;
    else if (v == "6526A" || v == "8521" || v == "NEW")   m_newModel = true, m_instantSdr = false;
    else if (v == "MEGA65")                               m_newModel = true, m_instantSdr = true;
    else { err = "a CIA is a 6526 (old), a 6526A (new) or mega65"; return false; }
    return true;
}

void CIA6526::reset() {
    m_pra  = 0xFF;
    m_prb  = 0xFF;
    m_ddra = 0x00;
    m_ddrb = 0x00;

    m_ta = Timer{};
    m_tb = Timer{};
    m_tat = m_tbt = 0;

    m_icrMask  = 0x00;
    m_irqFlags = 0;
    m_newFlags = 0;
    m_ackFlags = 0;
    m_sdrDelay = SDR_ALL_CNT;
    m_shifter = 0;
    m_srBits = 0;
    m_sdrValid = m_sdrForceFinish = m_sdrActive = false;
    m_cntOut = true;
    m_ifrDelay = 0;
    m_irqOut   = false;
    m_clk      = 0;
    m_rdi      = ~0ull;
    m_quiet    = 0;

    m_cra = 0x00;
    m_crb = 0x00;

    m_todTen = 0x00;
    m_todSec = 0x00;
    m_todMin = 0x00;
    m_todHr  = 0x01;

    m_alarmTen = 0x00;
    m_alarmSec = 0x00;
    m_alarmMin = 0x00;
    m_alarmHr  = 0x00;

    m_todLatched = false;
    m_latchTen   = 0x00;
    m_latchSec   = 0x00;
    m_latchMin   = 0x00;
    m_latchHr    = 0x01;

    m_todStopped = true;
    m_todTickCounter = 0;
    m_todCountdown = m_todTicks ? m_todTicks : 1;

    if (m_irqLine) m_irqLine->set(false);
    if (m_portADevice) m_portADevice->setDdr(0x00);
    if (m_portBDevice) m_portBDevice->setDdr(0x00);
    if (m_portAWriteCallback) m_portAWriteCallback(m_pra, m_ddra);
}

// ---------------------------------------------------------------------------
// Address decode
// ---------------------------------------------------------------------------

bool CIA6526::ioRead(IBus* /*bus*/, uint32_t addr, uint8_t* val) { return readReg(addr, val, false); }
bool CIA6526::ioPeek(IBus* /*bus*/, uint32_t addr, uint8_t* val) { return readReg(addr, val, true); }

// A register read; `peek` returns the same value without its side effects
// (ICR clear and IRQ release, TOD latch and release, port device reads).
bool CIA6526::readReg(uint32_t addr, uint8_t* val, bool peek) {
    if ((addr & ~m_decodeMask) != m_baseAddr) return false;

    uint8_t reg = addr & 0x0F;
    switch (reg) {
        case PRA: {
            // Output bits from latch; input bits from port device (pins).
            *val = m_pra & m_ddra; // output bits
            if (m_portADevice) {
                uint8_t pins = peek ? m_portADevice->peekPort() : m_portADevice->readPort();
                *val |= (pins & ~m_ddra); // input bits
                if (m_outputsReadPins) *val &= pins | ~m_ddra; // a device pulls an output low
            } else {
                *val |= (~m_ddra & 0xFF); // unconnected pins float high
            }
            break;
        }
        case PRB: {
            *val = m_prb & m_ddrb;
            if (m_portBDevice) {
                uint8_t pins = peek ? m_portBDevice->peekPort() : m_portBDevice->readPort();
                *val |= (pins & ~m_ddrb);
                if (m_outputsReadPins) *val &= pins | ~m_ddrb;
            } else {
                *val |= (~m_ddrb & 0xFF);
            }
            *val = pb67(*val);
            break;
        }
        case DDRA: *val = m_ddra; break;
        case DDRB: *val = m_ddrb; break;

        case TALO: *val = m_ta.cnt & 0xFF; break;
        case TAHI: *val = (m_ta.cnt >> 8) & 0xFF; break;

        case TBLO: *val = m_tb.cnt & 0xFF; break;
        case TBHI: *val = (m_tb.cnt >> 8) & 0xFF; break;

        // TOD reads: reading TODHR latches all TOD registers; reading TODTEN
        // releases the latch (the clock keeps running)
        case TODTEN: case TODSEC: case TODMIN: case TODHR: {
            if (!m_todLatched) {
                m_latchTen = m_todTen; m_latchSec = m_todSec;
                m_latchMin = m_todMin; m_latchHr = m_todHr;
            }
            const uint8_t latch[4] = { m_latchTen, m_latchSec, m_latchMin, m_latchHr };
            const uint8_t live[4]  = { m_todTen, m_todSec, m_todMin, m_todHr };
            *val = (m_todLatched || !peek ? latch : live)[reg - TODTEN];
            if (!peek && reg == TODTEN) m_todLatched = false;
            if (!peek && reg == TODHR)  m_todLatched = true;
            break;
        }

        case SDR: *val = m_sdr; break;

        case ICR: {
            // The flags; bit 7 as the pipeline has set it so far
            if (peek) { *val = (uint8_t)m_irqFlags; break; }
            // The old 6526 (VICE's cia_read_ for CIA_MODEL_6526): the flags
            // clear at once, bit 7 two cycles on; a raise one cycle away is
            // cancelled; the timer B bug drops a flag set just after a read
            // The serial port sets its flag after a read in the same cycle
            // (VICE runs ciacore_intsdr after the access): the read neither
            // sees nor clears it
            const bool spAfter = m_spClk == m_clk && !(m_spPrior & ICR_SP);
            uint32_t spIntBefore = 0;
            if (spAfter) {
                spIntBefore = m_spPrior & ICR_INT;
                m_irqFlags &= ~ICR_SP;
                if (!spIntBefore && !(m_irqFlags & m_icrMask & 0x1F)) m_irqFlags &= ~ICR_INT;
            }
            m_rdi = m_clk;
            m_quiet = 0;
            if (m_irqFlags & IRQ_TBB) m_irqFlags &= ~(IRQ_TBB | ICR_TB);
            if (m_newModel) {
                // The 6526A: bit 7 of a raise due this cycle reads set; the
                // flags read clear a cycle later (VICE's ack_irqflags)
                if ((m_ifrDelay & IFR_RAISE0) && (m_irqFlags & 0x1F)) m_irqFlags |= ICR_INT;
                if (m_irqFlags & 0x9F) m_ackFlags |= (m_irqFlags & 0x9F) | ICR_INT;
                m_ifrDelay |= IFR_ACK1;
                m_ifrDelay &= ~(IFR_RAISE0 | IFR_D7SET0);
                *val = (uint8_t)m_irqFlags;
            } else {
                m_ifrDelay |= IFR_ACK1;
                m_ifrDelay &= ~IFR_RAISE0;
                *val = (uint8_t)m_irqFlags;
                m_irqFlags &= ICR_INT;
                m_newFlags = 0;
            }
            m_ifrDelay |= IFR_READ0;
            setInt(false);
            if (spAfter) {
                m_irqFlags |= ICR_SP;
                m_ackFlags &= ~ICR_SP;
                if (m_icrMask & ICR_SP) {
                    if (m_newModel) {                   // raised this cycle
                        m_irqFlags |= ICR_INT;
                        setInt(true);
                    } else {                            // next cycle
                        m_ifrDelay |= IFR_RAISE0 | IFR_D7SET0;
                    }
                }
            }
            ciaLog()->trace("{} ICR read -> ${:02X}", m_name, *val);
            break;
        }
        case CRA: *val = (m_cra & ~CR_START) | (m_ta.state & T_START); break;
        case CRB: *val = (m_crb & ~CR_START) | (m_tb.state & T_START); break;

        default: *val = 0xFF; break;
    }
    return true;
}

bool CIA6526::ioWrite(IBus* /*bus*/, uint32_t addr, uint8_t val) {
    if ((addr & ~m_decodeMask) != m_baseAddr) return false;
    m_quiet = 0;                                // the timers may change

    uint8_t reg = addr & 0x0F;
    switch (reg) {
        case PRA:
            m_pra = val;
            if (m_portADevice) {
                m_portADevice->writePort(val);
            }
            if (m_portAWriteCallback) m_portAWriteCallback(m_pra, m_ddra);
            break;
        case PRB:
            m_prb = val;
            if (m_portBDevice) m_portBDevice->writePort(val);
            break;
        case DDRA:
            m_ddra = val;
            if (m_portADevice) m_portADevice->setDdr(val);
            if (m_portAWriteCallback) m_portAWriteCallback(m_pra, m_ddra);
            break;
        case DDRB:
            m_ddrb = val;
            if (m_portBDevice) m_portBDevice->setDdr(val);
            break;

        // Latch writes (VICE ciat_set_latchlo/hi): a load in progress takes
        // the new byte; the high byte loads a stopped counter
        case TALO:
            m_ta.latch = (m_ta.latch & 0xFF00) | val;
            if (m_ta.state & T_LOAD) m_ta.cnt = (m_ta.cnt & 0xFF00) | val;
            break;
        case TAHI:
            m_ta.latch = (m_ta.latch & 0x00FF) | ((uint16_t)val << 8);
            if ((m_ta.state & T_LOAD) || !(m_ta.state & T_START)) m_ta.cnt = m_ta.latch;
            break;
        case TBLO:
            m_tb.latch = (m_tb.latch & 0xFF00) | val;
            if (m_tb.state & T_LOAD) m_tb.cnt = (m_tb.cnt & 0xFF00) | val;
            break;
        case TBHI:
            m_tb.latch = (m_tb.latch & 0x00FF) | ((uint16_t)val << 8);
            if ((m_tb.state & T_LOAD) || !(m_tb.state & T_START)) m_tb.cnt = m_tb.latch;
            break;

        // TOD / alarm writes (CRB bit 7 picks the alarm). VICE cia_store
        case TODTEN: case TODSEC: case TODMIN: case TODHR: {
            if (reg == TODHR) {
                val &= 0x9F;
                // Writing hour 12 to the clock flips AM/PM
                if ((val & 0x1F) == 0x12 && !(m_crb & CRB_ALARM)) val ^= 0x80;
            } else if (reg == TODTEN) {
                val &= 0x0F;
            } else {
                val &= 0x7F;
            }
            uint8_t* alarm[4] = { &m_alarmTen, &m_alarmSec, &m_alarmMin, &m_alarmHr };
            uint8_t* tod[4]   = { &m_todTen, &m_todSec, &m_todMin, &m_todHr };
            uint8_t* r = (m_crb & CRB_ALARM ? alarm : tod)[reg - TODTEN];
            if (!(m_crb & CRB_ALARM)) {
                if (reg == TODTEN && m_todStopped) {   // tenths restarts it
                    m_todTickCounter = 0;
                    m_todStopped = false;
                }
                if (reg == TODHR) m_todStopped = true;  // hours stops it
            }
            if (*r != val) {
                *r = val;
                checkTodAlarm();
            }
            break;
        }

        case SDR:
            // Serial data register.  When CRA bit 6 is set (output mode),
            // writing triggers a serial shift.  With no physical IEC device,
            // the shift "completes" immediately — set ICR bit 3 (SP flag).
            // This matches xemu's CIA_OUT_SDR_SETS_ICR_BIT3 behavior and
            // prevents the ROM's IEC serial polling loop from hanging
            // (see mega65-rom-public#169).
            // The ROM polls $DC0D for completion; an enabled SP interrupt
            // is raised as for any other source.
            m_sdr = val;
            if (!(m_cra & CRA_SPMODE_OUT)) break;
            if (m_instantSdr) {
                setIrqFlag(ICR_SP);
                if (m_serialOut) m_serialOut(val);
            } else {
                scheduleSdr(SDR_SET1);                  // into the shifter two cycles on
            }
            break;

        case ICR:
            // Bit 7 = 1: set bits in mask; bit 7 = 0: clear bits from mask.
            if (val & 0x80) m_icrMask |=  (val & 0x7F);
            else            m_icrMask &= ~(val & 0x7F);
            // A pending flag now enabled raises IRQ two cycles on (the
            // 6526A: next cycle, unless the ICR was read last cycle); one
            // being raised is never cancelled, except on the old 6526 right
            // after a read
            if (m_irqFlags & m_icrMask & 0x7F) {
                if (!m_irqOut) {
                    if (!m_newModel) m_ifrDelay |= IFR_RAISE1 | IFR_D7SET1;
                    else if (!(m_ifrDelay & IFR_READ1)) m_ifrDelay |= IFR_RAISE0 | IFR_D7SET0;
                }
            } else if (!m_newModel && (m_ifrDelay & IFR_ACK_1)) {
                m_ifrDelay &= ~(IFR_RAISE0 | IFR_D7SET0);
            }
            ciaLog()->trace("{} ICR write: val=${:02X} mask=${:02X} flags=${:02X}",
                            m_name, val, m_icrMask, m_irqFlags);
            break;

        case CRA: {
            if ((val & CR_START) && !(m_cra & CR_START)) m_tat = 1;
            if (((val ^ m_cra) & CRA_SPMODE_OUT) && !m_instantSdr) sdrModeChange(val);
            if ((val & CR_START) && !(m_ta.state & T_START))
                ciaLog()->debug("{} Timer A started: latch=${:04X} {} (mask=${:02X})",
                                m_name, m_ta.latch, (val & CR_ONESHOT) ? "one-shot" : "continuous", m_icrMask);
            setCtrl(m_ta, val);
            m_cra = val & ~CR_LOAD;
            break;
        }
        case CRB: {
            if ((val & CR_START) && !(m_crb & CR_START)) m_tbt = 1;
            if ((val & CR_START) && !(m_tb.state & T_START))
                ciaLog()->debug("{} Timer B started: latch=${:04X} {} (mask=${:02X})",
                                m_name, m_tb.latch, (val & CR_ONESHOT) ? "one-shot" : "continuous", m_icrMask);
            // Counting timer A underflows (or CNT): no phi2 counting
            setCtrl(m_tb, (val & CRB_INMODE_TA) ? (uint8_t)(val | 0x20) : val);
            m_crb = val & ~CR_LOAD;
            break;
        }

        default: break;
    }
    if (m_sdrRerun) {                           // the serial port acts on the write
        m_sdrRerun = false;
        runSdrCycle();                          // (a flag set now is seen next cycle)
    }
    return true;
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

// The timer state machine (VICE ciat_init_table): each cycle the CR bits as
// written move one stage on: START+PHI2IN -> COUNT2 -> COUNT3 (decrements)
// -> COUNT; FLOAD -> LOAD1 -> LOAD; ONESHOT -> ONESHOT0 -> ONESHOT1.
const uint16_t* CIA6526::stateTable() {
    static const std::vector<uint16_t> table = [] {
        std::vector<uint16_t> t(0x2000);
        for (int i = 0; i < 0x2000; ++i) {
            uint16_t s = (uint16_t)(i & (T_START | T_ONESHOT | T_PHI2IN));
            if ((i & T_START) && (i & T_PHI2IN)) s |= T_COUNT2;
            if ((i & T_COUNT2) || ((i & T_STEP) && (i & T_START))) s |= T_COUNT3;
            if (i & T_COUNT3) s |= T_COUNT;
            if (i & T_FLOAD) s |= T_LOAD1;
            if (i & T_LOAD1) s |= T_LOAD;
            if (i & T_ONESHOT) s |= T_ONESHOT0;
            if (i & T_ONESHOT0) s |= T_ONESHOT1;
            t[i] = s;
        }
        return t;
    }();
    return table.data();
}

// One cycle of a timer (the body of VICE's ciat_update); true on underflow
bool CIA6526::stepTimer(Timer& t, const uint16_t* table) {
    uint16_t s = t.state;
    if (t.cnt && (s & T_COUNT3)) t.cnt--;
    s = table[s & 0x1FFF];
    bool under = false;
    if (t.cnt == 0 && (s & T_COUNT3)) { s |= T_LOAD | T_OUT; under = true; }
    if (s & T_LOAD) { t.cnt = t.latch; s &= ~T_COUNT3; }
    if ((s & T_OUT) && (s & (T_ONESHOT1 | T_ONESHOT0))) s &= ~(T_START | T_COUNT2);
    t.state = s;
    return under;
}

// The one-shot pipeline is settled (all set or all clear)
static inline bool oneshotSettled(uint16_t t, uint16_t a, uint16_t b, uint16_t c) {
    const uint16_t m = t & (a | b | c);
    return m == 0 || m == (a | b | c);
}

// Counting steadily: one decrement a cycle, nothing else changing
bool CIA6526::timerCounting(uint16_t t) {
    return (t & (T_START | T_FLOAD | T_LOAD1 | T_PHI2IN | T_COUNT2 | T_COUNT3 | T_COUNT | T_LOAD))
               == (T_START | T_PHI2IN | T_COUNT2 | T_COUNT3 | T_COUNT)
        && oneshotSettled(t, T_ONESHOT, T_ONESHOT0, T_ONESHOT1);
}

// Counting steadily or stopped for good (VICE's "warp" cases)
bool CIA6526::timerSteady(uint16_t t) {
    if (timerCounting(t)) return true;
    return !(t & (T_COUNT2 | T_COUNT3 | T_COUNT))
        && (!(t & T_START) || !(t & (T_PHI2IN | T_STEP)))
        && !(t & (T_FLOAD | T_LOAD1 | T_LOAD))
        && oneshotSettled(t, T_ONESHOT, T_ONESHOT0, T_ONESHOT1);
}

// Each state's class for the fast path: 0 changing, 1 stopped, 2 counting
const uint8_t* CIA6526::steadyTable() {
    static const std::vector<uint8_t> table = [] {
        std::vector<uint8_t> t(0x2000);
        for (int i = 0; i < 0x2000; ++i)
            t[i] = timerCounting((uint16_t)i) ? 2 : timerSteady((uint16_t)i) ? 1 : 0;
        return t;
    }();
    return table.data();
}

// One cycle: both timers, then the interrupt flag pipeline
void CIA6526::runCycle() {
    const uint16_t* table = stateTable();
    ++m_clk;
    const bool ua = stepTimer(m_ta, table);
    const bool ub = stepTimer(m_tb, table);
    if (ua) {
        setIrqFlag(ICR_TA);
        m_tat ^= 1;
        // Shifting out: CNT toggles about 1.5 cycles on (VICE ciacore_intta)
        if ((m_cra & CRA_SPMODE_OUT) && !m_instantSdr && (m_srBits || m_sdrValid)) {
            m_sdrDelay |= (m_sdrDelay & (SDR_TOGGLE_CNT0 | SDR_NOGGLE_CNT0)) ? SDR_NOGGLE_CNT1 : SDR_TOGGLE_CNT1;
            m_sdrActive = true;
        }
        // Timer B counting timer A: one count, taken next cycle
        if ((m_crb & (CRB_INMODE_TA | CR_START)) == (CRB_INMODE_TA | CR_START)
            && (m_tb.state & T_START))
            m_tb.state |= T_STEP;
    }
    if (ub) {
        setIrqFlag(ICR_TB);
        // The old 6526's timer B bug: an ICR read the cycle before
        if (!m_newModel && m_rdi == m_clk - 1) m_irqFlags |= IRQ_TBB;
        else                    m_irqFlags &= ~IRQ_TBB;
        m_tbt ^= 1;
    }
    if (m_sdrActive) runSdrCycle();
    if (m_ifrDelay || m_newFlags) runIfrCycle();
}

// Run cycles, skipping ahead while nothing but steady counting goes on
void CIA6526::runTimers(uint64_t cycles) {
    const uint8_t* steady = steadyTable();
    while (cycles > 0) {
        const uint8_t ka = steady[m_ta.state & 0x1FFF], kb = steady[m_tb.state & 0x1FFF];
        if (ka && kb && !m_ifrDelay && !m_newFlags && !m_sdrActive) {
            uint64_t k = cycles;
            if (ka == 2) k = std::min<uint64_t>(k, m_ta.cnt ? m_ta.cnt - 1u : 0u);
            if (kb == 2) k = std::min<uint64_t>(k, m_tb.cnt ? m_tb.cnt - 1u : 0u);
            if (k > 0) {
                if (ka == 2) m_ta.cnt = (uint16_t)(m_ta.cnt - k);
                if (kb == 2) m_tb.cnt = (uint16_t)(m_tb.cnt - k);
                m_clk += k;
                cycles -= k;
                continue;
            }
        }
        runCycle();
        --cycles;
    }
    // How long nothing but steady counting goes on (for tick()'s shortcut)
    const uint8_t ka = steady[m_ta.state & 0x1FFF], kb = steady[m_tb.state & 0x1FFF];
    m_quiet = 0;
    if (ka && kb && !m_ifrDelay && !m_newFlags && !m_sdrActive) {
        uint64_t k = ~0ull;
        if (ka == 2) k = std::min<uint64_t>(k, m_ta.cnt ? m_ta.cnt - 1u : 0u);
        if (kb == 2) k = std::min<uint64_t>(k, m_tb.cnt ? m_tb.cnt - 1u : 0u);
        m_quiet = k;
        m_quietA = ka == 2;
        m_quietB = kb == 2;
    }
}

// A shift register event from a register write: the pipeline runs for this
// cycle again (VICE's schedule_sdr_alarm for the current cycle)
void CIA6526::scheduleSdr(uint32_t feed) {
    m_sdrDelay |= feed;
    m_quiet = 0;
    m_sdrRerun = true;
}

// CRA's serial port direction changes (VICE strange_extra_sdr_flags and
// cia_store CRA). It sees the shift register as it was before this cycle's
// step, as VICE's references show (cia-sdr-icr)
void CIA6526::sdrModeChange(uint8_t cra) {
    const bool stepped = m_sdrStepClk == m_clk;
    const uint32_t D = stepped ? m_sdrDelayBefore : m_sdrDelay;
    const uint8_t B = stepped ? m_srBitsBefore : m_srBits;
    if ((B > 1 && B < 15) || (B == 15 && !(D & SDR_CNT2)))
        scheduleSdr(SDR_SET_IRQ2);
    if (!(cra & CRA_SPMODE_OUT)) {
        // To input: a byte half out may still finish
        const uint32_t want = SDR_CNT1 | SDR_CNT2;
        bool force = (D & want) != want;
        if (!force && B != 2 && !(D & (SDR_TOGGLE_CNT1 | 0x0001)) && (D & SDR_TOGGLE_CNT0))
            force = true;
        m_sdrForceFinish = force;
    } else {
        if (!m_cntOut && m_srBits) m_shifter <<= 1;
        if (m_sdrForceFinish) {
            scheduleSdr(SDR_SET_IRQ2);
            m_sdrForceFinish = false;
        }
    }
    m_srBits = 0;
    m_sdrValid = false;
    m_sdrDelay &= ~(SDR_ALL_TOGGLE | SDR_ALL_NOGGLE);
    m_cntOut = true;
}

// One cycle of the serial port (VICE ciacore_intsdr)
void CIA6526::runSdrCycle() {
    uint32_t feed = 0;
    if (m_sdrStepClk != m_clk) {               // (not a second step this cycle)
        m_sdrStepClk = m_clk;
        m_sdrDelayBefore = m_sdrDelay;
        m_srBitsBefore = m_srBits;
    }
    if (m_sdrDelay & SDR_SET0) {
        if (m_srBits == 0) {
            m_srBits = 16;
            m_shifter = (uint16_t)(m_sdr << 1);
        } else if (m_srBits == 1) {
            m_shifter |= m_sdr;
            m_srBits = 17;
        } else {
            m_sdrValid = true;
        }
    }
    if (m_sdrDelay & SDR_TOGGLE_CNT0) {
        if (m_srBits && (--m_srBits & 1)) {
            m_cntOut = false;                           // a bit goes out
            if (m_srBits == 1) {
                if (m_serialOut) m_serialOut((uint8_t)(m_shifter >> 8));
                feed |= SDR_SET_IRQ2;                   // the flag two cycles on
                if (m_sdrValid) {                       // the next byte follows on
                    m_shifter |= m_sdr;
                    m_sdrValid = false;
                    m_srBits = 17;
                }
            }
        } else {
            m_shifter <<= 1;
            m_cntOut = true;
        }
    }
    if (m_sdrDelay & SDR_SET_IRQ0) {
        m_spClk = m_clk;
        m_spPrior = m_irqFlags;
        setIrqFlag(ICR_SP);
    }
    m_sdrDelay |= feed;
    m_sdrDelay = (m_sdrDelay << 1) & ~SDR_CLEAR;
    if (m_cntOut) m_sdrDelay |= SDR_CNT0;
    bool active = (m_sdrDelay & SDR_ACTIVE) != 0;
    if (!active) {
        const uint32_t cnt = m_sdrDelay & SDR_ALL_CNT;
        active = cnt != 0 && cnt != SDR_ALL_CNT;
    }
    m_sdrActive = active;
}

// One cycle of the interrupt flag pipeline (VICE cia_run_ifr_cycle)
void CIA6526::runIfrCycle() {
    uint32_t delay = m_ifrDelay;
    if (delay & IFR_ACK0) {
        if (m_newModel) m_irqFlags &= ~m_ackFlags;       // the flags read clear now
        else            m_irqFlags &= ~ICR_INT;          // bit 7 clears after a read
        m_ackFlags = 0;
    }
    if (m_newFlags & m_icrMask & 0x1F) {
        // The 6526A raises IRQ this cycle, unless the ICR was read last cycle
        if (m_newModel && m_rdi + 1 != m_clk) delay |= IFR_RAISE0 | IFR_D7SET0;
        else                                  delay |= IFR_RAISE1 | IFR_D7SET1;
    }
    if (delay & IFR_D7SET0) m_irqFlags |= ICR_INT;
    if (delay & IFR_RAISE0) setInt(true);
    m_newFlags = 0;
    m_ifrDelay = (delay << 1) & ~IFR_CLEAR;
}

// A source sets its flag now; the pipeline raises IRQ from it next cycle
void CIA6526::setIrqFlag(uint32_t bits) {
    m_irqFlags |= bits;
    m_newFlags |= bits;
    m_ackFlags &= ~bits;               // a flag set again isn't cleared by a read before
    m_quiet = 0;
}

// The IRQ output; a shared line is only driven on a change
void CIA6526::setInt(bool on) {
    if (on == m_irqOut) return;
    m_irqOut = on;
    if (m_irqLine) m_irqLine->set(on);
}

// PB6/PB7 as timer outputs: a one-cycle pulse on underflow, or a toggle
// (set when the timer is started)
uint8_t CIA6526::pb67(uint8_t byte) const {
    if (!((m_cra | m_crb) & CR_PBON)) return byte;
    if (m_cra & CR_PBON) {
        byte &= 0xBF;
        if ((m_cra & CR_TOGGLE) ? m_tat : (m_ta.state & T_OUT)) byte |= 0x40;
    }
    if (m_crb & CR_PBON) {
        byte &= 0x7F;
        if ((m_crb & CR_TOGGLE) ? m_tbt : (m_tb.state & T_OUT)) byte |= 0x80;
    }
    return byte;
}

void CIA6526::tick(uint64_t cycles) {
    // Up to each mains tick, then the tick (the TOD's flag lands in its cycle)
    while (cycles >= m_todCountdown) {
        const uint64_t n = m_todCountdown;
        runCycles(n);
        cycles -= n;
        powerTick();
    }
    m_todCountdown -= cycles;
    runCycles(cycles);
}

void CIA6526::runCycles(uint64_t cycles) {
    if (!cycles) return;
    if (cycles <= m_quiet) {                    // nothing but steady counting
        m_quiet -= cycles;
        if (m_quietA) m_ta.cnt = (uint16_t)(m_ta.cnt - cycles);
        if (m_quietB) m_tb.cnt = (uint16_t)(m_tb.cnt - cycles);
        m_clk += cycles;
    } else {
        runTimers(cycles);
    }
}

// The alarm matches the clock: the TOD flag
void CIA6526::checkTodAlarm() {
    if (m_todTen == m_alarmTen && m_todSec == m_alarmSec &&
        m_todMin == m_alarmMin && m_todHr == m_alarmHr)
        setIrqFlag(ICR_TOD);
}

// A mains tick (VICE ciacore_inttod, without its random jitter)
void CIA6526::powerTick() {
    const uint64_t perSec = m_clockHz;
    m_todTicks = perSec / m_powerHz;
    const uint64_t due = (uint64_t)m_powerTickCounter * perSec / m_powerHz;
    if (m_powerTicks < due) ++m_todTicks;
    else if (m_powerTicks > due) --m_todTicks;
    if (++m_powerTickCounter >= m_powerHz) {
        m_todTicks = perSec > m_powerTicks ? perSec - m_powerTicks : 1;
        m_powerTickCounter = 0;
        m_powerTicks = 0;
    } else {
        m_powerTicks += m_todTicks;
    }
    m_todCountdown = m_todTicks ? m_todTicks : 1;

    if (m_todStopped) return;
    // The divider: a 3-bit ring counter (000 001 011 111 110 100), matching
    // at 110 for 50 Hz and 100 for 60 Hz; the tenths count a tick later
    if (m_todTickCounter != ((m_cra & CRA_TODIN) ? 4 : 5)) {
        if (++m_todTickCounter > 5) m_todTickCounter = 0;
        return;
    }
    m_todTickCounter = 0;
    // Each digit is a 4-bit counter (3 bits for the tens of seconds and
    // minutes), so set values outside BCD count on as the chip does
    int ts = m_todTen & 0x0F, sl = m_todSec & 0x0F, sh = (m_todSec >> 4) & 7;
    int ml = m_todMin & 0x0F, mh = (m_todMin >> 4) & 7;
    int hl = m_todHr & 0x0F, hh = (m_todHr >> 4) & 1, pm = m_todHr & 0x80;
    ts = (ts + 1) & 0x0F;
    if (ts == 10) {
        ts = 0;
        sl = (sl + 1) & 0x0F;
        if (sl == 10) {
            sl = 0;
            sh = (sh + 1) & 7;
            if (sh == 6) {
                sh = 0;
                ml = (ml + 1) & 0x0F;
                if (ml == 10) {
                    ml = 0;
                    mh = (mh + 1) & 7;
                    if (mh == 6) {
                        mh = 0;
                        // 09:59:59 -> 10:00:00, 12:59:59 -> 01:00:00
                        if ((hh == 1 && hl == 2) || (hh == 0 && hl == 9)) {
                            hl = hh;
                            hh ^= 1;
                        } else {
                            hl = (hl + 1) & 0x0F;
                            if (hh == 1 && hl == 2) pm ^= 0x80;   // 11 -> 12 flips AM/PM
                        }
                    }
                }
            }
        }
    }
    m_todTen = (uint8_t)ts;
    m_todSec = (uint8_t)(sl | (sh << 4));
    m_todMin = (uint8_t)(ml | (mh << 4));
    m_todHr  = (uint8_t)(hl | (hh << 4) | pm);
    checkTodAlarm();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void CIA6526::flagTrigger() {
    setIrqFlag(0x10);
}

void CIA6526::getDeviceInfo(DeviceInfo& out) const {
    out.name = m_name;
    out.baseAddr = m_baseAddr;
    out.addrMask = addrMask();

    auto addReg = [&](const std::string& name, Reg reg, uint8_t val, const std::string& desc = "") {
        out.registers.push_back({name, (uint32_t)reg, val, desc});
    };

    addReg("PRA", PRA, m_pra);
    addReg("PRB", PRB, m_prb);
    addReg("DDRA", DDRA, m_ddra);
    addReg("DDRB", DDRB, m_ddrb);
    addReg("TALO", TALO, m_ta.latch & 0xFF);
    addReg("TAHI", TAHI, m_ta.latch >> 8);
    addReg("TBLO", TBLO, m_tb.latch & 0xFF);
    addReg("TBHI", TBHI, m_tb.latch >> 8);
    addReg("TODTEN", TODTEN, m_todTen);
    addReg("TODSEC", TODSEC, m_todSec);
    addReg("TODMIN", TODMIN, m_todMin);
    addReg("TODHR", TODHR, m_todHr);
    addReg("SDR", SDR, 0); // SDR not fully implemented
    addReg("ICR", ICR, (uint8_t)m_irqFlags);
    addReg("CRA", CRA, m_cra);
    addReg("CRB", CRB, m_crb);

    char buf[32];
    std::sprintf(buf, "%d", (int)m_ta.cnt);
    out.state.push_back({"Timer A Counter", buf});
    std::sprintf(buf, "%d", (int)m_tb.cnt);
    out.state.push_back({"Timer B Counter", buf});
    out.state.push_back({"Timer A Running", (m_ta.state & T_START) ? "true" : "false"});
    out.state.push_back({"Timer B Running", (m_tb.state & T_START) ? "true" : "false"});
    
    out.dependencies.push_back({"Port A Device", m_portADevice ? "connected" : "none"});
    out.dependencies.push_back({"Port B Device", m_portBDevice ? "connected" : "none"});
    out.dependencies.push_back({"IRQ Line", m_irqLine ? "connected" : "none"});
}

std::string CIA6526::advanceCycles(uint32_t cycles) {
    const bool interruptWasSet = (m_irqFlags & ICR_INT) != 0;

    // Advance the timers and interrupt pipeline (not the TOD clock)
    runTimers(cycles);

    const bool interruptNowSet = (m_irqFlags & ICR_INT) != 0;

    std::string result = m_name + " advanced by " + std::to_string(cycles) + " cycles\n\n";

    // Timer A status
    result += "Timer A: count=$" + toHex(m_ta.cnt, 4);
    result += " (latch=$" + toHex(m_ta.latch, 4) + ")";
    result += " [" + std::string((m_ta.state & T_START) ? "RUNNING" : "STOPPED") + "]";
    result += "\n";

    // Timer B status
    result += "Timer B: count=$" + toHex(m_tb.cnt, 4);
    result += " (latch=$" + toHex(m_tb.latch, 4) + ")";
    result += " [" + std::string((m_tb.state & T_START) ? "RUNNING" : "STOPPED") + "]";
    result += "\n";

    // ICR status
    result += "ICR: enabled_mask=$" + toHex(m_icrMask, 2);
    result += " pending=$" + toHex((uint8_t)m_irqFlags, 2) + "\n";

    // Interrupt bit status
    result += "Interrupt bit: " + std::string(interruptNowSet ? "SET" : "NOT SET");
    if (interruptWasSet != interruptNowSet) {
        result += " (changed during advance)";
    }
    result += "\n";

    return result;
}

std::string CIA6526::getStatusString() const {
    std::string result = m_name + " Status:\n";

    // Timer A
    result += "  Timer A: count=$" + toHex(m_ta.cnt, 4);
    result += " latch=$" + toHex(m_ta.latch, 4);
    result += " [" + std::string((m_ta.state & T_START) ? "RUNNING" : "STOPPED") + "]";
    result += " mode=" + std::string((m_cra & CR_ONESHOT) ? "oneshot" : "continuous") + "\n";

    // Timer B
    result += "  Timer B: count=$" + toHex(m_tb.cnt, 4);
    result += " latch=$" + toHex(m_tb.latch, 4);
    result += " [" + std::string((m_tb.state & T_START) ? "RUNNING" : "STOPPED") + "]";
    result += " mode=" + std::string((m_crb & CR_ONESHOT) ? "oneshot" : "continuous") + "\n";

    // ICR
    result += "  ICR: enabled=$" + toHex(m_icrMask, 2);
    result += " pending=$" + toHex((uint8_t)m_irqFlags, 2) + "\n";

    // Port status
    result += "  Port A: $" + toHex(m_pra, 2) + " (IEC bus)\n";
    result += "  Port B: $" + toHex(m_prb, 2) + "\n";

    // Interrupt bit
    result += "  Interrupt bit: " + std::string((m_irqFlags & ICR_INT) ? "SET" : "NOT SET") + "\n";

    return result;
}
