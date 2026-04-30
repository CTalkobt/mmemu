#pragma once
#include "libdevices/main/io_handler.h"
#include <cstring>
#include <string>

/**
 * MEGA65 Hardware Math Accelerator + RNG
 *
 * Register map (base $D700, 256 bytes):
 *
 *   $D70F        MATHBUSY — bit 7: DIVBUSY (0 = result valid, always 0 in emu)
 *
 *   $D760-$D763  DIVINA   — Dividend (32-bit LE)
 *   $D764-$D767  DIVINB   — Divisor  (32-bit LE)
 *   $D768-$D76F  DIVOUT   — Quotient (64-bit LE, valid when DIVBUSY=0)
 *
 *   $D770-$D773  MULTINA  — Multiplicand / Div Remainder (32-bit LE)
 *   $D774-$D777  MULTINB  — Multiplier (32-bit LE)
 *   $D778-$D77F  MULTOUT  — Product (64-bit LE, always valid)
 *
 *   $D7EF        RNDREG   — Random byte (read advances LFSR)
 *   $D7FE        RNGSTAT  — bit 7: 1 = RNG not ready (always 0 in emu)
 */
class Mega65MathDevice : public IOHandler {
public:
    explicit Mega65MathDevice(uint32_t base = 0xD700);
    virtual ~Mega65MathDevice() = default;

    const char* name() const override { return m_name.c_str(); }
    uint32_t    baseAddr() const override { return m_base; }
    uint32_t    addrMask() const override { return 0xFF; } // 256 bytes

    void setName(const std::string& n) override { m_name = n; }
    void setBaseAddr(uint32_t a) override { m_base = a; }

    void reset() override;
    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override;
    void tick(uint64_t /*cycles*/) override {}

private:
    void computeMultiply();
    void computeDivide();
    void advanceRng();

    uint32_t m_base;
    std::string m_name{"MEGA65 Math"};
    uint8_t m_regs[256];
    uint32_t m_rngState;  // LFSR state
};
