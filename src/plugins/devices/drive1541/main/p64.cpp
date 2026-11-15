// P64 reading, altered from the P64 reference implementation (VICE
// src/lib/p64/p64.c): only reading side 0, into std::vector, in this
// project's style.
//
// P64 reference implementation by Benjamin 'BeRo' Rosseaux
// Copyright (c) 2011-2012, Benjamin Rosseaux
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
//    1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
//
//    2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
//
//    3. This notice may not be removed or altered from any source
//    distribution.

#include "p64.h"
#include <cstring>

namespace p64 {

namespace {

uint32_t crc32(const uint8_t* d, size_t n) {
    static const uint32_t T[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
    if (!n) return 0;
    uint32_t v = 0xffffffff;
    for (size_t i = 0; i < n; ++i) {
        v ^= d[i];
        v = T[v & 0xf] ^ (v >> 4);
        v = T[v & 0xf] ^ (v >> 4);
    }
    return v ^ 0xffffffff;
}

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// The adaptive binary range decoder (12-bit probabilities)
struct RangeDecoder {
    const uint8_t* buf;
    size_t size, pos = 0;
    uint32_t code = 0, low = 0, high = 0xffffffff;
    RangeDecoder(const uint8_t* b, size_t n) : buf(b), size(n) {
        for (int i = 0; i < 4; ++i) code = (code << 8) | next();
    }
    uint8_t next() { return pos < size ? buf[pos++] : 0; }
    uint32_t bit(uint32_t& p) {
        const uint32_t mid = low + ((high - low) >> 12) * p;
        uint32_t b;
        if (code <= mid) { p += (0xfff - p) >> 4; high = mid; b = 1; }
        else             { p -= p >> 4; low = mid + 1; b = 0; }
        while (!((low ^ high) & 0xff000000)) {
            low <<= 8;
            high = (high << 8) | 0xff;
            code = (code << 8) | next();
        }
        return b;
    }
};

// A half-track chunk: the pulse count, the coded size, then the range-coded
// deltas of position and strength (10 probability models)
bool readStream(const uint8_t* d, size_t n, std::vector<uint32_t>& pulses) {
    if (n < 8) return false;
    const uint32_t count = le32(d), size = le32(d + 4);
    if (!size) return count == 0;
    if (8 + (size_t)size > n) return false;
    enum { POSITION = 0, STRENGTH = 4, POSITION_FLAG = 8, STRENGTH_FLAG = 9, MODELS = 10 };
    static const uint32_t COUNTS[MODELS] = {65536, 65536, 65536, 65536, 65536, 65536, 65536, 65536, 4, 4};
    uint32_t offsets[MODELS], states[MODELS] = {};
    uint32_t total = 0;
    for (int i = 0; i < MODELS; ++i) { offsets[i] = total; total += COUNTS[i]; }
    std::vector<uint32_t> prob(total, 2048);
    RangeDecoder rc(d + 8, size);
    auto readBit = [&](int m) { return states[m] = rc.bit(prob[offsets[m] + states[m]]); };
    auto readDword = [&](int m) {
        uint32_t r = 0;
        for (int b = 0; b < 4; ++b) {
            uint32_t ctx = 1;
            for (int k = 7; k >= 0; --k)
                ctx = (ctx << 1) | rc.bit(prob[offsets[m + b] + (((states[m + b] << 8) | ctx) & 0xffff)]);
            states[m + b] = ctx & 0xff;
            r |= (ctx & 0xff) << (b * 8);
        }
        return r;
    };
    uint32_t position = 0, delta = 0, strength = 0;
    pulses.clear();
    while (pulses.size() < count) {
        if (readBit(POSITION_FLAG)) {
            delta = readDword(POSITION);
            if (!delta) break;
        }
        position += delta;
        if (readBit(STRENGTH_FLAG)) strength += readDword(STRENGTH);
        pulses.push_back(position);
    }
    return pulses.size() == count;
}

}  // namespace

bool readPulses(const std::vector<uint8_t>& data, std::vector<std::vector<uint32_t>>& halfTracks,
                bool* writeProtected) {
    halfTracks.assign(84, {});
    if (data.size() < 24 || std::memcmp(data.data(), "P64-1541", 8) != 0) return false;
    if (le32(&data[8]) != 0) return false;                       // version
    const uint32_t flags = le32(&data[12]), size = le32(&data[16]), crc = le32(&data[20]);
    if (24 + (size_t)size > data.size() || crc32(&data[24], size) != crc) return false;
    if (writeProtected) *writeProtected = flags & 1;
    size_t at = 24;
    const size_t end = 24 + (size_t)size;
    while (at + 12 <= end) {
        const uint8_t* sig = &data[at];
        const uint32_t n = le32(&data[at + 4]), c = le32(&data[at + 8]);
        at += 12;
        if (at + n > end) return false;
        if (n && crc32(&data[at], n) != c) return false;
        // "HTP" + half-track (VICE's numbering: 2 = track 1); bit 7: side 1
        if (n && sig[0] == 'H' && sig[1] == 'T' && sig[2] == 'P' && !(sig[3] & 0x80)) {
            const int h = (sig[3] & 0x7f) - 2;
            if (h >= 0 && h < 84 && !readStream(&data[at], n, halfTracks[(size_t)h])) return false;
        }
        at += n;
        if (!n && sig[0] == 'D' && sig[1] == 'O' && sig[2] == 'N' && sig[3] == 'E') break;
    }
    return true;
}

std::vector<uint8_t> toGcr(const std::vector<uint32_t>& pulses, uint32_t len) {
    std::vector<uint8_t> out((len + 7) / 8, 0);
    if (!len) return out;
    const uint32_t range = SAMPLES_PER_ROTATION, incHi = range / len, incLo = range % len;
    size_t cur = 0;
    const bool any = !pulses.empty();
    uint32_t posHi = any ? pulses[0] - 1 : 0, posLo = len - 1;
    for (uint32_t bit = 0; bit < len; ++bit) {
        posHi += incHi;
        posLo += incLo;
        while (posLo >= len) { posLo -= len; ++posHi; }
        for (;;) {
            if (any && cur < pulses.size() && pulses[cur] < posHi) {
                posHi = pulses[cur] + incHi - 20;               // 1.25 us of headroom
                posLo = incLo;
                ++cur;
                out[bit >> 3] |= (uint8_t)(1 << (~bit & 7));
            } else if (posHi >= range) {
                posHi -= range;
                cur = 0;
                continue;
            }
            break;
        }
    }
    return out;
}

}  // namespace p64
