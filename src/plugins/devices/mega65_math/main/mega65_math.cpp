#include "mega65_math.h"
#include "include/mmemu_plugin_api.h"
#include <cstring>

// Offset constants (from $D760 base)
static constexpr int OFF_DIVIDEND  = 0x00; // $D760-$D763
static constexpr int OFF_DIVISOR   = 0x04; // $D764-$D767
static constexpr int OFF_QUOTIENT  = 0x08; // $D768-$D76B
static constexpr int OFF_SIGN      = 0x0E; // $D76E
static constexpr int OFF_MULA      = 0x10; // $D770-$D773 (also div remainder)
static constexpr int OFF_MULB      = 0x14; // $D774-$D777
static constexpr int OFF_PRODUCT   = 0x18; // $D778-$D77B

Mega65MathDevice::Mega65MathDevice(uint32_t base) : m_base(base) {
    std::memset(m_regs, 0, sizeof(m_regs));
}

void Mega65MathDevice::reset() {
    std::memset(m_regs, 0, sizeof(m_regs));
}

void Mega65MathDevice::computeMultiply() {
    uint32_t a = m_regs[OFF_MULA]
               | (uint32_t(m_regs[OFF_MULA + 1]) << 8)
               | (uint32_t(m_regs[OFF_MULA + 2]) << 16)
               | (uint32_t(m_regs[OFF_MULA + 3]) << 24);
    uint32_t b = m_regs[OFF_MULB]
               | (uint32_t(m_regs[OFF_MULB + 1]) << 8)
               | (uint32_t(m_regs[OFF_MULB + 2]) << 16)
               | (uint32_t(m_regs[OFF_MULB + 3]) << 24);
    uint32_t product = a * b;
    m_regs[OFF_PRODUCT]     = product & 0xFF;
    m_regs[OFF_PRODUCT + 1] = (product >> 8) & 0xFF;
    m_regs[OFF_PRODUCT + 2] = (product >> 16) & 0xFF;
    m_regs[OFF_PRODUCT + 3] = (product >> 24) & 0xFF;
}

void Mega65MathDevice::computeDivide() {
    uint32_t dividend = m_regs[OFF_DIVIDEND]
                      | (uint32_t(m_regs[OFF_DIVIDEND + 1]) << 8)
                      | (uint32_t(m_regs[OFF_DIVIDEND + 2]) << 16)
                      | (uint32_t(m_regs[OFF_DIVIDEND + 3]) << 24);
    uint32_t divisor  = m_regs[OFF_DIVISOR]
                      | (uint32_t(m_regs[OFF_DIVISOR + 1]) << 8)
                      | (uint32_t(m_regs[OFF_DIVISOR + 2]) << 16)
                      | (uint32_t(m_regs[OFF_DIVISOR + 3]) << 24);

    uint32_t quotient = 0;
    uint32_t remainder = 0;
    if (divisor != 0) {
        quotient  = dividend / divisor;
        remainder = dividend % divisor;
    }

    m_regs[OFF_QUOTIENT]     = quotient & 0xFF;
    m_regs[OFF_QUOTIENT + 1] = (quotient >> 8) & 0xFF;
    m_regs[OFF_QUOTIENT + 2] = (quotient >> 16) & 0xFF;
    m_regs[OFF_QUOTIENT + 3] = (quotient >> 24) & 0xFF;

    // Remainder goes to $D770 (same as mul input A)
    m_regs[OFF_MULA]     = remainder & 0xFF;
    m_regs[OFF_MULA + 1] = (remainder >> 8) & 0xFF;
    m_regs[OFF_MULA + 2] = (remainder >> 16) & 0xFF;
    m_regs[OFF_MULA + 3] = (remainder >> 24) & 0xFF;
}

bool Mega65MathDevice::ioRead(IBus* /*bus*/, uint32_t addr, uint8_t* val) {
    uint32_t off = addr - m_base;
    if (off >= 32) return false;
    *val = m_regs[off];
    return true;
}

bool Mega65MathDevice::ioWrite(IBus* /*bus*/, uint32_t addr, uint8_t val) {
    uint32_t off = addr - m_base;
    if (off >= 32) return false;

    m_regs[off] = val;

    // Trigger computation when the last byte of an input register is written.
    // The compiler writes low-to-high, so the last STA triggers the result.
    // Multiply: triggered by write to highest byte of Input B ($D777, offset 0x17)
    // Divide:   triggered by write to highest byte of Divisor ($D767, offset 0x07)
    //
    // For maximum compatibility, recompute on any write to the input range.
    if (off >= OFF_MULA && off < OFF_MULA + 4) {
        // Input A changed — recompute both (mul uses A, div remainder lives here)
        computeMultiply();
    }
    if (off >= OFF_MULB && off < OFF_MULB + 4) {
        computeMultiply();
    }
    if (off >= OFF_DIVIDEND && off < OFF_DIVIDEND + 4) {
        computeDivide();
    }
    if (off >= OFF_DIVISOR && off < OFF_DIVISOR + 4) {
        computeDivide();
    }

    return true;
}

// --- Plugin boilerplate ---

static IOHandler* createMega65Math() {
    return new Mega65MathDevice();
}

static DevicePluginInfo s_devices[] = {
    {"mega65_math", createMega65Math}
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "mega65_math",
    "MEGA65 Math Accelerator",
    "1.0.0",
    nullptr, nullptr,           // deps, supportedMachineIds
    0, nullptr,                 // cores
    0, nullptr,                 // toolchains
    1, s_devices,               // 1 device
    0, nullptr,                 // machines
    0, nullptr,                 // loaders
    0, nullptr                  // cartridges
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    (void)host;
    return &s_manifest;
}
