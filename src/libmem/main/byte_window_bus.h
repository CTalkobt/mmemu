#pragma once

#include "ibus.h"
#include <cstdint>
#include <cstring>

/**
 * Read-only bus exposing a small window of bytes at a fixed address; every
 * other address reads as 0 and writes are ignored.
 *
 * Used to disassemble instruction bytes captured earlier (e.g. trace entries),
 * so the text reflects what executed even if memory has since changed.
 */
class ByteWindowBus : public IBus {
public:
    static constexpr int MAX_BYTES = 16;

    ByteWindowBus(uint32_t base, const uint8_t* bytes, int len, uint32_t addrMask = 0xFFFFFFFF)
        : m_base(base), m_len(len < 0 ? 0 : (len > MAX_BYTES ? MAX_BYTES : len)) {
        m_config.addrBits = 32;
        m_config.dataBits = 8;
        m_config.role = BusRole::CODE;
        m_config.littleEndian = true;
        m_config.addrMask = addrMask;
        std::memcpy(m_bytes, bytes, m_len);
    }

    const BusConfig& config() const override { return m_config; }
    const char*      name()   const override { return "window"; }

    uint8_t read8(uint32_t addr) override { return peek8(addr); }
    void    write8(uint32_t, uint8_t) override {}
    uint8_t peek8(uint32_t addr) override {
        uint32_t off = (addr - m_base) & m_config.addrMask;
        return off < (uint32_t)m_len ? m_bytes[off] : 0;
    }

    size_t stateSize() const override { return 0; }
    void   saveState(uint8_t*) const override {}
    void   loadState(const uint8_t*) override {}
    int    writeCount() const override { return 0; }
    void   getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void   clearWriteLog() override {}

private:
    BusConfig m_config;
    uint32_t  m_base;
    int       m_len;
    uint8_t   m_bytes[MAX_BYTES] = {};
};
