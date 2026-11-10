#include "georam.h"
#include <cctype>
#include <fstream>
#include <iterator>

GeoRam::GeoRam(uint32_t sizeBytes) {
    if (!setSize(sizeBytes)) setSize(512 * 1024);
}

bool GeoRam::setSize(uint32_t bytes) {
    switch (bytes >> 10) {
        case 512: case 1024: case 2048: case 4096: break;
        default: return false;
    }
    if (bytes % 1024) return false;
    m_ram.assign(bytes, 0);
    reset();
    return true;
}

bool GeoRam::loadImage(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open '" + path + "'"; return false; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!setSize((uint32_t)data.size())) {
        err = "'" + path + "' is " + std::to_string(data.size()) + " bytes; a GeoRAM image is 512K, 1M, 2M or 4M";
        return false;
    }
    m_ram = std::move(data);
    return true;
}

bool GeoRam::saveImage(const std::string& path, std::string& err) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { err = "cannot write '" + path + "'"; return false; }
    f.write((const char*)m_ram.data(), (std::streamsize)m_ram.size());
    if (!f) { err = "write to '" + path + "' failed"; return false; }
    return true;
}

bool GeoRam::setOption(const std::string& key, const std::string& value, std::string& err) {
    if (key == "size") {
        uint64_t n = 0;
        size_t i = 0;
        while (i < value.size() && isdigit((unsigned char)value[i])) n = n * 10 + (uint64_t)(value[i++] - '0');
        std::string unit = value.substr(i);
        for (auto& c : unit) c = (char)toupper((unsigned char)c);
        if (unit == "K" || unit == "KB") n <<= 10;
        else if (unit == "M" || unit == "MB") n <<= 20;
        else if (!unit.empty()) n = 0;
        if (i == 0 || n > (4u << 20) || !setSize((uint32_t)n)) {
            err = "a GeoRAM is 512K, 1M, 2M or 4M";
            return false;
        }
        return true;
    }
    if (key == "image") return loadImage(value, err);
    err = "the GeoRAM's options are a size (512K-4M) or an image file";
    return false;
}

bool GeoRam::ioRead(IBus*, uint32_t addr, uint8_t* val) {
    if ((addr & 0xFF00) != 0xDE00) return false;    // $DFxx: write-only, not driven
    *val = m_ram[offset(addr)];
    return true;
}

bool GeoRam::ioPeek(IBus* bus, uint32_t addr, uint8_t* val) {
    if ((addr & 0xFFFE) == 0xDFFE) { *val = (addr & 1) ? m_block : m_page; return true; }
    return ioRead(bus, addr, val);
}

bool GeoRam::ioWrite(IBus*, uint32_t addr, uint8_t v) {
    addr &= 0xFFFF;
    if ((addr & 0xFF00) == 0xDE00) { m_ram[offset(addr)] = v; return true; }
    if (addr < 0xDF80 || addr > 0xDFFF) return false;
    if (addr & 1) m_block = (uint8_t)(v % (m_ram.size() / 16384));   // VICE: modulo the blocks
    else m_page = (uint8_t)(v % 64);
    return true;
}

void GeoRam::getDeviceInfo(DeviceInfo& out) const {
    out.name = name();
    out.baseAddr = baseAddr();
    out.addrMask = addrMask();
    out.registers.push_back({"PAGE", 0x1FE, m_page, "the 256-byte page in the 16 KB block ($DFFE)", {}});
    out.registers.push_back({"BLOCK", 0x1FF, m_block, "the 16 KB block ($DFFF)", {}});
    const uint32_t s = size();
    out.state.push_back({"Size", s >= (1u << 20) ? std::to_string(s >> 20) + " MB" : std::to_string(s >> 10) + " KB"});
}
