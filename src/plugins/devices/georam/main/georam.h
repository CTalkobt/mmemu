#pragma once

#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <string>
#include <vector>

/**
 * GeoRAM / NeoRAM (Berkeley Softworks' RAM expansion for GEOS): its RAM seen
 * a 256-byte page at a time through I/O1 ($DE00-$DEFF). After VICE's
 * src/c64/cart/georam.c (GPL-2.0-or-later, the VICE team).
 *
 * - $DFFE: the page within a 16 KB block (0-63); $DFFF: the block (taken
 *   modulo the blocks fitted). Both are write-only (reads aren't driven) and
 *   repeat in pairs from $DF80.
 * - Sizes 512K, 1M, 2M, 4M (VICE: smaller units never existed).
 * - A reset clears the two registers; the RAM stays (VICE georam_reset).
 */
class GeoRam : public IOHandler {
public:
    explicit GeoRam(uint32_t sizeBytes = 512 * 1024);

    /** 512K, 1M, 2M or 4M; false otherwise. Clears the RAM. */
    bool setSize(uint32_t bytes);
    uint32_t size() const { return (uint32_t)m_ram.size(); }
    bool loadImage(const std::string& path, std::string& err);
    bool saveImage(const std::string& path, std::string& err) const;
    std::vector<uint8_t>& ram() { return m_ram; }
    uint8_t page() const { return m_page; }
    uint8_t block() const { return m_block; }

    // IOHandler
    void setName(const std::string& n) override { m_name = n; }
    const char* name() const override { return m_name.c_str(); }
    uint32_t baseAddr() const override { return 0xDE00; }
    uint32_t addrMask() const override { return 0x1FF; }
    bool ioRead(IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioPeek(IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override;
    void reset() override { m_page = m_block = 0; }
    void tick(uint64_t) override {}
    bool setOption(const std::string& key, const std::string& value, std::string& err) override;
    bool saveDeviceImage(const std::string& path, std::string& err) override { return saveImage(path, err); }
    void getDeviceInfo(DeviceInfo& out) const override;

private:
    size_t offset(uint32_t addr) const { return (size_t)m_block * 16384 + (size_t)m_page * 256 + (addr & 0xFF); }

    std::string m_name = "GeoRAM";
    std::vector<uint8_t> m_ram;
    uint8_t m_page = 0, m_block = 0;
};
