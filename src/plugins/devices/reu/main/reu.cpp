#include "reu.h"
#include "ram_init_pattern.h"
#include "libmem/main/ibus.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace {
enum { REG_STATUS, REG_COMMAND, REG_HOST_LO, REG_HOST_HI, REG_REU_LO, REG_REU_HI, REG_BANK,
       REG_LEN_LO, REG_LEN_HI, REG_INT, REG_ADDR_CTL, REG_FIRST_UNUSED };
enum { TO_REU, FROM_REU, SWAP, VERIFY };

std::string sizeName(uint32_t bytes) {
    return bytes >= (1u << 20) ? std::to_string(bytes >> 20) + " MB" : std::to_string(bytes >> 10) + " KB";
}
}  // namespace

Reu::Reu(uint32_t sizeBytes) {
    if (!setSize(sizeBytes)) setSize(512 * 1024);
    reset();
}

bool Reu::setSize(uint32_t bytes) {
    switch (bytes >> 10) {
        case 128: case 256: case 512: case 1024: case 2048: case 4096: case 8192: case 16384: break;
        default: return false;
    }
    if (bytes % 1024) return false;
    m_ram.assign(bytes, 0);
    initRam();
    // VICE set_reu_size
    m_wrap = m_dramWrap = 0x80000;          // the REC wraps at 512 KB...
    m_bankUnused = 0xF8;
    m_statusPreset = ST_256K;
    if (bytes == 128 * 1024) {              // ...the 1700 (64 Kbit chips) at 128 KB
        m_statusPreset = 0;
        m_wrap = m_dramWrap = 0x20000;
    } else if (bytes > 512 * 1024) {        // the bank's upper bits: a latch to the DRAM
        m_bankUnused = 0;
        m_dramWrap = bytes;
    }
    m_status = (uint8_t)((m_status & ~ST_256K) | m_statusPreset);
    return true;
}

bool Reu::loadImage(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open '" + path + "'"; return false; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!setSize((uint32_t)data.size())) {
        err = "'" + path + "' is " + std::to_string(data.size()) + " bytes; an REU image is 128K, 256K, 512K, 1M, 2M, 4M, 8M or 16M";
        return false;
    }
    m_ram = std::move(data);
    return true;
}

bool Reu::saveImage(const std::string& path, std::string& err) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { err = "cannot write '" + path + "'"; return false; }
    f.write((const char*)m_ram.data(), (std::streamsize)m_ram.size());
    if (!f) { err = "write to '" + path + "' failed"; return false; }
    return true;
}

bool Reu::setOption(const std::string& key, const std::string& value, std::string& err) {
    if (key == "size") {
        // "512K", "1M", "16MB"
        uint64_t n = 0;
        size_t i = 0;
        while (i < value.size() && isdigit((unsigned char)value[i])) n = n * 10 + (uint64_t)(value[i++] - '0');
        std::string unit = value.substr(i);
        for (auto& c : unit) c = (char)toupper((unsigned char)c);
        if (unit == "K" || unit == "KB") n <<= 10;
        else if (unit == "M" || unit == "MB") n <<= 20;
        else if (!unit.empty()) n = 0;
        if (i == 0 || n > (16u << 20) || !setSize((uint32_t)n)) {
            err = "an REU is 128K (1700), 256K (1764), 512K (1750), 1M, 2M, 4M, 8M or 16M";
            return false;
        }
        reset();
        return true;
    }
    if (key == "image") {
        if (!loadImage(value, err)) return false;
        reset();
        return true;
    }
    if (key == "glue") {
        // The board: a C64C's custom-IC glue logic lets a DMA read made as
        // BA falls see the RAM under I/O (VICE testprogs REU/reutiming2)
        if (value == "c64c") m_c64cGlue = true;
        else if (value == "c64" || value == "breadbin") m_c64cGlue = false;
        else { err = "the REU's glue is c64 (breadbin) or c64c"; return false; }
        return true;
    }
    err = "the REU's options are a size (128K-16M), an image file, or glue (c64, c64c)";
    return false;
}

// Power-up contents (VICE reu_init_ram): the stripes measured on a 1764,
// then blocks inverted as x1541 saw. With banks 4-7 of a 256K unit
// unconnected, what reads there depends on them (testprogs REU/mirrors)
void Reu::initRam() {
    static const RamInitParam REU_RAM{255, 2, 1, 0x100, 255};
    ramInitPattern(m_ram.data(), m_ram.size(), REU_RAM);
    auto invert = [&](uint32_t a, uint32_t len) {
        for (uint32_t e = a + len; a < e && a < m_ram.size(); ++a) m_ram[a] ^= 0xFF;
    };
    for (uint32_t b = 0; b < (uint32_t)(m_ram.size() >> 16); b += 4) {
        for (uint32_t i = 0; i < 2; ++i) {
            invert(0x002A00 + ((i + b) << 16), 0x2A00);
            invert(0x008000 + ((i + b) << 16), 0x2C00);
            invert(0x00D600 + ((i + b) << 16), 0x2A00);
        }
        for (uint32_t i = 0; i < 2; ++i) {
            invert(0x020000 + ((i + b) << 16), 0x2A00);
            invert(0x025400 + ((i + b) << 16), 0x2C00);
            invert(0x02AC00 + ((i + b) << 16), 0x2A00);
        }
    }
}

void Reu::reset() {
    // VICE reu_reset: the registers; the RAM stays
    m_status = m_statusPreset;
    m_command = CMD_NO_FF00;
    m_host = m_hostShadow = m_reuAddr = m_reuShadow = 0;
    m_len = m_lenShadow = 0xFFFF;
    m_bank = m_bankShadow = m_bankUnused;
    m_intMask = INT_UNUSED;
    m_addrCtl = AC_UNUSED;
    m_armed = m_active = false;
    setIrq(false);
}

uint8_t Reu::reg(int r) const {
    switch (r) {
        case REG_STATUS:   return m_status;
        case REG_COMMAND:  return m_command;
        case REG_HOST_LO:  return (uint8_t)m_host;
        case REG_HOST_HI:  return (uint8_t)(m_host >> 8);
        case REG_REU_LO:   return (uint8_t)m_reuAddr;
        case REG_REU_HI:   return (uint8_t)(m_reuAddr >> 8);
        case REG_BANK:     return (uint8_t)(m_bank | m_bankUnused);
        case REG_LEN_LO:   return (uint8_t)m_len;
        case REG_LEN_HI:   return (uint8_t)(m_len >> 8);
        case REG_INT:      return m_intMask;
        case REG_ADDR_CTL: return m_addrCtl;
        default:           return 0xFF;
    }
}

bool Reu::ioPeek(IBus*, uint32_t addr, uint8_t* val) {
    if ((addr & 0xFF00) != 0xDF00) return false;
    *val = reg((int)(addr & 0x1F));
    return true;
}

bool Reu::ioRead(IBus*, uint32_t addr, uint8_t* val) {
    if ((addr & 0xFF00) != 0xDF00) return false;
    if (m_active) return false;                     // the REC doesn't drive the bus during its DMA
    const int r = (int)(addr & 0x1F);
    *val = reg(r);
    if (r == REG_STATUS) {
        m_status &= (uint8_t)~(ST_VERIFY | ST_EOB | ST_IRQ);
        setIrq(false);
    } else if (r == REG_BANK) {
        *val |= 0xF8;                               // the latch's upper bits don't read back
    }
    return true;
}

bool Reu::ioWrite(IBus*, uint32_t addr, uint8_t v) {
    if ((addr & 0xFFFF) == 0xFF00) {
        // The write that starts the transfer goes on to whatever is there; the
        // REU then has the bus at once (AEC), so a read-modify-write
        // instruction's second write never reaches memory (testprogs
        // REU/rmw-trigger: INC $FF00 starts it and leaves RAM as it was)
        if (m_active) return true;
        ff00Write();
        return false;
    }
    if ((addr & 0xFF00) != 0xDF00) return false;
    if (m_active) return true;
    switch ((int)(addr & 0x1F)) {
        case REG_COMMAND:
            m_command = v;
            if (v & CMD_EXEC) {
                if (v & CMD_NO_FF00) start();
                else m_armed = true;
            }
            break;
        case REG_HOST_LO: m_host = m_hostShadow = (uint16_t)((m_hostShadow & 0xFF00) | v); break;
        case REG_HOST_HI: m_host = m_hostShadow = (uint16_t)((m_hostShadow & 0x00FF) | (v << 8)); break;
        case REG_REU_LO:  m_reuAddr = m_reuShadow = (uint16_t)((m_reuShadow & 0xFF00) | v); break;
        case REG_REU_HI:  m_reuAddr = m_reuShadow = (uint16_t)((m_reuShadow & 0x00FF) | (v << 8)); break;
        case REG_BANK:    m_bank = m_bankShadow = (uint8_t)(v & ~m_bankUnused); break;
        case REG_LEN_LO:  m_len = m_lenShadow = (uint16_t)((m_lenShadow & 0xFF00) | v); break;
        case REG_LEN_HI:  m_len = m_lenShadow = (uint16_t)((m_lenShadow & 0x00FF) | (v << 8)); break;
        case REG_INT:
            m_intMask = (uint8_t)(v | INT_UNUSED);
            // An enabled condition already pending raises the IRQ now
            if (((m_intMask & (INT_ON | INT_EOB)) == (INT_ON | INT_EOB) && (m_status & ST_EOB)) ||
                ((m_intMask & (INT_ON | INT_VERIFY)) == (INT_ON | INT_VERIFY) && (m_status & ST_VERIFY))) {
                m_status |= ST_IRQ;
                setIrq(true);
            }
            break;
        case REG_ADDR_CTL: m_addrCtl = (uint8_t)(v | AC_UNUSED); break;
        default: break;
    }
    return true;
}

void Reu::ff00Write() {
    if (m_armed && !m_active) {
        m_armed = false;
        start();
    }
}

void Reu::start() {
    m_armed = false;
    m_active = true;
    m_type = m_command & 3;
    m_dHost = m_host;
    m_dReu = (uint32_t)m_reuAddr | ((uint32_t)m_bank << 16);
    m_left = m_len ? m_len : 0x10000;
    m_hostStep = (m_addrCtl & AC_FIX_C64) ? 0 : 1;
    m_reuStep = (m_addrCtl & AC_FIX_REU) ? 0 : 1;
    m_swapRead = false;
    m_firstByte = true;
}

uint8_t Reu::baHistory() const {
    uint8_t h = 0;
    for (int n = 0; n < 4; ++n)
        if (m_ba->baLowForDma(-n)) h |= (uint8_t)(1 << n);
    return h;
}

// A read of the C64 side; on a C64C, one made while BA is low gets the RAM
// under I/O (Denise Reu::stash, Reu::swap)
uint8_t Reu::hostRead(bool baLow) {
    syncTo(0);
    if (baLow && m_c64cGlue && m_ram64) return m_ram64[m_dHost];
    return m_bus->read8(m_dHost);
}

// The REC's 19-bit counter wraps at 512 KB (128 KB on the 1700); the bank's
// upper bits are outside it (VICE increment_reu_with_wrap_around)
uint32_t Reu::step(uint32_t a) const {
    uint32_t next = (a & 0x7FFFF) + (uint32_t)m_reuStep;
    if (next == m_wrap) next = 0;
    return (a & 0xF80000) | next;
}

uint8_t Reu::ramRead(uint32_t a) const {
    a &= m_dramWrap - 1;
    return a < m_ram.size() ? m_ram[a] : m_floating;
}

void Reu::ramWrite(uint32_t a, uint8_t v) {
    a &= m_dramWrap - 1;
    if (a < m_ram.size()) m_ram[a] = v;
}

int Reu::busMasterCycle() {
    if (!m_active) return 0;
    if (!m_bus) { m_active = false; return 0; }
    auto advance = [&]() {
        m_dHost = (uint16_t)(m_dHost + m_hostStep);
        m_dReu = step(m_dReu);
        --m_left;
    };
    if (m_type == SWAP && m_ba) {
        // Denise Reu::swap: read and write cycles alternate, whether or not
        // the access happens. A read goes ahead unless BA has been low for
        // four cycles (the first byte not in the cycle BA falls); a write
        // waits for BA, but for the last byte. A byte read again overwrites
        // the one before, so the read that counts is the last before BA
        // ends: on a badline, cycle 14 (VICE testprogs REU/reutiming2 e4-g4)
        const uint8_t h = baHistory();
        if (!m_swapRead) {
            if ((h & 3) == 1 && m_firstByte) return 1;
            if ((h & 15) != 15) {
                m_swapReu = ramRead(m_dReu);
                m_swapC64 = hostRead(h & 1);
            }
        } else if (!(h & 1) || m_left == 1) {
            ramWrite(m_dReu, m_swapC64);
            syncTo(0);
            m_bus->write8(m_dHost, m_swapReu);
            m_firstByte = false;
            advance();
            if (!m_left) { finish(m_dHost, m_dReu, 1, ST_EOB); return 1; }
        }
        m_swapRead = !m_swapRead;
        return 1;
    }
    // The VIC-II has the bus (VICE reu.c reu_ba): a read waits; a write to
    // the C64 still goes in the first cycle BA is low, then they wait too
    // (reu_clk_inc_post_write steals on the second; VICE testprogs
    // REU/bonzai spritetiming: 45 free cycles a line under 8 sprites)
    if (m_ba && m_ba->baLowForDma(0)) {
        if (m_type != FROM_REU || m_baWrites >= 1) return 1;
        ++m_baWrites;
    } else {
        m_baWrites = 0;
    }
    switch (m_type) {
        case TO_REU: {
            // BA is low for the CPU but not yet for the REU: sprite 0's DMA
            // starting (Denise reuSprite0)
            const uint8_t v = hostRead(m_ba && m_ba->baLow(0));
            ramWrite(m_dReu, v);
            advance();
            if (!m_left) { finish(m_dHost, m_dReu, 1, ST_EOB); m_floating = v; }
            return 1;
        }
        case FROM_REU: {
            const uint8_t v = m_floating = ramRead(m_dReu);
            syncTo(0);
            m_bus->write8(m_dHost, v);
            advance();
            if (!m_left) { finish(m_dHost, m_dReu, 1, ST_EOB); m_floating = ramRead(m_dReu); }
            return 1;
        }
        case SWAP: {
            const uint8_t r = ramRead(m_dReu);
            syncTo(0);
            const uint8_t c = m_bus->read8(m_dHost);
            ramWrite(m_dReu, c);
            syncTo(1);
            m_bus->write8(m_dHost, r);
            advance();
            if (!m_left) finish(m_dHost, m_dReu, 1, ST_EOB);
            return 2;
        }
        default: {                                  // VERIFY (VICE reu_dma_compare)
            const uint8_t r = ramRead(m_dReu);
            syncTo(0);
            const uint8_t c = m_bus->read8(m_dHost);
            advance();
            if (r == c) {
                if (!m_left) finish(m_dHost, m_dReu, 1, ST_EOB);
                return 1;
            }
            // A mismatch stops it (a cycle more if bytes were left); with one
            // byte left, that one is compared too
            uint8_t st = ST_VERIFY;
            int cycles = m_left >= 1 ? 2 : 1;
            int len = m_left;
            if (len == 0) {
                len = 1;
                st |= ST_EOB;
            } else if (len == 1) {
                if (ramRead(m_dReu) == m_bus->read8(m_dHost)) st |= ST_EOB;
            }
            finish(m_dHost, m_dReu, len, st);
            return cycles;
        }
    }
}

// VICE reu_dma_update_regs, then the command's execute bit off and the $FF00
// trigger off
void Reu::finish(uint16_t host, uint32_t reuAddr, int len, uint8_t st) {
    reuAddr &= m_bankUnused ? 0x7FFFFu : m_dramWrap - 1;   // VICE wrap_around_mask_when_storing
    m_status |= st;
    if (!(m_command & CMD_AUTOLOAD)) {
        if (!(m_addrCtl & AC_FIX_C64)) m_host = host;
        if (!(m_addrCtl & AC_FIX_REU)) {
            m_reuAddr = (uint16_t)reuAddr;
            m_bank = (uint8_t)((reuAddr >> 16) & ~m_bankUnused);
        }
        m_len = (uint16_t)len;
    } else {
        m_host = m_hostShadow;
        m_reuAddr = m_reuShadow;
        m_bank = m_bankShadow;
        m_len = m_lenShadow;
    }
    if (((st & ST_EOB) && (m_intMask & (INT_ON | INT_EOB)) == (INT_ON | INT_EOB)) ||
        ((st & ST_VERIFY) && (m_intMask & (INT_ON | INT_VERIFY)) == (INT_ON | INT_VERIFY))) {
        m_status |= ST_IRQ;
        setIrq(true);
    }
    m_active = false;
    m_command = (uint8_t)((m_command & ~CMD_EXEC) | CMD_NO_FF00);
}

void Reu::getDeviceInfo(DeviceInfo& out) const {
    out.name = name();
    out.baseAddr = baseAddr();
    out.addrMask = addrMask();
    static const char* const NAMES[] = {"STATUS", "COMMAND", "C64_LO", "C64_HI", "REU_LO", "REU_HI", "BANK",
                                        "LEN_LO", "LEN_HI", "IRQ_MASK", "ADDR_CTL"};
    for (int r = 0; r < REG_FIRST_UNUSED; ++r)
        out.registers.push_back({NAMES[r], (uint32_t)r, reg(r), "", {}});
    out.state.push_back({"Size", sizeName(size())});
    out.state.push_back({"DMA", m_active ? "running" : m_armed ? "waiting for $FF00" : "idle"});
}
