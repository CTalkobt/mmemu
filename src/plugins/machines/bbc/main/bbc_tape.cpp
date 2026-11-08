// BBC Micro and Electron cassette: the 6850 ACIA and UEF tapes, after b-em
// (acia.c, sysacia.c, tape.c, uef.c; GPL-2.0-or-later)

#include "bbc_tape.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <zlib.h>

namespace bbc {

// ---------------------------------------------------------------------------
// Acia6850
// ---------------------------------------------------------------------------
uint8_t Acia6850::read(uint16_t addr) {
    if (addr & 1) {                                  // receive data
        m_status &= (uint8_t)~(IRQ | RDRF);
        return m_rx;
    }
    uint8_t s = m_status & (uint8_t)~IRQ;            // status
    if (irq()) s |= IRQ;
    return s;
}

void Acia6850::write(uint16_t addr, uint8_t v) {
    if (addr & 1) {                                  // transmit data
        if (onTransmit) onTransmit(v);
        m_status &= (uint8_t)~TDRE;
        // 10 bits at 1200 baud (2 MHz cycles), or 300 baud dividing by 64
        m_txLeft = m_cassette ? ((m_control & 3) == 2 ? 66667 : 16667) : 0;
    } else if (v != m_control) {                     // control
        m_control = v;
        if (v == 3) {                                // master reset
            m_status &= CTS | DCD;
            if (!(m_status & CTS)) m_status |= TDRE;
        }
    }
}

void Acia6850::poll(int cycles) {
    // The byte went: the transmit register is empty again (unless CTS is high)
    if (m_txLeft > 0 && (m_txLeft -= cycles) > 0) return;
    if (!(m_status & (TDRE | CTS))) m_status |= TDRE;
}

void Acia6850::receive(uint8_t v) {
    m_rx = v;
    m_status |= RDRF | IRQ;
}

void Acia6850::dcdHigh() {
    if (m_status & DCD) return;
    m_status |= DCD | IRQ;
}

void Acia6850::dcdLow() { m_status &= (uint8_t)~DCD; }

// ---------------------------------------------------------------------------
// UefTape
// ---------------------------------------------------------------------------
namespace {
constexpr double CPU_HZ = 2000000.0;
constexpr int POLL_CYCLES = 128;                     // b-em otherstuff_poll
const char UEF_MAGIC[] = "UEF File!";
}  // namespace

UefTape::UefTape() {}

void UefTape::reset() {
    // The motor relay off
    m_motor = false;
}

void UefTape::setBaud(double baud) {
    // uef.c: tapellatch = (1000000 / (baud / 10)) / 64
    if (baud <= 0) return;
    m_pps = baud / 10.0;
    m_latch = std::max(1, (int)((1000000.0 / m_pps) / 64.0));
}

bool UefTape::load(const std::vector<uint8_t>& file, std::string* err) {
    std::vector<uint8_t> data;
    if (file.size() >= 2 && file[0] == 0x1F && file[1] == 0x8B) {
        // gzip: unpack with zlib
        z_stream zs{};
        if (inflateInit2(&zs, 15 + 32) != Z_OK) { if (err) *err = "zlib"; return false; }
        zs.next_in = const_cast<Bytef*>(file.data());
        zs.avail_in = (uInt)file.size();
        uint8_t buf[65536];
        int r;
        do {
            zs.next_out = buf;
            zs.avail_out = sizeof(buf);
            r = inflate(&zs, Z_NO_FLUSH);
            if (r != Z_OK && r != Z_STREAM_END) { inflateEnd(&zs); if (err) *err = "bad gzip data"; return false; }
            data.insert(data.end(), buf, buf + (sizeof(buf) - zs.avail_out));
        } while (r != Z_STREAM_END);
        inflateEnd(&zs);
    } else {
        data = file;
    }
    if (data.size() < 12 || std::memcmp(data.data(), UEF_MAGIC, 10) != 0) {
        if (err) *err = "not a UEF tape";
        return false;
    }
    m_uef.assign(data.begin() + 12, data.end());
    m_pos = 0;
    m_inChunk = false;
    m_inTone = 0;
    setBaud(1200);
    m_latchCount = 0;
    m_play = true;                                   // PLAY down: rolls when the motor starts
    return true;
}

bool UefTape::mountTape(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return load(file);
}

void UefTape::controlTape(const std::string& op) {
    if (op == "play") m_play = !m_uef.empty();
    else if (op == "stop") m_play = false;
    else if (op == "rewind") {
        m_pos = 0;
        m_inChunk = false;
        m_inTone = 0;
        setBaud(1200);
    }
}

void UefTape::setMotor(bool motor, bool output) {
    if (motor && !m_motor) m_recIdleFrom = m_now;
    if (!motor && m_motor && m_recording) recFlushData();
    m_motor = motor;
    m_output = output;
}

void UefTape::clock(uint64_t cycles) {
    m_now += cycles;
    m_cycleAcc += cycles;
    while (m_cycleAcc >= POLL_CYCLES) {
        m_cycleAcc -= POLL_CYCLES;
        if (m_latchCount == 0) {
            if (m_motor && m_play && !m_recording) poll();
            m_latchCount = m_latch;
        }
        m_latchCount--;
    }
}

float UefTape::nextFloat() {
    const uint32_t bits = next32();
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// uef_poll: one byte time of the tape
void UefTape::poll() {
    if (!m_inChunk) {
        if (m_pos + 6 > m_uef.size()) { m_pos = m_uef.size(); return; }   // the end
        m_startChunk = true;
        m_chunkId = next16();
        m_chunkLen = next32();
        m_inChunk = true;
        m_chunkPos = 0;
        m_secondTone = false;
    }
    auto tone = [&](uint16_t n) { int t = n / 20; return t ? t : 1; };
    switch (m_chunkId) {
        case 0x100:                                  // raw data
            if (m_startChunk) { if (onDataStart) onDataStart(); m_startChunk = false; }
            if (--m_chunkLen <= 0) m_inChunk = false;
            if (onByte) onByte(next());
            return;
        case 0x104:                                  // defined-format data
            if (!m_chunkPos) {
                m_dataBits = next();
                next();                              // parity
                next();                              // stop bits
                m_chunkLen -= 3;
                m_chunkPos = 1;
                if (onDataStart) onDataStart();
                if (m_chunkLen <= 0) m_inChunk = false;
            } else {
                if (--m_chunkLen <= 0) m_inChunk = false;
                const uint8_t v = next();
                if (onByte) onByte(m_dataBits == 7 ? (uint8_t)(v & 0x7F) : v);
            }
            return;
        case 0x110:                                  // carrier tone
            if (onCarrier) onCarrier();
            if (!m_inTone) {
                m_inTone = tone(next16());
            } else if (--m_inTone == 0) {
                m_inChunk = false;
            }
            return;
        case 0x111:                                  // carrier tone with a dummy byte
            if (onCarrier && !(m_secondTone && m_inTone == 1)) onCarrier();
            if (!m_inTone && !m_secondTone && m_chunkPos == 0) {
                m_inTone = tone(next16());
                m_chunkPos = 1;
            } else if (--m_inTone <= 0) {
                if (m_secondTone) {
                    m_inTone = 0;
                    m_inChunk = false;
                } else {
                    m_secondTone = true;
                    m_inTone = tone(next16());
                    if (onByte) onByte(0xAA);
                }
            }
            return;
        case 0x112:                                  // integer gap
            if (!m_inTone) {
                m_inTone = tone(next16());
            } else if (--m_inTone == 0) {
                m_inChunk = false;
            }
            return;
        case 0x113:                                  // baud rate
            setBaud(nextFloat());
            m_inChunk = false;
            return;
        case 0x116:                                  // floating-point gap
            if (!m_chunkPos) {
                m_gapLeft = nextFloat();
                m_chunkPos = 1;
            } else {
                m_gapLeft -= (float)(1.0 / m_pps);
                if (m_gapLeft <= 0) m_inChunk = false;
            }
            return;
        default:                                     // origin, platform, security waves, ...
            skip((uint32_t)std::max<int64_t>(0, m_chunkLen));
            m_inChunk = false;
            return;
    }
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------
bool UefTape::startTapeRecord() {
    m_rec.clear();
    m_recData.clear();
    m_recording = true;
    m_recIdleFrom = m_now;
    return true;
}

void UefTape::stopTapeRecord() {
    recFlushData();
    m_recording = false;
}

void UefTape::recChunk(uint16_t id, const std::vector<uint8_t>& body) {
    const uint32_t n = (uint32_t)body.size();
    const uint8_t hdr[6] = {(uint8_t)id, (uint8_t)(id >> 8), (uint8_t)n, (uint8_t)(n >> 8),
                            (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
    m_rec.insert(m_rec.end(), hdr, hdr + 6);
    m_rec.insert(m_rec.end(), body.begin(), body.end());
}

void UefTape::recFlushData() {
    if (m_recData.empty()) return;
    recChunk(0x100, m_recData);
    m_recData.clear();
}

void UefTape::recTone(uint64_t cycles) {
    // The line idles at the carrier (2400 Hz) while the motor runs
    uint64_t waves = (uint64_t)((double)cycles * 2400.0 / CPU_HZ);
    while (waves) {
        const uint16_t n = (uint16_t)std::min<uint64_t>(waves, 0xFFFF);
        recChunk(0x110, {(uint8_t)n, (uint8_t)(n >> 8)});
        waves -= n;
    }
}

void UefTape::transmitted(uint8_t v) {
    if (!m_recording || !m_motor || !m_output) return;
    // More than two byte times since the last byte: carrier tone in between
    const uint64_t idle = m_now - m_recIdleFrom;
    if (idle > (uint64_t)(2.0 * CPU_HZ / m_pps)) {
        recFlushData();
        recTone(idle);
    }
    m_recData.push_back(v);
    m_recIdleFrom = m_now;
}

bool UefTape::saveTapeRecording(const std::string& path) {
    recFlushData();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(UEF_MAGIC, 10);                          // with its terminating zero
    const uint8_t version[2] = {10, 0};              // UEF 0.10
    f.write((const char*)version, 2);
    f.write((const char*)m_rec.data(), (std::streamsize)m_rec.size());
    return (bool)f;
}

}  // namespace bbc
