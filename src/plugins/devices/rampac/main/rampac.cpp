// RAMPAC / Node Datapac (see rampac.h). mmsim's own code.

#include "rampac.h"
#include <cstdio>
#include <cstring>
#include <fstream>

Rampac::~Rampac() { flush(); }

bool Rampac::open(const std::string& path) {
    flush();
    std::vector<uint8_t> ram(SIZE, 0);
    if (!path.empty()) {
        std::ifstream f(path, std::ios::binary);
        if (f) {
            f.read(reinterpret_cast<char*>(ram.data()), (std::streamsize)SIZE);
        } else {
            std::ofstream make(path, std::ios::binary);       // a new card: all zeros
            if (!make) return false;
            make.write(reinterpret_cast<const char*>(ram.data()), (std::streamsize)SIZE);
        }
    }
    m_ram = std::move(ram);
    m_path = path;
    m_sector = 0;
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

void Rampac::step() {
    if (++m_counter >= SECTOR_SIZE) {
        m_counter = 0;
        ++m_sector;                         // uint8_t: 255 wraps to 0
    }
}

uint8_t Rampac::in(uint8_t port) {
    if (port == 0x83) {
        const uint8_t v = m_ram[(size_t)m_sector * SECTOR_SIZE + m_counter];
        step();
        return v;
    }
    return peek(port);
}

uint8_t Rampac::peek(uint8_t port) const {
    if (port == 0x81) return m_sector;
    if (port == 0x83) return m_ram[(size_t)m_sector * SECTOR_SIZE + m_counter];
    return 0xFF;
}

void Rampac::out(uint8_t port, uint8_t v) {
    if (port == 0x81 || port == 0x85) {
        m_sector = v;
        m_counter = 0;
    } else if (port == 0x83) {
        m_ram[(size_t)m_sector * SECTOR_SIZE + m_counter] = v;
        m_dirty = true;
        step();
    }
}

std::vector<uint8_t> Rampac::saveState() const {
    std::vector<uint8_t> v(m_ram);
    v.push_back(m_sector);
    v.push_back((uint8_t)(m_counter & 0xFF));
    v.push_back((uint8_t)(m_counter >> 8));
    return v;
}

void Rampac::loadState(const std::vector<uint8_t>& s) {
    if (s.size() != SIZE + 3) return;
    std::memcpy(m_ram.data(), s.data(), SIZE);
    m_sector = s[SIZE];
    m_counter = (s[SIZE + 1] | s[SIZE + 2] << 8) % SECTOR_SIZE;
    m_dirty = true;
}
