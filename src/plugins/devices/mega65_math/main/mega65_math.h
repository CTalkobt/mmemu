#pragma once
#include "libdevices/main/io_handler.h"
#include <cstring>
#include <string>

/**
 * MEGA65 Hardware Math Accelerator
 *
 * Emulates the 32-bit multiply and divide unit mapped at $D760-$D77F.
 *
 * Register map (active bytes used by cc45 highlighted):
 *
 *   Divider:
 *     $D760-$D763  Dividend  (32-bit LE, cc45 uses low 16)
 *     $D764-$D767  Divisor   (32-bit LE, cc45 uses low 16)
 *     $D768-$D76B  Quotient  (32-bit LE, read-only, cc45 reads low 16)
 *
 *   Scratch:
 *     $D76E        Sign byte (read/write, used by signed math)
 *
 *   Multiplier:
 *     $D770-$D773  Input A / Div Remainder (32-bit LE)
 *     $D774-$D777  Input B   (32-bit LE)
 *     $D778-$D77B  Product   (32-bit LE, read-only)
 *
 * Multiply result is available immediately after writing Input B.
 * Divide result is available immediately after writing Divisor.
 * (Real hardware has a ~20-cycle latency; we compute instantly.)
 */
class Mega65MathDevice : public IOHandler {
public:
    explicit Mega65MathDevice(uint32_t base = 0xD760);
    virtual ~Mega65MathDevice() = default;

    const char* name() const override { return m_name.c_str(); }
    uint32_t    baseAddr() const override { return m_base; }
    uint32_t    addrMask() const override { return 0x1F; } // 32 bytes: $D760-$D77F

    void setName(const std::string& n) override { m_name = n; }
    void setBaseAddr(uint32_t a) override { m_base = a; }

    void reset() override;
    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override;
    void tick(uint64_t /*cycles*/) override {}

private:
    void computeMultiply();
    void computeDivide();

    uint32_t m_base;
    std::string m_name{"MEGA65 Math"};

    // Register file: 32 bytes at offsets 0x00-0x1F
    // Offset from base:
    //   0x00-0x03  dividend     (div input A)
    //   0x04-0x07  divisor      (div input B)
    //   0x08-0x0B  quotient     (div output, read-only)
    //   0x0C-0x0F  (reserved)
    //   0x0E       sign scratch (read/write)
    //   0x10-0x13  mul input A / div remainder
    //   0x14-0x17  mul input B
    //   0x18-0x1B  product      (mul output, read-only)
    //   0x1C-0x1F  (reserved)
    uint8_t m_regs[32];
};
