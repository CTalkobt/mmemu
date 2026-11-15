// The Exatron Stringy Floppy, after xtrs (Tim Mann; trs_stringy.c,
// MIT-style licence); mmsim's own code.

#include "trs80_stringy.h"
#include <cstdio>
#include <cstring>
#include <fstream>

namespace trs80 {

namespace {
constexpr uint8_t IN_WP = 0x01, IN_EOT = 0x04, IN_NOWAFER = 0x08, FLUX = 0x80;
constexpr uint8_t OUT_MOTOR = 0x01, OUT_WRITE = 0x04;
constexpr size_t HEADER = 12;
enum { STOPPED, READING, WRITING };
}  // namespace

Stringy::~Stringy() { flush(); }

int Stringy::state(uint8_t out) {
    if (!(out & OUT_MOTOR)) return STOPPED;
    return (out & OUT_WRITE) ? WRITING : READING;
}

std::vector<uint8_t> Stringy::create(int kb, int eot, bool wp) {
    const uint32_t len = (uint32_t)kb * 1024u * 2u * 9u / 8u;      // with gaps and leaders (xtrs)
    std::vector<uint8_t> f(HEADER + len, 0);
    std::memcpy(f.data(), "ESF\x1a", 4);
    f[4] = (uint8_t)HEADER;
    f[5] = wp ? 1 : 0;
    f[6] = (uint8_t)eot;
    f[7] = (uint8_t)(eot >> 8);
    for (int i = 0; i < 4; ++i) f[8 + i] = (uint8_t)(len >> (8 * i));
    return f;
}

bool Stringy::load(int u, const std::vector<uint8_t>& f, const std::string& path, std::string* err) {
    if (u < 0 || u >= UNITS) return false;
    if (f.size() < HEADER || std::memcmp(f.data(), "ESF\x1a", 4) != 0 || f[4] != HEADER) {
        if (err) *err = "not an .esf wafer";
        return false;
    }
    flush();
    Wafer w;
    w.path = path;
    w.present = true;
    w.writeProtected = f[5] & 1;
    w.eotCells = (uint16_t)(f[6] | f[7] << 8);
    const uint32_t len = (uint32_t)f[8] | (uint32_t)f[9] << 8 | (uint32_t)f[10] << 16 | (uint32_t)f[11] << 24;
    w.data.assign(f.begin() + HEADER, f.end());
    w.data.resize(len, 0);
    w.length = (int64_t)len * CELL * 8;
    w.eot = (int64_t)w.eotCells * CELL;
    w.inPort = w.writeProtected ? IN_WP : 0;
    w.changeTo = 1;
    m_unit[u] = std::move(w);
    return true;
}

std::vector<uint8_t> Stringy::image(int u) const {
    const Wafer& w = m_unit[u & 7];
    if (!w.present) return {};
    std::vector<uint8_t> f = create(0, w.eotCells, w.writeProtected);
    const uint32_t len = (uint32_t)w.data.size();
    for (int i = 0; i < 4; ++i) f[8 + i] = (uint8_t)(len >> (8 * i));
    f.resize(HEADER);
    f.insert(f.end(), w.data.begin(), w.data.end());
    return f;
}

bool Stringy::mountDisk(int unit, const std::string& path) {
    const int u = unit - 1;
    if (u < 0 || u >= UNITS) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!load(u, b, path)) return false;
    std::ofstream w(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!w) { m_unit[u].writeProtected = true; m_unit[u].inPort |= IN_WP; }
    if (onMount) onMount();
    return true;
}

void Stringy::ejectDisk(int unit) {
    const int u = unit - 1;
    if (u < 0 || u >= UNITS || !m_unit[u].present) return;
    flush();
    m_unit[u] = Wafer();
}

bool Stringy::getDiskStatus(int unit, int& track, int& sector, bool& led) {
    const int u = unit - 1;
    if (!m_fitted || u < 0 || u >= UNITS) return false;
    track = 0;
    sector = (int)(m_unit[u].pos / CELL / 8);
    led = state(m_unit[u].outPort) != STOPPED;
    return true;
}

std::string Stringy::getMountedDiskPath(int unit) {
    const int u = unit - 1;
    return u >= 0 && u < UNITS ? m_unit[u].path : "";
}

void Stringy::getDeviceInfo(DeviceInfo& out) const {
    out.name = "Exatron Stringy Floppy";
    out.baseAddr = 0xF0;
    out.addrMask = 0x07;
    for (int u = 0; u < UNITS; ++u) {
        const Wafer& w = m_unit[u];
        if (!w.present) continue;
        char b[96];
        std::snprintf(b, sizeof b, " (%zu bytes, at %lld%s)", w.data.size(), (long long)(w.pos / CELL / 8),
                      state(w.outPort) == WRITING ? ", writing" : state(w.outPort) == READING ? ", reading" : "");
        out.state.push_back({"Unit " + std::to_string(u), (w.path.empty() ? "(wafer)" : w.path) + b});
    }
}

void Stringy::flush() {
    for (int u = 0; u < UNITS; ++u) {
        Wafer& w = m_unit[u];
        if (!w.dirty || w.path.empty()) continue;
        const auto f = image(u);
        std::ofstream o(w.path, std::ios::binary | std::ios::trunc);
        if (o) o.write(reinterpret_cast<const char*>(f.data()), (std::streamsize)f.size());
        w.dirty = false;
    }
}

// The wafer's start: as xtrs's re-read of the header
void Stringy::rewind(Wafer& w) {
    w.bytePos = 0;
    w.filePos = 0;
    w.byteBuf = 0;
    w.bitPos = 0;
}

void Stringy::update(Wafer& w, uint64_t now) {
    if (state(w.outPort) == STOPPED) return;
    w.pos += (int64_t)(now - w.posTime);
    w.posTime = now;
    if (w.length > 0 && w.pos >= w.length) {
        w.pos -= w.length;
        w.changePos -= w.length;
    }
    if (w.pos >= w.length - w.eot) w.inPort |= IN_EOT;
    else w.inPort &= (uint8_t)~IN_EOT;
}

int Stringy::readBit(Wafer& w) {
    if (w.bitPos == 0) {
        if (w.bytePos++ >= w.data.size()) { w.filePos = 0; w.bytePos = 0; }
        w.byteBuf = fileByte(w, w.filePos++);
    }
    const int bit = (w.byteBuf >> w.bitPos) & 1;
    w.bitPos = (uint8_t)((w.bitPos + 1) & 7);
    return bit;
}

void Stringy::writeBit(Wafer& w, int bit) {
    w.byteBuf = (uint8_t)((w.byteBuf & ~(1 << w.bitPos)) | (bit << w.bitPos));
    if (++w.bitPos == 8) {
        if (w.filePos < w.data.size()) w.data[w.filePos] = w.byteBuf;
        ++w.filePos;
        if (++w.bytePos >= w.data.size()) { w.filePos = 0; w.bytePos = 0; }
        w.bitPos = 0;
        w.byteBuf = 0;
        w.dirty = true;
    }
}

void Stringy::writeFlux(Wafer& w, int flux, int64_t delta) {
    int cells = (int)((delta + 1) / CELL);
    if (cells > 3) cells = 3;               // xtrs crushes long runs (TRS32 shows none)
    if (cells < 0) cells = 0;
    const int64_t adjust = delta - (int64_t)cells * CELL;
    for (int i = 0; i < cells; ++i) writeBit(w, flux);
    w.pos -= adjust;                        // so the leader is met where it was written
}

void Stringy::byteFlush(Wafer& w) {
    if (state(w.outPort) != WRITING || w.bitPos == 0) return;
    const uint8_t mask = (uint8_t)(0xFF << w.bitPos);
    w.byteBuf = (uint8_t)((fileByte(w, w.filePos) & mask) | (w.byteBuf & ~mask));
    if (w.filePos < w.data.size()) w.data[w.filePos] = w.byteBuf;
    w.dirty = true;
}

uint8_t Stringy::peek(uint8_t port) const {
    if (!m_fitted) return 0xFF;
    const Wafer& w = m_unit[port & 7];
    return w.present ? w.inPort : IN_NOWAFER;
}

uint8_t Stringy::in(uint8_t port, uint64_t now) {
    if (!m_fitted) return 0xFF;
    Wafer& w = m_unit[port & 7];
    if (!w.present) return IN_NOWAFER;
    update(w, now);
    if (state(w.outPort) == READING) {
        while (w.pos >= w.changePos) {
            w.inPort = (uint8_t)((w.inPort & ~FLUX) | (w.changeTo ? 0 : FLUX));
            w.changeTo = !readBit(w);
            w.changePos += CELL;
        }
    }
    return w.inPort;
}

void Stringy::out(uint8_t port, uint8_t v, uint64_t now) {
    if (!m_fitted) return;
    Wafer& w = m_unit[port & 7];
    if (!w.present) return;
    const int was = state(w.outPort), next = state(v);
    update(w, now);
    if (was == STOPPED && next != STOPPED) {
        w.pos = 0;
        w.inPort &= (uint8_t)~IN_EOT;
        rewind(w);
        w.posTime = now;
        w.inPort &= (uint8_t)~FLUX;
        w.changePos = w.pos;
        w.changeTo = 1;
    }
    if (was != WRITING && next == WRITING && !w.writeProtected) writeFlux(w, 1, 0);
    if (was == WRITING && !w.writeProtected) {
        if (((w.outPort ^ v) & FLUX) || next != WRITING) {
            writeFlux(w, (v & FLUX) != 0, w.pos - w.changePos);
            w.changePos = w.pos;
        }
        if (next != WRITING) byteFlush(w);
    }
    w.outPort = v;
}

}  // namespace trs80
