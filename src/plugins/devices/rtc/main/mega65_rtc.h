#pragma once
#include "libdevices/main/io_handler.h"
#include "libdevices/main/device_info.h"
#include <cstdint>
#include <ctime>
#include <string>

/**
 * MEGA65 Real-Time Clock
 *
 * Emulates the I2C RTC accessible at $D710-$D71F (I/O mapped from $FFD7110).
 * On read, returns host system time in BCD format.
 * On write, stores an offset from the host clock (machine state only,
 * does not affect the host system time).
 *
 * Register map (base $D710):
 *   $D710 ($FFD7110): RTCSEC   — seconds (BCD, 0-59)
 *   $D711 ($FFD7111): RTCMIN   — minutes (BCD, 0-59)
 *   $D712 ($FFD7112): RTCHOUR  — hours (BCD), bit 7=24h mode
 *   $D713 ($FFD7113): RTCDAY   — day of month (BCD, 1-31)
 *   $D714 ($FFD7114): RTCMONTH — month (BCD, 1-12)
 *   $D715 ($FFD7115): RTCYEAR  — year (BCD, 0-99)
 *   $D716-$D71F:      reserved / day-of-week
 *   $D740-$D77F:      64 bytes NVRAM (battery-backed config storage)
 */
class Mega65Rtc : public IOHandler {
public:
    explicit Mega65Rtc(uint32_t base = 0xD710);
    ~Mega65Rtc() override = default;

    const char* name()     const override { return m_name.c_str(); }
    uint32_t    baseAddr() const override { return m_base; }
    uint32_t    addrMask() const override { return 0x7F; } // $D710-$D77F

    void setName(const std::string& n) override { m_name = n; }
    void setBaseAddr(uint32_t a) override { m_base = a; }

    void reset() override;
    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t  val) override;
    void tick(uint64_t cycles) override;

    // Set the physical bus for direct mapping at $FFD7110
    void setPhysBus(IBus* bus) { m_physBus = bus; }
    void getDeviceInfo(DeviceInfo& out) const override;

private:
    static uint8_t toBcd(int val) { return (uint8_t)(((val / 10) << 4) | (val % 10)); }
    static int fromBcd(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }

    uint32_t    m_base;
    std::string m_name{"RTC"};

    // Time offset: added to host time when reading, set by writes.
    // Allows the machine to "set" the clock without affecting the host.
    int         m_offsetSec  = 0;
    int         m_offsetMin  = 0;
    int         m_offsetHour = 0;
    int         m_offsetDay  = 0;
    int         m_offsetMon  = 0;
    int         m_offsetYear = 0;
    bool        m_offsetActive = false;

    // NVRAM (64 bytes, battery-backed config storage)
    uint8_t     m_nvram[64];
    IBus*       m_physBus = nullptr;
    uint64_t    m_tickAccum = 0;
};
