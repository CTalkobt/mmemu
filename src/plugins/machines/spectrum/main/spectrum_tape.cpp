// Spectrum cassettes, after Fuse (tape.c, tape_traps.c, loader.c) and
// libspectrum (tap.c, tzx_read.c, tape_block.c); GPL-2.0-or-later.

#include "spectrum_tape.h"
#include "libdevices/main/disk_file.h"
#include "plugins/z80/main/cpu_z80.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

namespace spectrum {

namespace {
constexpr uint32_t TAPE_HZ = 3500000;           // TZX's T-states
constexpr uint32_t MS = 3500;                   // T-states a millisecond

uint8_t sz53p(uint8_t v) {
    uint8_t p = v;
    p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
    return (uint8_t)((v & 0xA8) | (v ? 0 : 0x40) | ((p & 1) ? 0 : 0x04));
}
uint16_t le16(const std::vector<uint8_t>& b, size_t at) { return (uint16_t)(b[at] | b[at + 1] << 8); }
uint32_t le24(const std::vector<uint8_t>& b, size_t at) { return (uint32_t)(b[at] | b[at + 1] << 8 | b[at + 2] << 16); }
uint32_t le32(const std::vector<uint8_t>& b, size_t at) { return le24(b, at) | (uint32_t)b[at + 3] << 24; }
}  // namespace

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
bool SpectrumTape::mountTape(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!diskfile::read(path, bytes)) return false;           // plain, .gz or .zip
    const std::string ext = diskfile::imageExt(path);
    const bool tzx = ext == "tzx" || (bytes.size() >= 8 && std::memcmp(bytes.data(), "ZXTape!\x1A", 8) == 0);
    return load(bytes, tzx);
}

bool SpectrumTape::load(const std::vector<uint8_t>& bytes, bool tzx, std::string* err) {
    m_blocks.clear();
    m_block = 0;
    m_edges.clear();
    m_edge = 0;
    m_playing = false;
    m_level = false;
    return tzx ? parseTzx(bytes, err) : parseTap(bytes, err);
}

bool SpectrumTape::parseTap(const std::vector<uint8_t>& b, std::string* err) {
    for (size_t at = 0; at + 2 <= b.size();) {
        const size_t n = le16(b, at);
        at += 2;
        if (at + n > b.size()) { if (err) *err = "truncated .tap block"; return false; }
        Block k;
        k.rom = true;
        k.data.assign(b.begin() + (long)at, b.begin() + (long)(at + n));
        k.pilotCount = (!k.data.empty() && k.data[0] < 0x80) ? 8063 : 3223;
        k.pauseMs = 1000;
        m_blocks.push_back(std::move(k));
        at += n;
    }
    return !m_blocks.empty();
}

bool SpectrumTape::parseTzx(const std::vector<uint8_t>& b, std::string* err) {
    if (b.size() < 10 || std::memcmp(b.data(), "ZXTape!\x1A", 8) != 0) { if (err) *err = "not a TZX file"; return false; }
    std::vector<std::pair<size_t, int>> loops;   // block index at the loop start, repetitions
    size_t at = 10;
    auto need = [&](size_t n) { return at + n <= b.size(); };
    while (at < b.size()) {
        const uint8_t id = b[at++];
        Block k;
        switch (id) {
            case 0x10: {                                         // standard speed data
                if (!need(4)) goto truncated;
                k.rom = true;
                k.pauseMs = le16(b, at);
                const size_t n = le16(b, at + 2);
                at += 4;
                if (!need(n)) goto truncated;
                k.data.assign(b.begin() + (long)at, b.begin() + (long)(at + n));
                at += n;
                k.pilotCount = (!k.data.empty() && k.data[0] < 0x80) ? 8063 : 3223;
                m_blocks.push_back(std::move(k));
                break;
            }
            case 0x11: {                                         // turbo speed data
                if (!need(0x12)) goto truncated;
                k.pilot = le16(b, at); k.sync1 = le16(b, at + 2); k.sync2 = le16(b, at + 4);
                k.zero = le16(b, at + 6); k.one = le16(b, at + 8); k.pilotCount = le16(b, at + 10);
                k.usedBits = b[at + 12] ? b[at + 12] : 8;
                k.pauseMs = le16(b, at + 13);
                const size_t n = le24(b, at + 15);
                at += 0x12;
                if (!need(n)) goto truncated;
                k.data.assign(b.begin() + (long)at, b.begin() + (long)(at + n));
                at += n;
                m_blocks.push_back(std::move(k));
                break;
            }
            case 0x12:                                           // pure tone
                if (!need(4)) goto truncated;
                k.kind = Block::Tone;
                k.toneLen = le16(b, at);
                k.toneCount = le16(b, at + 2);
                at += 4;
                m_blocks.push_back(std::move(k));
                break;
            case 0x13: {                                         // pulse sequence
                if (!need(1)) goto truncated;
                const int n = b[at++];
                if (!need((size_t)n * 2)) goto truncated;
                k.kind = Block::Pulses;
                for (int i = 0; i < n; ++i) k.pulses.push_back(le16(b, at + (size_t)i * 2));
                at += (size_t)n * 2;
                m_blocks.push_back(std::move(k));
                break;
            }
            case 0x14: {                                         // pure data
                if (!need(10)) goto truncated;
                k.zero = le16(b, at); k.one = le16(b, at + 2);
                k.usedBits = b[at + 4] ? b[at + 4] : 8;
                k.pauseMs = le16(b, at + 5);
                const size_t n = le24(b, at + 7);
                at += 10;
                if (!need(n)) goto truncated;
                k.pilotCount = 0;
                k.sync1 = k.sync2 = 0;
                k.data.assign(b.begin() + (long)at, b.begin() + (long)(at + n));
                at += n;
                m_blocks.push_back(std::move(k));
                break;
            }
            case 0x15: {                                         // direct recording
                if (!need(8)) goto truncated;
                k.kind = Block::Direct;
                k.sampleT = le16(b, at);
                k.pauseMs = le16(b, at + 2);
                k.usedBits = b[at + 4] ? b[at + 4] : 8;
                const size_t n = le24(b, at + 5);
                at += 8;
                if (!need(n)) goto truncated;
                k.data.assign(b.begin() + (long)at, b.begin() + (long)(at + n));
                at += n;
                m_blocks.push_back(std::move(k));
                break;
            }
            case 0x20:                                           // pause, or stop the tape
                if (!need(2)) goto truncated;
                k.pauseMs = le16(b, at);
                k.kind = k.pauseMs ? Block::Pause : Block::Stop;
                at += 2;
                m_blocks.push_back(std::move(k));
                break;
            case 0x21: if (!need(1)) goto truncated; at += 1 + b[at]; break;      // group start
            case 0x22: break;                                    // group end
            case 0x23: at += 2; break;                           // jump (not followed)
            case 0x24:                                           // loop start
                if (!need(2)) goto truncated;
                loops.push_back({m_blocks.size(), le16(b, at)});
                at += 2;
                break;
            case 0x25:                                           // loop end: repeat the blocks
                if (!loops.empty()) {
                    const auto [from, count] = loops.back();
                    loops.pop_back();
                    std::vector<Block> body(m_blocks.begin() + (long)from, m_blocks.end());
                    for (int i = 1; i < count; ++i) m_blocks.insert(m_blocks.end(), body.begin(), body.end());
                }
                break;
            case 0x26: if (!need(2)) goto truncated; at += 2 + (size_t)le16(b, at) * 2; break;   // call sequence
            case 0x27: break;                                    // return
            case 0x28: if (!need(2)) goto truncated; at += 2 + le16(b, at); break;  // select
            case 0x2A:                                           // stop the tape if a 48K
                if (!need(4)) goto truncated;
                at += 4;
                k.kind = Block::Stop48;
                m_blocks.push_back(std::move(k));
                break;
            case 0x2B:                                           // set the signal level
                if (!need(5)) goto truncated;
                k.kind = Block::Level;
                k.level = b[at + 4] != 0;
                at += 5;
                m_blocks.push_back(std::move(k));
                break;
            case 0x30: if (!need(1)) goto truncated; at += 1 + b[at]; break;          // text
            case 0x31: if (!need(2)) goto truncated; at += 2 + b[at + 1]; break;      // message
            case 0x32: if (!need(2)) goto truncated; at += 2 + le16(b, at); break;    // archive info
            case 0x33: if (!need(1)) goto truncated; at += 1 + (size_t)b[at] * 3; break;  // hardware
            case 0x35: if (!need(20)) goto truncated; at += 20 + le32(b, at + 16); break;  // custom info
            case 0x5A: at += 9; break;                           // glue
            case 0x18: case 0x19:                                // CSW, generalized data: skipped
                if (!need(4)) goto truncated;
                at += 4 + le32(b, at);
                break;
            default:                                             // newer blocks carry a length
                if (!need(4)) goto truncated;
                at += 4 + le32(b, at);
                break;
        }
    }
    return true;
truncated:
    if (err) *err = "truncated TZX block";
    return !m_blocks.empty();
}

bool SpectrumTape::saveTapeRecording(const std::string& path) {
    std::vector<uint8_t> out;
    for (const auto& blk : m_saved) {
        out.push_back((uint8_t)blk.size());
        out.push_back((uint8_t)(blk.size() >> 8));
        out.insert(out.end(), blk.begin(), blk.end());
    }
    if (diskfile::isArchive(path)) return diskfile::save(path, out);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f) f.write((const char*)out.data(), (std::streamsize)out.size());
    return (bool)f;
}

void SpectrumTape::controlTape(const std::string& op) {
    std::string o = op;
    for (auto& c : o) c = (char)std::tolower((unsigned char)c);
    if (o == "play") { if (!m_playing) { m_playing = true; m_startPending = true; } }
    else if (o == "stop") m_playing = false;
    else if (o == "rewind") { m_playing = false; m_block = 0; m_edges.clear(); m_edge = 0; m_level = false; }
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------
uint64_t SpectrumTape::scaled(uint32_t len) {
    // Fuse: edge * processor speed / 3.5 MHz, the remainder carried
    const uint64_t num = (uint64_t)len * m_cpuHz + m_scaleRem;
    m_scaleRem = num % TAPE_HZ;
    return num / TAPE_HZ;
}

void SpectrumTape::expand(const Block& b) {
    m_edges.clear();
    m_edge = 0;
    auto dataBits = [&](const Block& k) {
        for (size_t i = 0; i < k.data.size(); ++i) {
            const int bits = i + 1 == k.data.size() ? k.usedBits : 8;
            uint8_t v = k.data[i];
            for (int j = 0; j < bits; ++j, v <<= 1) {
                const uint32_t len = (v & 0x80) ? k.one : k.zero;
                m_edges.push_back({len, -1});
                m_edges.push_back({len, -1});
            }
        }
    };
    switch (b.kind) {
        case Block::Data:
            for (uint32_t i = 0; i < b.pilotCount; ++i) m_edges.push_back({b.pilot, -1});
            if (b.sync1) m_edges.push_back({b.sync1, -1});
            if (b.sync2) m_edges.push_back({b.sync2, -1});
            dataBits(b);
            break;
        case Block::Tone:
            for (uint32_t i = 0; i < b.toneCount; ++i) m_edges.push_back({b.toneLen, -1});
            break;
        case Block::Pulses:
            for (uint32_t p : b.pulses) m_edges.push_back({p, -1});
            break;
        case Block::Direct:                                      // each bit is a level for sampleT
            for (size_t i = 0; i < b.data.size(); ++i) {
                const int bits = i + 1 == b.data.size() ? b.usedBits : 8;
                uint8_t v = b.data[i];
                for (int j = 0; j < bits; ++j, v <<= 1) {
                    const int8_t lv = (v & 0x80) ? 1 : 0;
                    if (!m_edges.empty() && m_edges.back().set == lv) m_edges.back().len += b.sampleT;
                    else m_edges.push_back({b.sampleT, lv});
                }
            }
            break;
        case Block::Pause:
            m_edges.push_back({b.pauseMs * MS, 0});
            return;
        default:
            return;
    }
    // The pause after a block: low (libspectrum's pause after data)
    if (b.pauseMs) m_edges.push_back({b.pauseMs * MS, 0});
}

bool SpectrumTape::nextEdge() {
    while (m_edge >= m_edges.size()) {
        if (m_block >= m_blocks.size()) return false;
        const Block& b = m_blocks[m_block++];
        if (b.kind == Block::Stop || (b.kind == Block::Stop48 && m_is48)) { m_edges.clear(); m_edge = 0; return false; }
        if (b.kind == Block::Level) { m_level = b.level; continue; }
        expand(b);
    }
    const Edge& e = m_edges[m_edge++];
    if (e.set < 0) m_level = !m_level;
    else m_level = e.set != 0;
    m_nextAt += scaled(e.len);
    return true;
}

void SpectrumTape::play(uint64_t now) {
    if (m_playing && !m_startPending) return;
    m_playing = true;
    m_startPending = false;
    m_nextAt = now;
    m_reads = 0;
}

void SpectrumTape::advance(uint64_t now) {
    if (m_startPending) play(now);
    while (m_playing && m_nextAt <= now) {
        if (!nextEdge()) { m_playing = false; break; }
    }
}

// Fuse loader.c (the timing heuristic): reads of EAR in a tight loop with B
// counting start the tape; reads that are not stop it
void SpectrumTape::earRead(uint64_t now, uint8_t b) {
    const uint64_t dt = now - m_lastReadT;
    const uint8_t db = (uint8_t)(b - m_lastB);
    m_lastReadT = now;
    m_lastB = b;
    if (!mounted() || !m_autoPlay) return;
    if (m_playing) {
        if (dt <= 1000 && (db == 1 || db == 0 || db == 0xFF)) m_reads = 0;
        else if (++m_reads >= 10) { m_playing = false; m_reads = 0; }
    } else if (m_block < m_blocks.size() || m_edge < m_edges.size()) {
        if (dt <= 500 && (db == 1 || db == 0xFF)) {
            if (++m_reads >= 10) play(now);
        } else {
            m_reads = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// The ROM traps (Fuse tape_traps.c)
// ---------------------------------------------------------------------------
bool SpectrumTape::trapLoad(Z80& cpu, const std::function<uint8_t(uint16_t)>& rd,
                            const std::function<void(uint16_t, uint8_t)>& wr) {
    if (m_playing || m_edge < m_edges.size()) return false;    // mid-block: the pulses
    while (m_block < m_blocks.size() && m_blocks[m_block].kind == Block::Level) m_level = m_blocks[m_block++].level;
    if (m_block >= m_blocks.size()) return false;
    const Block& blk = m_blocks[m_block];
    if (blk.kind != Block::Data || !blk.rom) {                 // a custom loader's: play it
        play(cpu.cycles());
        return false;
    }
    uint16_t af = (uint16_t)cpu.regRead(Z80::R_AF), af2 = (uint16_t)cpu.regRead(Z80::R_AF2);
    uint16_t bc = (uint16_t)cpu.regRead(Z80::R_BC), de = (uint16_t)cpu.regRead(Z80::R_DE);
    uint16_t hl = (uint16_t)cpu.regRead(Z80::R_HL), ix = (uint16_t)cpu.regRead(Z80::R_IX);
    uint8_t A = af >> 8, F = af & 0xFF, B = bc >> 8, C = bc & 0xFF, H = hl >> 8, L = hl & 0xFF;
    const auto& d = blk.data;
    const int length = (int)d.size();
    int read = length - 1;
    if (read > de) read = de;
    int processed = 0;
    uint8_t parity = 0;
    auto cp1 = [&]() {                                          // CP 1
        const uint16_t r = (uint16_t)(A - 1);
        uint8_t f = (uint8_t)((r & 0x100) ? 0x01 : ((r & 0xFF) ? 0 : 0x40));
        f |= 0x02 | ((A & 0x0F) == 0 ? 0x10 : 0) | (((A ^ 1) & (A ^ r) & 0x80) ? 0x04 : 0) | (r & 0x80);
        F = f;
    };
    auto finish = [&]() {
        C = 1;
        H = parity;
        de = (uint16_t)(de - processed);
        ix = (uint16_t)(ix + processed);
    };
    auto fail = [&]() { F &= (uint8_t)~0x01; finish(); };
    [&]() {
        if (!length) { L = 1; af2 = (uint16_t)((af2 & 0xFF00) | 1); F &= (uint8_t)~0x01; return; }
        const bool verify = !(af2 & 0x01);
        const uint8_t requested = af2 >> 8;
        A = 0;
        L = parity = d[0];
        if (!de) {                                              // the ROM's zero-length bug
            B = 0xB0;
            A = parity;
            cp1();
            finish();
            return;
        }
        af2 = 0x0145;
        A = (uint8_t)(requested ^ parity);
        F = sz53p(A);
        if (A) { fail(); return; }
        if (read) L = d[(size_t)read];
        for (processed = 0; processed < read; ++processed) {
            const uint8_t v = d[(size_t)processed + 1];
            parity ^= v;
            if (verify) {
                if (v != rd((uint16_t)(ix + processed))) { L = v; fail(); return; }
            } else {
                wr((uint16_t)(ix + processed), v);
            }
        }
        if (de == processed && read + 1 < length) {               // the parity byte
            parity ^= d[(size_t)read + 1];
            A = parity;
            cp1();
            B = 0xB0;
            finish();
            return;
        }
        B = 255; L = 1;                                         // ran out: INC B
        B = 0;
        F = (uint8_t)((F & 0x01) | 0x10 | 0x40);
        fail();
    }();
    cpu.regWrite(Z80::R_AF, (uint32_t)(A << 8 | F));
    cpu.regWrite(Z80::R_AF2, af2);
    cpu.regWrite(Z80::R_BC, (uint32_t)(B << 8 | C));
    cpu.regWrite(Z80::R_DE, de);
    cpu.regWrite(Z80::R_HL, (uint32_t)(H << 8 | L));
    cpu.regWrite(Z80::R_IX, ix);
    cpu.setPc(0x05E2);                                          // the RET that returns to LD-BYTES's caller
    ++m_block;                                                  // the block is used
    m_level = false;
    return true;
}

void SpectrumTape::trapSave(Z80& cpu, const std::function<uint8_t(uint16_t)>& rd) {
    const uint8_t A = (uint8_t)(cpu.regRead(Z80::R_AF) >> 8);
    const uint16_t de = (uint16_t)cpu.regRead(Z80::R_DE), ix = (uint16_t)cpu.regRead(Z80::R_IX);
    std::vector<uint8_t> blk;
    uint8_t parity = A;
    blk.push_back(A);
    for (uint16_t i = 0; i < de; ++i) {
        const uint8_t v = rd((uint16_t)(ix + i));
        parity ^= v;
        blk.push_back(v);
    }
    blk.push_back(parity);
    m_saved.push_back(std::move(blk));
    cpu.setPc(0x053E);                                          // SA/LD-RET's RET
}

}  // namespace spectrum
