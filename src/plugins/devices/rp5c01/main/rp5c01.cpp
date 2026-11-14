// Ricoh RP5C01 RTC, after the Ricoh data sheet; MAME's rp5c01.cpp
// consulted. mmsim's own code.

#include "rp5c01.h"

namespace {
template <class T> void put(std::vector<uint8_t>& v, T x) {
    for (size_t i = 0; i < sizeof(T); ++i) v.push_back((uint8_t)((uint64_t)x >> (8 * i)));
}
template <class T> void get(const uint8_t*& p, const uint8_t* e, T& x) {
    uint64_t r = 0;
    for (size_t i = 0; i < sizeof(T) && p < e; ++i) r |= (uint64_t)*p++ << (8 * i);
    x = (T)r;
}
// Bits each register keeps, blocks 0 and 1 (the data sheet's register table)
const uint8_t MASK[2][13] = {
    {0xF, 0x7, 0xF, 0x7, 0xF, 0x3, 0x7, 0xF, 0x3, 0xF, 0x1, 0xF, 0xF},
    {0x0, 0x0, 0xF, 0x7, 0xF, 0x3, 0x7, 0xF, 0x3, 0x0, 0x1, 0x3, 0x0},
};
enum { SEC1, SEC10, MIN1, MIN10, HOUR1, HOUR10, WDAY, DAY1, DAY10, MON1, MON10, YEAR1, YEAR10 };
enum { MODE = 13, TEST = 14, RESET = 15 };
int daysIn(int month, bool leap) {
    static const int D[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && leap) return 29;
    return (month >= 1 && month <= 12) ? D[month - 1] : 31;
}
}  // namespace

Rp5c01::Rp5c01() {
    hostClock = []() { return std::time(nullptr); };
    m_reg[1][MON10] = 1;          // 24-hour mode
    syncToHost();
    updatePin();
}

int Rp5c01::hour24() const {
    const uint8_t* r = m_reg[0].data();
    if (is24h()) return r[HOUR10] * 10 + r[HOUR1];
    int h = (r[HOUR10] & 1) * 10 + r[HOUR1];
    if (h == 12) h = 0;
    return h + ((r[HOUR10] & 2) ? 12 : 0);
}

void Rp5c01::setHour24(int h) {
    uint8_t* r = m_reg[0].data();
    if (is24h()) {
        r[HOUR10] = (uint8_t)(h / 10);
        r[HOUR1] = (uint8_t)(h % 10);
        return;
    }
    int h12 = h % 12;
    if (h12 == 0) h12 = 12;
    r[HOUR10] = (uint8_t)((h12 / 10) | (h >= 12 ? 2 : 0));
    r[HOUR1] = (uint8_t)(h12 % 10);
}

void Rp5c01::syncToHost() {
    const std::time_t t = hostClock() + (std::time_t)m_offset;
    std::tm tm{};
    localtime_r(&t, &tm);
    uint8_t* r = m_reg[0].data();
    const int sec = tm.tm_sec > 59 ? 59 : tm.tm_sec;
    r[SEC1] = (uint8_t)(sec % 10); r[SEC10] = (uint8_t)(sec / 10);
    r[MIN1] = (uint8_t)(tm.tm_min % 10); r[MIN10] = (uint8_t)(tm.tm_min / 10);
    setHour24(tm.tm_hour);
    r[WDAY] = (uint8_t)tm.tm_wday;
    r[DAY1] = (uint8_t)(tm.tm_mday % 10); r[DAY10] = (uint8_t)(tm.tm_mday / 10);
    r[MON1] = (uint8_t)((tm.tm_mon + 1) % 10); r[MON10] = (uint8_t)((tm.tm_mon + 1) / 10);
    const int y = tm.tm_year % 100;
    r[YEAR1] = (uint8_t)(y % 10); r[YEAR10] = (uint8_t)(y / 10);
    m_reg[1][YEAR1] = (uint8_t)((tm.tm_year + 1900) % 4);
    checkAlarm();
}

std::tm Rp5c01::counterTm() const {
    const uint8_t* r = m_reg[0].data();
    std::tm tm{};
    tm.tm_sec = r[SEC10] * 10 + r[SEC1];
    tm.tm_min = r[MIN10] * 10 + r[MIN1];
    tm.tm_hour = hour24();
    tm.tm_mday = r[DAY10] * 10 + r[DAY1];
    tm.tm_mon = r[MON10] * 10 + r[MON1] - 1;
    tm.tm_year = 100 + r[YEAR10] * 10 + r[YEAR1];
    tm.tm_isdst = -1;
    return tm;
}

uint8_t Rp5c01::read(int reg) const {
    reg &= 15;
    if (reg == MODE) return m_mode;
    if (reg > MODE) return 0;                 // test and reset are write only
    switch (m_mode & 3) {
    case 0:
    case 1: return m_reg[m_mode & 3][reg];
    case 2: return m_ram[reg] & 0x0F;
    default: return m_ram[reg] >> 4;
    }
}

void Rp5c01::write(int reg, uint8_t v) {
    reg &= 15;
    v &= 0x0F;
    switch (reg) {
    case MODE:
        m_mode = v;
        checkAlarm();
        updatePin();
        return;
    case TEST: m_test = v; return;
    case RESET:
        m_reset = v;
        if (v & 1)
            for (int i = MIN1; i <= DAY10; ++i) m_reg[1][i] = 0;
        if (v & 2) m_div = 0;
        checkAlarm();
        updatePin();
        return;
    default: break;
    }
    switch (m_mode & 3) {
    case 0:
    case 1: {
        const int b = m_mode & 3;
        m_reg[b][reg] = v & MASK[b][reg];
        if (b == 0) {
            std::tm tm = counterTm();
            const std::time_t set = mktime(&tm);
            if (set != (std::time_t)-1) m_offset = (int64_t)(set - hostClock());
        }
        checkAlarm();
        updatePin();
        break;
    }
    case 2: m_ram[reg] = (uint8_t)((m_ram[reg] & 0xF0) | v); break;
    default: m_ram[reg] = (uint8_t)((v << 4) | (m_ram[reg] & 0x0F)); break;
    }
}

void Rp5c01::advanceSecond() {
    uint8_t* r = m_reg[0].data();
    int sec = r[SEC10] * 10 + r[SEC1] + 1;
    if (sec < 60) { r[SEC1] = (uint8_t)(sec % 10); r[SEC10] = (uint8_t)(sec / 10); return; }
    r[SEC1] = r[SEC10] = 0;
    int min = r[MIN10] * 10 + r[MIN1] + 1;
    if (min < 60) { r[MIN1] = (uint8_t)(min % 10); r[MIN10] = (uint8_t)(min / 10); return; }
    r[MIN1] = r[MIN10] = 0;
    int h = hour24() + 1;
    if (h < 24) { setHour24(h); return; }
    setHour24(0);
    r[WDAY] = (uint8_t)((r[WDAY] + 1) % 7);
    const int month = r[MON10] * 10 + r[MON1];
    const int day = r[DAY10] * 10 + r[DAY1] + 1;
    if (day <= daysIn(month, (m_reg[1][YEAR1] & 3) == 0)) {
        r[DAY1] = (uint8_t)(day % 10); r[DAY10] = (uint8_t)(day / 10);
        return;
    }
    r[DAY1] = 1; r[DAY10] = 0;
    if (month < 12) {
        r[MON1] = (uint8_t)((month + 1) % 10); r[MON10] = (uint8_t)((month + 1) / 10);
        return;
    }
    r[MON1] = 1; r[MON10] = 0;
    const int y = (r[YEAR10] * 10 + r[YEAR1] + 1) % 100;
    r[YEAR1] = (uint8_t)(y % 10); r[YEAR10] = (uint8_t)(y / 10);
    m_reg[1][YEAR1] = (uint8_t)((m_reg[1][YEAR1] + 1) & 3);
}

void Rp5c01::checkAlarm() {
    // Minute through 10-day match (an all-zero alarm never matches)
    bool match = true, zero = true;
    for (int i = MIN1; i <= DAY10; ++i) {
        if (m_reg[1][i]) zero = false;
        if (m_reg[1][i] != m_reg[0][i]) match = false;
    }
    m_alarmMatch = match && !zero;
}

bool Rp5c01::alarmPin() const { return m_pin; }

void Rp5c01::updatePin() {
    bool low = (m_mode & 4) && m_alarmMatch;
    // The 16 Hz and 1 Hz pulses: low in the second half of each period
    if (!(m_reset & 4) && ((m_div / 1024) & 1)) low = true;
    if (!(m_reset & 8) && m_div >= 16384) low = true;
    const bool pin = !low;
    if (pin == m_pin) return;
    m_pin = pin;
    if (onAlarm) onAlarm(pin);
}

void Rp5c01::crystal() {
    if (++m_div >= 32768) {
        m_div = 0;
        if (m_mode & 8) {
            advanceSecond();
            checkAlarm();
        }
    }
    if ((m_div & 1023) == 0) updatePin();
}

void Rp5c01::tickCrystal(uint64_t xtalTicks) {
    while (xtalTicks--) crystal();
}

void Rp5c01::tick(uint64_t cycles) {
    m_frac += (double)cycles * 32768.0;
    while (m_frac >= m_inputHz) {
        m_frac -= m_inputHz;
        crystal();
    }
}

std::vector<uint8_t> Rp5c01::saveState() const {
    std::vector<uint8_t> v;
    for (const auto& b : m_reg) v.insert(v.end(), b.begin(), b.end());
    v.insert(v.end(), m_ram.begin(), m_ram.end());
    put(v, m_mode); put(v, m_reset); put(v, m_test); put(v, m_div); put(v, m_offset);
    return v;
}

void Rp5c01::loadState(const std::vector<uint8_t>& s) {
    if (s.size() < 39) return;
    const uint8_t* p = s.data();
    const uint8_t* e = p + s.size();
    for (auto& b : m_reg)
        for (auto& x : b) x = *p++;
    for (auto& x : m_ram) x = *p++;
    get(p, e, m_mode); get(p, e, m_reset); get(p, e, m_test); get(p, e, m_div); get(p, e, m_offset);
    checkAlarm();
    updatePin();
}
