// uPD1990AC / uPD4990A serial calendar clock, after the NEC data sheets;
// MAME's upd1990a.cpp and VirtualT's clock.cpp consulted. mmsim's own code.

#include "upd1990a.h"

namespace {
template <class T> void put(std::vector<uint8_t>& v, T x) {
    for (size_t i = 0; i < sizeof(T); ++i) v.push_back((uint8_t)((uint64_t)x >> (8 * i)));
}
template <class T> void get(const uint8_t*& p, const uint8_t* e, T& x) {
    uint64_t r = 0;
    for (size_t i = 0; i < sizeof(T) && p < e; ++i) r |= (uint64_t)*p++ << (8 * i);
    x = (T)r;
}
int fromBcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }
uint8_t toBcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
int daysIn(int month, int year) {
    static const int D[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return (month >= 1 && month <= 12) ? D[month - 1] : 31;
}
// BCD increment of v, wrapping from `last` to `first`; true on the wrap
bool bcdStep(uint8_t& v, int first, int last) {
    const int n = fromBcd(v) + 1;
    if (n > last) {
        v = toBcd(first);
        return true;
    }
    v = toBcd(n);
    return false;
}
}  // namespace

Upd1990a::Upd1990a(Variant v) : m_variant(v) {
    hostClock = []() { return std::time(nullptr); };
    syncToHost();
}

void Upd1990a::syncToHost() {
    const std::time_t t = hostClock() + (std::time_t)m_offset;
    std::tm tm{};
    localtime_r(&t, &tm);
    m_counter[0] = toBcd(tm.tm_sec > 59 ? 59 : tm.tm_sec);
    m_counter[1] = toBcd(tm.tm_min);
    m_counter[2] = toBcd(tm.tm_hour);
    m_counter[3] = toBcd(tm.tm_mday);
    m_counter[4] = (uint8_t)(((tm.tm_mon + 1) << 4) | tm.tm_wday);
    m_counter[5] = toBcd(tm.tm_year % 100);
    m_hiddenYear = tm.tm_year + 1900;
}

std::tm Upd1990a::counterTm() const {
    std::tm tm{};
    tm.tm_sec = fromBcd(m_counter[0]);
    tm.tm_min = fromBcd(m_counter[1]);
    tm.tm_hour = fromBcd(m_counter[2]);
    tm.tm_mday = fromBcd(m_counter[3]);
    tm.tm_mon = (m_counter[4] >> 4) - 1;
    tm.tm_year = m_variant == Variant::Upd4990a ? 100 + fromBcd(m_counter[5]) : m_hiddenYear - 1900;
    tm.tm_isdst = -1;
    return tm;
}

void Upd1990a::advanceSecond() {
    if (!bcdStep(m_counter[0], 0, 59)) return;
    if (!bcdStep(m_counter[1], 0, 59)) return;
    if (!bcdStep(m_counter[2], 0, 23)) return;
    // A new day: the weekday counts on its own
    m_counter[4] = (uint8_t)((m_counter[4] & 0xF0) | (((m_counter[4] & 0x0F) + 1) % 7));
    int month = m_counter[4] >> 4;
    const int year = m_variant == Variant::Upd4990a ? 2000 + fromBcd(m_counter[5]) : m_hiddenYear;
    if (!bcdStep(m_counter[3], 1, daysIn(month, year))) return;
    if (++month > 12) {
        month = 1;
        bcdStep(m_counter[5], 0, 99);
        ++m_hiddenYear;
    }
    m_counter[4] = (uint8_t)((month << 4) | (m_counter[4] & 0x0F));
}

void Upd1990a::setTp(bool v) {
    if (m_tp == v) return;
    m_tp = v;
    if (onTp) onTp(v);
}

void Upd1990a::setTpPeriod(uint64_t halfTicks) {
    m_tpHalf = halfTicks;
    m_tpPhase = 0;
    m_tpRun = true;
}

void Upd1990a::crystal() {
    ++m_xtal;
    if (m_test) {
        // Test mode: the counter steps at 1024 Hz
        if (!m_held && (m_xtal & 31) == 0) advanceSecond();
    } else if (!m_held && ++m_secPhase >= 32768) {
        m_secPhase = 0;
        advanceSecond();
    }
    if (m_tpRun && ++m_tpPhase >= m_tpHalf) {
        m_tpPhase = 0;
        setTp(!m_tp);
    }
}

void Upd1990a::tickCrystal(uint64_t xtalTicks) {
    while (xtalTicks--) crystal();
}

void Upd1990a::tick(uint64_t cycles) {
    m_frac += (double)cycles * 32768.0;
    while (m_frac >= m_inputHz) {
        m_frac -= m_inputHz;
        crystal();
    }
}

bool Upd1990a::dataOut() const {
    if (!m_oe && !m_test) return true;
    switch (m_doutMode) {
    case 0: return (m_xtal / 16384) & 1;                    // 1 Hz
    case 3:
        // Time read: 0.5 Hz on the uPD1990AC, 1 Hz on the uPD4990A (MAME)
        return m_variant == Variant::Upd4990a ? (m_xtal / 16384) & 1 : (m_xtal / 32768) & 1;
    default: return m_shift[0] & 1;                         // shift, time set
    }
}

void Upd1990a::setClk(bool level) {
    const bool rising = level && !m_clk;
    m_clk = level;
    if (!rising || !m_cs) return;
    const bool serial = m_variant == Variant::Upd4990a && m_cPins == 7;
    bool in = m_dataIn;
    if (serial) {
        // DATA IN feeds the command register, whose bit 0 feeds the data register
        in = m_serialCmd & 1;
        m_serialCmd = (uint8_t)((m_serialCmd >> 1) | (m_dataIn ? 8 : 0));
    }
    if (m_cmd != 1) return;
    const int n = shiftBytes();
    for (int i = 0; i < n; ++i) {
        const bool carry = i + 1 < n ? (m_shift[i + 1] & 1) : in;
        m_shift[i] = (uint8_t)((m_shift[i] >> 1) | (carry ? 0x80 : 0));
    }
}

void Upd1990a::setStb(bool level) {
    const bool rising = level && !m_stb;
    m_stb = level;
    if (!rising || !m_cs) return;
    int cmd = m_cPins;
    if (m_variant == Variant::Upd4990a) {
        if (cmd == 7) cmd = m_serialCmd & 15;
    } else if (cmd == 7) {
        cmd = 15;                     // the uPD1990AC's 7 is the test mode
    }
    execute(cmd);
}

void Upd1990a::releaseHold() {
    if (!m_held) return;
    m_held = false;
    m_secPhase = 1024;                // the next second 31/32 s on
}

void Upd1990a::execute(int cmd) {
    m_cmd = cmd;
    const bool wasTest = m_test;
    if (cmd != 15) m_test = false;
    switch (cmd) {
    case 0:
        releaseHold();
        m_doutMode = 0;
        if (wasTest) setTpPeriod(256);
        break;
    case 1:
        releaseHold();
        m_doutMode = 1;
        break;
    case 2: {
        const int n = shiftBytes();
        for (int i = 0; i < n; ++i) m_counter[i] = m_shift[i];
        m_held = true;
        m_doutMode = 2;
        std::tm tm = counterTm();
        const std::time_t set = mktime(&tm);
        if (set != (std::time_t)-1) m_offset = (int64_t)(set - hostClock());
        break;
    }
    case 3: {
        releaseHold();
        const int n = shiftBytes();
        for (int i = 0; i < n; ++i) m_shift[i] = m_counter[i];
        m_doutMode = 3;
        break;
    }
    case 4: m_tpInterval = false; setTpPeriod(256); break;     // 64 Hz
    case 5: m_tpInterval = false; setTpPeriod(64); break;      // 256 Hz
    case 6: m_tpInterval = false; setTpPeriod(8); break;       // 2048 Hz
    case 7: m_tpInterval = false; setTpPeriod(4); break;       // 4096 Hz (uPD4990A)
    case 8: m_tpInterval = true; setTp(true); setTpPeriod(16384); break;          // 1 s
    case 9: m_tpInterval = true; setTp(true); setTpPeriod(16384 * 10); break;     // 10 s
    case 10: m_tpInterval = true; setTp(true); setTpPeriod(16384 * 30); break;    // 30 s
    case 11: m_tpInterval = true; setTp(true); setTpPeriod(16384 * 60); break;    // 60 s
    case 12:
        setTp(true);
        m_tpPhase = 0;
        break;
    case 13: m_tpRun = true; break;
    case 14: m_tpRun = false; break;
    case 15:
        m_test = true;
        break;
    default: break;
    }
}

std::vector<uint8_t> Upd1990a::saveState() const {
    std::vector<uint8_t> v;
    for (uint8_t b : m_counter) put(v, b);
    for (uint8_t b : m_shift) put(v, b);
    put(v, m_serialCmd); put(v, (int32_t)m_hiddenYear); put(v, m_cPins); put(v, (uint8_t)m_cmd);
    put(v, (uint8_t)m_doutMode);
    put(v, (uint16_t)((m_dataIn ? 1 : 0) | (m_clk ? 2 : 0) | (m_stb ? 4 : 0) | (m_cs ? 8 : 0) | (m_oe ? 16 : 0) |
                      (m_held ? 32 : 0) | (m_test ? 64 : 0) | (m_tp ? 128 : 0) | (m_tpInterval ? 256 : 0) |
                      (m_tpRun ? 512 : 0)));
    put(v, m_tpHalf); put(v, m_tpPhase); put(v, m_secPhase); put(v, m_xtal); put(v, m_offset);
    return v;
}

void Upd1990a::loadState(const std::vector<uint8_t>& s) {
    const uint8_t* p = s.data();
    const uint8_t* e = p + s.size();
    for (uint8_t& b : m_counter) get(p, e, b);
    for (uint8_t& b : m_shift) get(p, e, b);
    int32_t year = 2000;
    uint8_t cmd = 0, dout = 0;
    uint16_t f = 0;
    get(p, e, m_serialCmd); get(p, e, year); get(p, e, m_cPins); get(p, e, cmd); get(p, e, dout);
    get(p, e, f);
    get(p, e, m_tpHalf); get(p, e, m_tpPhase); get(p, e, m_secPhase); get(p, e, m_xtal); get(p, e, m_offset);
    m_hiddenYear = year;
    m_cmd = cmd;
    m_doutMode = dout;
    m_dataIn = f & 1; m_clk = f & 2; m_stb = f & 4; m_cs = f & 8; m_oe = f & 16; m_held = f & 32;
    m_test = f & 64; m_tp = f & 128; m_tpInterval = f & 256; m_tpRun = f & 512;
    if (!m_tpHalf) m_tpHalf = 256;
    if (onTp) onTp(m_tp);
}
