#include "mega65_rtc.h"
#include "libmem/main/ibus.h"
#include <cstring>
#include <ctime>

Mega65Rtc::Mega65Rtc(uint32_t base) : m_base(base) {
    reset();
}

void Mega65Rtc::reset() {
    m_offsetSec = m_offsetMin = m_offsetHour = 0;
    m_offsetDay = m_offsetMon = m_offsetYear = 0;
    m_offsetActive = false;
    std::memset(m_nvram, 0, sizeof(m_nvram));
}

bool Mega65Rtc::ioRead(IBus*, uint32_t addr, uint8_t* val) {
    if ((addr & ~addrMask()) != m_base) return false;
    uint8_t reg = addr & 0x7F;

    // NVRAM: $D740-$D77F → offsets $40-$7F
    if (reg >= 0x40 && reg <= 0x7F) {
        *val = m_nvram[reg - 0x40];
        return true;
    }

    // RTC registers: $D710-$D715 → offsets $10-$15
    if (reg < 0x10 || reg > 0x15) {
        *val = 0;
        return true;
    }
    reg -= 0x10;  // normalize to 0-5

    // Get current host time
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);

    // Apply machine offsets if the clock was "set" by the program
    int sec  = t->tm_sec  + m_offsetSec;
    int min  = t->tm_min  + m_offsetMin;
    int hour = t->tm_hour + m_offsetHour;
    int day  = t->tm_mday + m_offsetDay;
    int mon  = (t->tm_mon + 1) + m_offsetMon;  // tm_mon is 0-based
    int year = (t->tm_year % 100) + m_offsetYear;

    // Normalize (simple clamping, not full calendar math)
    while (sec < 0) sec += 60;   sec %= 60;
    while (min < 0) min += 60;   min %= 60;
    while (hour < 0) hour += 24; hour %= 24;
    if (day < 1) day = 1; if (day > 31) day = 31;
    if (mon < 1) mon = 1; if (mon > 12) mon = 12;
    if (year < 0) year += 100; year %= 100;

    switch (reg) {
        case 0x00: *val = toBcd(sec);          break; // RTCSEC
        case 0x01: *val = toBcd(min);          break; // RTCMIN
        case 0x02: *val = toBcd(hour) | 0x80;  break; // RTCHOUR (24h mode, bit 7 set)
        case 0x03: *val = toBcd(day);          break; // RTCDAY
        case 0x04: *val = toBcd(mon);          break; // RTCMONTH
        case 0x05: *val = toBcd(year);         break; // RTCYEAR
        default:   *val = 0;                   break;
    }
    return true;
}

bool Mega65Rtc::ioWrite(IBus*, uint32_t addr, uint8_t val) {
    if ((addr & ~addrMask()) != m_base) return false;
    uint8_t reg = addr & 0x7F;

    // NVRAM writes
    if (reg >= 0x40 && reg <= 0x7F) {
        m_nvram[reg - 0x40] = val;
        return true;
    }

    // RTC register writes: compute offset from current host time
    if (reg < 0x10 || reg > 0x15) return true;
    reg -= 0x10;

    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    int written = fromBcd(val & 0x7F); // mask off flags

    switch (reg) {
        case 0x00: m_offsetSec  = written - t->tm_sec;         break;
        case 0x01: m_offsetMin  = written - t->tm_min;          break;
        case 0x02: m_offsetHour = (written & 0x3F) - t->tm_hour; break;
        case 0x03: m_offsetDay  = written - t->tm_mday;         break;
        case 0x04: m_offsetMon  = written - (t->tm_mon + 1);    break;
        case 0x05: m_offsetYear = written - (t->tm_year % 100); break;
        default: break;
    }
    m_offsetActive = true;
    return true;
}

void Mega65Rtc::tick(uint64_t cycles) {
    if (!m_physBus) return;
    // Update physical bus at $FFD7110-$FFD7115 every ~1000 cycles
    // so the ROM's 32-bit indirect reads see current time.
    m_tickAccum += cycles;
    if (m_tickAccum < 1000) return;
    m_tickAccum = 0;

    time_t now = time(nullptr);
    struct tm* t = localtime(&now);

    int sec  = (t->tm_sec  + m_offsetSec  + 60) % 60;
    int min  = (t->tm_min  + m_offsetMin  + 60) % 60;
    int hour = (t->tm_hour + m_offsetHour + 24) % 24;
    int day  = t->tm_mday + m_offsetDay;
    int mon  = (t->tm_mon + 1) + m_offsetMon;
    int year = (t->tm_year % 100 + m_offsetYear + 100) % 100;
    if (day < 1) day = 1; if (day > 31) day = 31;
    if (mon < 1) mon = 1; if (mon > 12) mon = 12;

    const uint32_t base = 0x0FFD7110;
    m_physBus->write8(base + 0, toBcd(sec));
    m_physBus->write8(base + 1, toBcd(min));
    m_physBus->write8(base + 2, toBcd(hour) | 0x80); // 24h mode
    m_physBus->write8(base + 3, toBcd(day));
    m_physBus->write8(base + 4, toBcd(mon));
    m_physBus->write8(base + 5, toBcd(year));
}

void Mega65Rtc::getDeviceInfo(DeviceInfo& out) const {
    out.name = m_name;
    out.baseAddr = m_base;
    out.addrMask = 0x7F;

    // Show current time
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    int hour = (t->tm_hour + m_offsetHour) % 24;
    int min  = (t->tm_min  + m_offsetMin) % 60;
    int sec  = (t->tm_sec  + m_offsetSec) % 60;
    int year = (t->tm_year % 100 + m_offsetYear) % 100;
    int mon  = (t->tm_mon + 1 + m_offsetMon);
    int day  = t->tm_mday + m_offsetDay;

    char buf[32];
    snprintf(buf, sizeof(buf), "20%02d-%02d-%02d %02d:%02d:%02d",
             year, mon, day, hour, min, sec);
    out.state.push_back({"Time", buf});
    out.state.push_back({"Offset active", m_offsetActive ? "yes" : "no"});

    out.registers.push_back({"RTCSEC",   0x00, toBcd(sec),  "Seconds (BCD)"});
    out.registers.push_back({"RTCMIN",   0x01, toBcd(min),  "Minutes (BCD)"});
    out.registers.push_back({"RTCHOUR",  0x02, (uint8_t)(toBcd(hour) | 0x80), "Hours (BCD, 24h)"});
    out.registers.push_back({"RTCDAY",   0x03, toBcd(day),  "Day (BCD)"});
    out.registers.push_back({"RTCMONTH", 0x04, toBcd(mon),  "Month (BCD)"});
    out.registers.push_back({"RTCYEAR",  0x05, toBcd(year), "Year (BCD)"});
}
