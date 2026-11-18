// RAMPAC / Node Datapac (see rampac.h). mmsim's own code.

#include "rampac.h"
#include <cstdio>
#include <cstring>
#include <fstream>

Rampac::Rampac(int kb) {
    if (kb != 128 && kb != 512 && kb != 1024) kb = 256;
    m_kb = kb;
    m_banks = kb <= 256 ? 1 : kb / 256;
    m_ram.assign((size_t)m_banks * BANK_SIZE, 0);
}

Rampac::~Rampac() { flush(); }

bool Rampac::open(const std::string& path) {
    flush();
    std::vector<uint8_t> ram(m_ram.size(), 0);
    if (!path.empty()) {
        std::ifstream f(path, std::ios::binary);
        if (f) {
            f.read(reinterpret_cast<char*>(ram.data()), (std::streamsize)ram.size());
        } else {
            std::ofstream make(path, std::ios::binary);       // a new card: all zeros
            if (!make) return false;
            make.write(reinterpret_cast<const char*>(ram.data()), (std::streamsize)ram.size());
        }
    }
    m_ram = std::move(ram);
    m_path = path;
    m_sector = 0;
    m_bank = 0;
    m_counter = 0;
    m_dirty = false;
    return true;
}

bool Rampac::saveTo(const std::string& path) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(m_ram.data()), (std::streamsize)m_ram.size());
    return (bool)f;
}

bool Rampac::flush() {
    if (!m_dirty || m_path.empty()) return true;
    if (!saveTo(m_path)) return false;
    m_dirty = false;
    return true;
}

// SELECT BLOCK's address side: the counter to 0, the bank from bus A10-A11
void Rampac::select(uint8_t port) {
    m_counter = 0;
    m_bank = ((port >> 2) & 3) & (m_banks - 1);
}

uint8_t Rampac::in(uint8_t port) {
    if (!decodes(port)) return 0xFF;
    if ((port & 2) == 0) {                  // SELECT BLOCK on a read: nothing drives the bus
        select(port);
        return 0xFF;
    }
    const uint8_t v = present() ? m_ram[addr()] : 0xFF;
    m_counter = (m_counter + 1) & (SECTOR_SIZE - 1);
    return v;
}

uint8_t Rampac::peek(uint8_t port) const {
    if (!decodes(port) || (port & 2) == 0) return 0xFF;
    return present() ? m_ram[addr()] : 0xFF;
}

void Rampac::out(uint8_t port, uint8_t v) {
    if (!decodes(port)) return;
    if ((port & 2) == 0) {
        select(port);
        m_sector = v;
        return;
    }
    if (present()) {
        m_ram[addr()] = v;
        m_dirty = true;
    }
    m_counter = (m_counter + 1) & (SECTOR_SIZE - 1);
}

std::vector<uint8_t> Rampac::saveState() const {
    std::vector<uint8_t> v(m_ram);
    v.push_back(m_sector);
    v.push_back((uint8_t)(m_counter & 0xFF));
    v.push_back((uint8_t)(m_counter >> 8));
    v.push_back((uint8_t)m_bank);
    return v;
}

void Rampac::loadState(const std::vector<uint8_t>& s) {
    const size_t n = m_ram.size();
    if (s.size() != n + 3 && s.size() != n + 4) return;   // (older snapshots: no bank byte)
    std::memcpy(m_ram.data(), s.data(), n);
    m_sector = s[n];
    m_counter = (s[n + 1] | s[n + 2] << 8) % SECTOR_SIZE;
    m_bank = s.size() == n + 4 ? (s[n + 3] & (m_banks - 1)) : 0;
    m_dirty = true;
}
