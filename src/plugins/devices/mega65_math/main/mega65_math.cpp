#include "mega65_math.h"
#include "include/mmemu_plugin_api.h"
#include <cstring>

// Offsets from base ($D700)
static constexpr int OFF_MATHBUSY = 0x0F; // $D70F — bit 7 = DIVBUSY
static constexpr int OFF_DIVINA   = 0x60; // $D760-$D763
static constexpr int OFF_DIVINB   = 0x64; // $D764-$D767
static constexpr int OFF_DIVOUT   = 0x68; // $D768-$D76F (8 bytes)
static constexpr int OFF_MULTINA  = 0x70; // $D770-$D773 (also div remainder)
static constexpr int OFF_MULTINB  = 0x74; // $D774-$D777
static constexpr int OFF_MULTOUT  = 0x78; // $D778-$D77F (8 bytes)
static constexpr int OFF_RNDREG   = 0xEF; // $D7EF — random byte
static constexpr int OFF_RNGSTAT  = 0xFE; // $D7FE — bit 7 = RNG not ready

static void store64(uint8_t* dst, uint64_t val) {
    for (int i = 0; i < 8; ++i)
        dst[i] = (val >> (i * 8)) & 0xFF;
}

static uint32_t load32(const uint8_t* src) {
    return src[0] | (uint32_t(src[1]) << 8)
         | (uint32_t(src[2]) << 16) | (uint32_t(src[3]) << 24);
}

Mega65MathDevice::Mega65MathDevice(uint32_t base) : m_base(base), m_rngState(0xACE1u) {
    std::memset(m_regs, 0, sizeof(m_regs));
    advanceRng();
}

void Mega65MathDevice::reset() {
    std::memset(m_regs, 0, sizeof(m_regs));
    m_rngState = 0xACE1u;
    advanceRng();
}

void Mega65MathDevice::advanceRng() {
    // 32-bit Galois LFSR (maximal-length, taps: 32,22,2,1)
    uint32_t lsb = m_rngState & 1;
    m_rngState >>= 1;
    if (lsb) m_rngState ^= 0xD0000001u;
    m_regs[OFF_RNDREG] = (uint8_t)(m_rngState & 0xFF);
    m_regs[OFF_RNGSTAT] = 0; // always ready in emulator
}

void Mega65MathDevice::computeMultiply() {
    uint64_t a = load32(&m_regs[OFF_MULTINA]);
    uint64_t b = load32(&m_regs[OFF_MULTINB]);
    store64(&m_regs[OFF_MULTOUT], a * b);
}

void Mega65MathDevice::computeDivide() {
    uint32_t dividend = load32(&m_regs[OFF_DIVINA]);
    uint32_t divisor  = load32(&m_regs[OFF_DIVINB]);

    uint64_t quotient  = 0;
    uint32_t remainder = 0;
    if (divisor != 0) {
        quotient  = dividend / divisor;
        remainder = dividend % divisor;
    }

    store64(&m_regs[OFF_DIVOUT], quotient);

    // Remainder goes to MULTINA ($D770)
    m_regs[OFF_MULTINA]     = remainder & 0xFF;
    m_regs[OFF_MULTINA + 1] = (remainder >> 8) & 0xFF;
    m_regs[OFF_MULTINA + 2] = (remainder >> 16) & 0xFF;
    m_regs[OFF_MULTINA + 3] = (remainder >> 24) & 0xFF;

    // DIVBUSY = 0 (bit 7 of $D70F) — already 0 since we compute instantly
}

bool Mega65MathDevice::ioRead(IBus* /*bus*/, uint32_t addr, uint8_t* val) {
    uint32_t off = addr - m_base;
    if (off >= 256) return false;
    // $D700-$D70F belong to the F018B DMA controller — don't claim them
    if (off <= 0x0F) return false; // $D700-$D70F reserved for F018B DMA
    if (off == OFF_RNDREG) {
        *val = m_regs[OFF_RNDREG];
        advanceRng(); // next read gets a new value
        return true;
    }
    *val = m_regs[off];
    return true;
}

bool Mega65MathDevice::ioWrite(IBus* /*bus*/, uint32_t addr, uint8_t val) {
    uint32_t off = addr - m_base;
    if (off >= 256) return false;
    // $D700-$D70F belong to the F018B DMA controller — don't claim them
    if (off <= 0x0F) return false; // $D700-$D70F reserved for F018B DMA
    if (off == OFF_RNDREG || off == OFF_RNGSTAT)
        return true; // read-only registers, ignore writes

    m_regs[off] = val;

    // Recompute on writes to input registers
    if (off >= OFF_MULTINA && off < OFF_MULTINA + 4)
        computeMultiply();
    if (off >= OFF_MULTINB && off < OFF_MULTINB + 4)
        computeMultiply();
    if (off >= OFF_DIVINA && off < OFF_DIVINA + 4)
        computeDivide();
    if (off >= OFF_DIVINB && off < OFF_DIVINB + 4)
        computeDivide();

    return true;
}
