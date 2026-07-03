#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>
#include <string>

/**
 * MemoryHeatMap — tracks read/write frequency per address.
 *
 * Shared data layer for memory heat map visualization (#25).
 * Usable from GUI (color grid), CLI (text table), and MCP (JSON).
 *
 * Attach to DebugContext which calls recordRead/recordWrite from
 * its onMemoryRead/onMemoryWrite observer callbacks.
 */
class MemoryHeatMap {
public:
    explicit MemoryHeatMap(uint32_t addrSpaceSize = 0x10000)
        : m_reads(addrSpaceSize, 0), m_writes(addrSpaceSize, 0),
          m_size(addrSpaceSize) {}

    void recordRead(uint32_t addr) {
        if (addr < m_size) m_reads[addr]++;
    }

    void recordWrite(uint32_t addr) {
        if (addr < m_size) m_writes[addr]++;
    }

    uint32_t readCount(uint32_t addr) const {
        return (addr < m_size) ? m_reads[addr] : 0;
    }

    uint32_t writeCount(uint32_t addr) const {
        return (addr < m_size) ? m_writes[addr] : 0;
    }

    uint32_t totalCount(uint32_t addr) const {
        return readCount(addr) + writeCount(addr);
    }

    uint32_t maxReads() const {
        return m_reads.empty() ? 0 : *std::max_element(m_reads.begin(), m_reads.end());
    }

    uint32_t maxWrites() const {
        return m_writes.empty() ? 0 : *std::max_element(m_writes.begin(), m_writes.end());
    }

    uint32_t size() const { return m_size; }

    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool en) { m_enabled = en; }

    void reset() {
        std::fill(m_reads.begin(), m_reads.end(), 0);
        std::fill(m_writes.begin(), m_writes.end(), 0);
    }

    struct HotSpot {
        uint32_t addr;
        uint32_t reads;
        uint32_t writes;
        uint32_t total;
    };

    /// Return the top N addresses by total access count.
    std::vector<HotSpot> topAddresses(int n, uint32_t startAddr = 0, uint32_t endAddr = 0) const {
        if (endAddr == 0) endAddr = m_size;
        std::vector<HotSpot> spots;
        for (uint32_t a = startAddr; a < endAddr && a < m_size; ++a) {
            uint32_t t = m_reads[a] + m_writes[a];
            if (t > 0) spots.push_back({a, m_reads[a], m_writes[a], t});
        }
        std::sort(spots.begin(), spots.end(),
                  [](const HotSpot& a, const HotSpot& b) { return a.total > b.total; });
        if ((int)spots.size() > n) spots.resize(n);
        return spots;
    }

    /// Get heat value for a page (256-byte block), normalized 0.0-1.0.
    /// Mode: 0=total, 1=reads only, 2=writes only
    double pageHeat(uint32_t page, int mode = 0) const {
        uint32_t base = page * 256;
        if (base >= m_size) return 0;
        uint32_t sum = 0;
        uint32_t end = std::min(base + 256, m_size);
        for (uint32_t a = base; a < end; ++a) {
            if (mode == 1) sum += m_reads[a];
            else if (mode == 2) sum += m_writes[a];
            else sum += m_reads[a] + m_writes[a];
        }
        return sum > 0 ? std::min(1.0, (double)sum / 10000.0) : 0;
    }

private:
    std::vector<uint32_t> m_reads;
    std::vector<uint32_t> m_writes;
    uint32_t m_size;
    bool m_enabled = false;
};
