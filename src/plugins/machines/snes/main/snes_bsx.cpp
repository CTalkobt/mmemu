// The Satellaview (BS-X), after Mesen2's BsxCart, BsxMemoryPack, BsxSatellaview
// and BsxStream (GPL-3.0, Sour and the Mesen contributors). See snes_bsx.h.

#include "snes_bsx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace snes {

// ---------------------------------------------------------------------------
// The memory pack
// ---------------------------------------------------------------------------

BsxPack::BsxPack(std::vector<uint8_t> data) : m_data(std::move(data)) {
    if (m_data.size() >= 1024) m_calculatedSize = (uint8_t)std::min(0x0C, (int)std::log2((double)(m_data.size() >> 10)));
}

uint8_t& BsxPack::at(uint32_t page, uint16_t a) { return m_data[page * 0x1000 + (a & 0xFFF)]; }

void BsxPack::reset() {
    m_enableCsr = m_enableEsr = m_enableVendorInfo = m_writeByte = false;
    m_command = 0;
}

uint8_t BsxPack::read(uint32_t page, uint16_t a, bool peek) {
    if (peek) return at(page, a);
    if (page == 0 && m_enableEsr) {
        switch (a & 0xFFF) {
        case 0x002: return 0xC0;
        case 0x004: return 0x82;
        }
    }
    if (m_enableCsr) {
        m_enableCsr = false;
        return 0x80;
    }
    if (m_enableVendorInfo && (a & 0x7FFF) >= 0x7F00 && (a & 0x7FFF) <= 0x7F13) {
        // The flash's vendor information: memory pack type 1
        switch (a & 0xFF) {
        case 0x00: return 0x4D;
        case 0x02: return 0x50;
        case 0x06: return (uint8_t)(0x10 | m_calculatedSize);
        default: return 0x00;
        }
    }
    return at(page, a);
}

void BsxPack::write(uint32_t page, uint16_t a, uint8_t v) {
    if (m_writeByte) {
        at(page, a) &= v;                      // flash: bits only clear
        m_writeByte = false;
    } else if (page == 0 && (a & 0xFFF) == 0) {
        command(v, 0);                         // (Mesen's handler for page 0 is in 64 KB page 0)
    }
}

void BsxPack::command(uint8_t v, uint32_t page) {
    m_command = (uint16_t)(m_command << 8 | v);
    switch (v) {
    case 0x00: case 0xFF: m_enableCsr = m_enableEsr = m_enableVendorInfo = false; break;
    case 0x10: case 0x40: m_writeByte = true; break;
    case 0x70: m_enableCsr = true; break;
    case 0x71: m_enableEsr = true; break;
    case 0x75: m_enableVendorInfo = true; break;
    }
    if (m_command == 0x20D0) {                 // page erase
        const size_t from = (size_t)page * 0x10000;
        if (from < m_data.size()) std::fill(m_data.begin() + (long)from, m_data.begin() + (long)std::min(m_data.size(), from + 0x10000), 0xFF);
    } else if (m_command == 0xA7D0) {          // chip erase
        std::fill(m_data.begin(), m_data.end(), 0xFF);
    }
}

// ---------------------------------------------------------------------------
// A stream
// ---------------------------------------------------------------------------

void BsxStream::reset(int64_t date, const std::string& dir) {
    m_file.close();
    m_dir = dir;
    m_channel = 0;
    m_prefix = m_data = m_status = 0;
    m_prefixLatch = m_dataLatch = m_firstPacket = false;
    m_fileOffset = 0;
    m_fileIndex = 0;
    m_queueLength = 0;
    m_prefixQueueLength = m_dataQueueLength = 0;
    m_activeChannel = 0;
    m_activeFileIndex = 0;
    m_date = date;
    m_tm = {};
}

bool BsxStream::fillQueues() {
    if (m_queueLength > 0) {
        --m_queueLength;
        if (m_prefixLatch && m_prefixQueueLength < 0x80) ++m_prefixQueueLength;
        if (m_dataLatch && m_dataQueueLength < 0x80) ++m_dataQueueLength;
    }
    return needUpdate();
}

bool BsxStream::loadFile() {
    m_activeChannel = m_channel;
    m_activeFileIndex = m_fileIndex;
    m_file.close();
    m_file.clear();
    char name[32];
    std::snprintf(name, sizeof name, "BSX%04X-%u.bin", m_activeChannel, (unsigned)m_activeFileIndex);
    m_file.open(m_dir + "/" + name, std::ios::binary);
    if (m_file) {
        m_firstPacket = true;
        m_file.seekg(0, std::ios::end);
        m_queueLength = (uint16_t)std::ceil((double)m_file.tellg() / 22.0);
        m_file.seekg(0, std::ios::beg);
        ++m_fileIndex;
        return true;
    }
    if (m_fileIndex > 0) {
        // Back to file 0 and try again
        m_fileIndex = 0;
        if (loadFile()) return true;
    }
    m_prefix |= 0x0F;                          // nothing on this channel
    return false;
}

uint8_t BsxStream::prefixCount() {
    if (!m_prefixLatch || !m_dataLatch) return 0;    // the stream is off
    if (m_prefixQueueLength == 0 && m_dataQueueLength == 0) {
        // Empty: the next data
        m_fileOffset = 0;
        if (m_channel == 0) {
            m_queueLength = 1;                 // the time channel
            m_firstPacket = true;
        } else {
            loadFile();
        }
    }
    return m_prefixQueueLength;
}

uint8_t BsxStream::prefix() {
    if (!m_prefixLatch) return 0;
    if (m_prefixQueueLength > 0) {
        m_prefix = 0;
        if (m_firstPacket) {
            m_prefix |= 0x10;
            m_firstPacket = false;
        }
        --m_prefixQueueLength;
        if (m_queueLength == 0 && m_prefixQueueLength == 0) m_prefix |= 0x80;   // the last packet
    }
    m_status |= m_prefix;
    return m_prefix;
}

uint8_t BsxStream::data(uint64_t master, uint64_t masterHz) {
    if (!m_dataLatch) return 0;
    if (m_dataQueueLength > 0) {
        if (m_channel == 0) {
            m_data = time(master, masterHz);
        } else if (m_file) {
            char c = 0;
            m_file.get(c);
            m_data = (uint8_t)c;
        }
        if (++m_fileOffset % 22 == 0) --m_dataQueueLength;    // a packet read
    }
    return m_data;
}

uint8_t BsxStream::status(bool reset) {
    const uint8_t v = m_status;
    if (reset) m_status = 0;
    return v;
}

void BsxStream::setChannelLow(uint8_t v) {
    if ((m_channel & 0xFF) != 0xFF) m_fileIndex = 0;
    m_channel = (uint16_t)((m_channel & 0xFF00) | v);
}

void BsxStream::setChannelHigh(uint8_t v) {
    if ((m_channel >> 8) != (v & 0x3F)) m_fileIndex = 0;
    m_channel = (uint16_t)((m_channel & 0xFF) | (v & 0x3F) << 8);
}

uint8_t BsxStream::time(uint64_t master, uint64_t masterHz) {
    if (m_fileOffset == 0) {
        const time_t t = (time_t)(m_date + (int64_t)(master / masterHz));
        localtime_r(&t, &m_tm);
        m_tm.tm_wday++;
        m_tm.tm_mon++;
        m_tm.tm_year += 1900;
    }
    switch (m_fileOffset) {
    case 4: return 0x10;                       // data group size (24-bit) $000010
    case 5: return 0x01;                       // must be 1
    case 6: return 0x01;                       // packets: 1
    case 10: return (uint8_t)m_tm.tm_sec;
    case 11: return (uint8_t)m_tm.tm_min;
    case 12: return (uint8_t)m_tm.tm_hour;
    case 13: return (uint8_t)m_tm.tm_wday;
    case 14: return (uint8_t)m_tm.tm_mday;
    case 15: return (uint8_t)m_tm.tm_mon;
    case 16: return (uint8_t)m_tm.tm_year;
    case 17: return (uint8_t)(m_tm.tm_year >> 8);
    default: return 0x00;
    }
}

// ---------------------------------------------------------------------------
// The cartridge and the Satellaview
// ---------------------------------------------------------------------------

Bsx::Bsx(const std::vector<uint8_t>* rom, std::vector<uint8_t>* sram, std::vector<uint8_t> pack)
    : m_rom(rom), m_sram(sram), m_pack(std::make_unique<BsxPack>(std::move(pack))), m_psram(PSRAM_SIZE, 0) {
    powerOn();
}

uint32_t Bsx::pagesOf(Kind k) const {
    switch (k) {
    case ROM: return (uint32_t)((m_rom->size() + 0xFFF) >> 12);
    case SRAM: return (uint32_t)((m_sram->size() + 0xFFF) >> 12);
    case PACK: return m_pack->pages();
    case PSRAM: return PSRAM_SIZE >> 12;
    case REGS: return 1;
    default: return 0;
    }
}

void Bsx::map(uint8_t b0, uint8_t b1, uint16_t a0, uint16_t a1, Kind k, uint16_t increment, uint16_t start) {
    const uint32_t n = pagesOf(k);
    if (!n) return;
    uint32_t page = start % n;
    for (uint32_t b = b0; b <= b1; ++b) {
        page = (page + increment) % n;
        for (uint32_t a = a0; a <= a1; a += 0x1000) {
            m_map[b << 4 | a >> 12] = {(uint8_t)k, page};
            if (++page >= n) page = 0;
        }
    }
}

void Bsx::unmap(uint8_t b0, uint8_t b1, uint16_t a0, uint16_t a1) {
    for (uint32_t b = b0; b <= b1; ++b)
        for (uint32_t a = a0; a < a1; a += 0x1000) m_map[b << 4 | a >> 12] = {};
}

void Bsx::powerOn() {
    for (auto& p : m_map) p = {};
    // The cartridge's LoROM map (Mesen's RegisterHandlers): ROM, and its RAM at $70-$7D/$F0-$FF
    map(0x00, 0x7D, 0x8000, 0xFFFF, ROM);
    map(0x80, 0xFF, 0x8000, 0xFFFF, ROM);
    if (m_rom->size() >= 0x200000) {
        map(0x70, 0x7D, 0x0000, 0x7FFF, SRAM);
        map(0xF0, 0xFF, 0x0000, 0x7FFF, SRAM);
    } else {
        map(0x70, 0x7D, 0x0000, 0xFFFF, SRAM);
        map(0xF0, 0xFF, 0x0000, 0xFFFF, SRAM);
    }
    // The BS-X's own: its registers at $00-$0F:5000, its RAM again at $10-$1F:5000
    map(0x00, 0x0F, 0x5000, 0x5FFF, REGS);
    map(0x10, 0x1F, 0x5000, 0x5FFF, SRAM);
    std::fill(m_psram.begin(), m_psram.end(), 0);
    reset();
}

void Bsx::reset() {
    for (auto& r : m_regs) r = true;
    m_regs[0x04] = m_regs[0x0A] = m_regs[0x0C] = m_regs[0x0D] = false;
    m_dirty = false;
    std::memcpy(m_dirtyRegs, m_regs, sizeof m_regs);
    m_pack->reset();
    satReset();
    updateMap();
}

void Bsx::satReset() {
    m_prevMaster = 0;
    m_streamReg = 0;
    m_extOutput = 0xFF;
    const int64_t date = m_customDate >= 0 ? m_customDate : (int64_t)std::time(nullptr);
    m_stream[0].reset(date, m_streamDir);
    m_stream[1].reset(date, m_streamDir);
}

void Bsx::updateMap() {
    const uint8_t unmappedBank = (uint8_t)(m_regs[0x0B] << 5);
    const uint8_t psRamBank = (uint8_t)(m_regs[0x05] << 4 | m_regs[0x06] << 5);
    if (!m_regs[0x02]) {
        // LoROM: the pack
        map(0x00, 0x7D, 0x8000, 0xFFFF, PACK);
        map(0x40, 0x7D, 0x0000, 0x7FFF, PACK);
        map(0x80, 0xFF, 0x8000, 0xFFFF, PACK);
        map(0xC0, 0xFF, 0x0000, 0x7FFF, PACK);
        // Its holes
        const uint16_t unmappedAddr = m_regs[0x0B] ? 0x0000 : 0x8000;
        if (m_regs[0x09]) unmap((uint8_t)(0x00 | unmappedBank << 1), (uint8_t)(0x1F | unmappedBank << 1), unmappedAddr, 0xFFFF);
        if (m_regs[0x0A]) unmap((uint8_t)(0x80 | unmappedBank << 1), (uint8_t)(0x9F | unmappedBank << 1), unmappedAddr, 0xFFFF);
        // The PSRAM
        const uint16_t psRamAddr = (psRamBank & 0x20) ? 0x0000 : 0x8000;
        if (m_regs[0x03]) {
            map((uint8_t)(0x00 | psRamBank << 1), (uint8_t)(0x0F | psRamBank << 1), psRamAddr, 0xFFFF, PSRAM);
            map(0x70, 0x7D, 0x0000, 0x7FFF, PSRAM);
        }
        if (m_regs[0x04]) {
            map((uint8_t)(0x80 | psRamBank << 1), (uint8_t)(0x8F | psRamBank << 1), psRamAddr, 0xFFFF, PSRAM);
            map(0xF0, 0xFF, 0x0000, 0x7FFF, PSRAM);
        }
    } else {
        // HiROM: the pack
        map(0x00, 0x3F, 0x8000, 0xFFFF, PACK, 8);
        map(0x40, 0x7D, 0x0000, 0xFFFF, PACK, 0);
        map(0x80, 0xBF, 0x8000, 0xFFFF, PACK, 8);
        map(0xC0, 0xFF, 0x0000, 0xFFFF, PACK, 0);
        // Its holes
        if (m_regs[0x09]) {
            unmap((uint8_t)(0x00 | unmappedBank), (uint8_t)(0x0F | unmappedBank), 0x8000, 0xFFFF);
            unmap((uint8_t)(0x40 | unmappedBank), (uint8_t)(0x4F | unmappedBank), 0x0000, 0xFFFF);
        }
        if (m_regs[0x0A]) {
            unmap((uint8_t)(0x80 | unmappedBank), (uint8_t)(0x8F | unmappedBank), 0x8000, 0xFFFF);
            unmap((uint8_t)(0xC0 | unmappedBank), (uint8_t)(0xCF | unmappedBank), 0x0000, 0xFFFF);
        }
        // The PSRAM
        if (m_regs[0x03]) {
            map((uint8_t)(0x00 | psRamBank), (uint8_t)(0x07 | psRamBank), 0x8000, 0xFFFF, PSRAM, 8);
            map((uint8_t)(0x40 | psRamBank), (uint8_t)(0x47 | psRamBank), 0x0000, 0xFFFF, PSRAM);
            map(0x20, 0x3F, 0x6000, 0x7FFF, PSRAM, 6);
        }
        if (m_regs[0x04]) {
            map((uint8_t)(0x80 | psRamBank), (uint8_t)(0x87 | psRamBank), 0x8000, 0xFFFF, PSRAM, 8);
            map((uint8_t)(0xC0 | psRamBank), (uint8_t)(0xC7 | psRamBank), 0x0000, 0xFFFF, PSRAM);
            map(0xA0, 0xBF, 0x6000, 0x7FFF, PSRAM, 6);
        }
    }
    // The BIOS, over the rest
    if (m_regs[0x07]) map(0x00, 0x3F, 0x8000, 0xFFFF, ROM);
    if (m_regs[0x08]) map(0x80, 0xBF, 0x8000, 0xFFFF, ROM);
}

int Bsx::read(uint8_t bank, uint16_t a, uint8_t openBus, bool peek) {
    const Page& p = m_map[(uint32_t)bank << 4 | a >> 12];
    const uint32_t off = p.index * 0x1000;
    switch (p.kind) {
    case ROM: {
        const uint32_t i = off + (a & 0xFFF);
        return i < m_rom->size() ? (*m_rom)[i] : 0;    // (Mesen pads the ROM to 4 KB)
    }
    case SRAM: case PSRAM: {
        auto& v = p.kind == SRAM ? *m_sram : m_psram;
        const uint32_t mask = v.size() - off < 0x1000 ? (uint32_t)(v.size() - off - 1) : 0xFFF;
        return v[off + (a & mask)];
    }
    case PACK: return m_pack->read(p.index, a, peek);
    case REGS: {
        if (a != 0x5000) return -1;
        const int r = bank & 0x0F;
        if (r <= 0x0D) return (uint8_t)(m_regs[r] << 7 | (openBus & 0x7F));
        return openBus & 0x7F;                         // $0E and $0F are write-only
    }
    default: return -1;
    }
}

void Bsx::write(uint8_t bank, uint16_t a, uint8_t v) {
    const Page& p = m_map[(uint32_t)bank << 4 | a >> 12];
    const uint32_t off = p.index * 0x1000;
    switch (p.kind) {
    case SRAM: case PSRAM: {
        auto& r = p.kind == SRAM ? *m_sram : m_psram;
        const uint32_t mask = r.size() - off < 0x1000 ? (uint32_t)(r.size() - off - 1) : 0xFFF;
        r[off + (a & mask)] = v;
        break;
    }
    case PACK: m_pack->write(p.index, a, v); break;
    case REGS: {
        if (a != 0x5000) return;
        const int reg = bank & 0x0F;
        if (reg == 0x0E) {
            // The pending changes take effect
            if (m_dirty) {
                std::memcpy(m_regs, m_dirtyRegs, sizeof m_regs);
                updateMap();
                m_dirty = false;
            }
        } else {
            const bool bit = v >> 7;
            if (m_regs[reg] != bit) {
                m_dirtyRegs[reg] = bit;
                m_dirty = true;
            }
        }
        break;
    }
    default: break;
    }
}

void Bsx::processClocks(uint64_t master, uint64_t masterHz) {
    if (m_stream[0].needUpdate() || m_stream[1].needUpdate()) {
        // 1000 packets a second (224 kbit/s)
        uint64_t gap = master - m_prevMaster;
        const uint64_t perPacket = masterHz / 1000;
        while (gap >= perPacket) {
            const bool more = m_stream[0].fillQueues() || m_stream[1].fillQueues();
            if (!more) { gap = 0; break; }
            gap -= perPacket;
        }
        m_prevMaster = master - gap;
    } else {
        m_prevMaster = master;
    }
}

int Bsx::satRead(uint8_t r, uint64_t master, uint64_t masterHz, bool peek) {
    if (r < 0x88 || r > 0x9A) return -1;
    if (peek) return 0;                                // (as Mesen: reading has effects)
    processClocks(master, masterHz);
    const bool resetStatus = m_streamReg & 0x01;
    switch (r) {
    case 0x88: return m_stream[0].channel() & 0xFF;
    case 0x89: return m_stream[0].channel() >> 8;
    case 0x8A: return m_stream[0].prefixCount();
    case 0x8B: return m_stream[0].prefix();
    case 0x8C: return m_stream[0].data(master, masterHz);
    case 0x8D: return m_stream[0].status(resetStatus);
    case 0x8E: return m_stream[1].channel() & 0xFF;
    case 0x8F: return m_stream[1].channel() >> 8;
    case 0x90: return m_stream[1].prefixCount();
    case 0x91: return m_stream[1].prefix();
    case 0x92: return m_stream[1].data(master, masterHz);
    case 0x93: return m_stream[1].status(resetStatus);
    case 0x94: return m_streamReg;                     // the LED and stream register
    case 0x95: return 0;
    case 0x96: return 0x10;                            // the Satellaview's status
    case 0x97: return m_extOutput;                     // Soundlink / EXT output
    case 0x98: return 0x80;                            // serial I/O (the serial number)
    case 0x99: return 0x01;
    default: return 0x10;                              // $219A
    }
}

bool Bsx::satWrite(uint8_t r, uint8_t v, uint64_t master, uint64_t masterHz) {
    if (r < 0x88 || r > 0x9F) return false;
    processClocks(master, masterHz);
    switch (r) {
    case 0x88: m_stream[0].setChannelLow(v); break;
    case 0x89: m_stream[0].setChannelHigh(v); break;
    case 0x8B: m_stream[0].setPrefixLatch(v); break;
    case 0x8C: m_stream[0].setDataLatch(v); break;
    case 0x8E: m_stream[1].setChannelLow(v); break;
    case 0x8F: m_stream[1].setChannelHigh(v); break;
    case 0x91: m_stream[1].setPrefixLatch(v); break;
    case 0x92: m_stream[1].setDataLatch(v); break;
    case 0x94: m_streamReg = v; break;
    case 0x97: m_extOutput = v; break;
    }
    return true;
}

}  // namespace snes
