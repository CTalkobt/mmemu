#include "gcr.h"

namespace gcr {

namespace {
const uint8_t TO_GCR[16] = {0x0A, 0x0B, 0x12, 0x13, 0x0E, 0x0F, 0x16, 0x17,
                            0x09, 0x19, 0x1A, 0x1B, 0x0D, 0x1D, 0x1E, 0x15};

int fromGcr(unsigned v) {
    for (int i = 0; i < 16; ++i)
        if (TO_GCR[i] == v) return i;
    return -1;
}
}  // namespace

int trackBytes(int track) {
    return track <= 17 ? 7692 : track <= 24 ? 7142 : track <= 30 ? 6666 : 6250;
}

int speedZone(int track) {
    return track <= 17 ? 3 : track <= 24 ? 2 : track <= 30 ? 1 : 0;
}

int sectorsOnTrack(int track) {
    return track <= 17 ? 21 : track <= 24 ? 19 : track <= 30 ? 18 : 17;
}

void encode4(const uint8_t in[4], uint8_t out[5]) {
    uint64_t bits = 0;
    for (int i = 0; i < 4; ++i)
        bits = (bits << 10) | ((uint64_t)TO_GCR[in[i] >> 4] << 5) | TO_GCR[in[i] & 0x0F];
    for (int i = 4; i >= 0; --i) { out[i] = (uint8_t)bits; bits >>= 8; }
}

bool decode4(const uint8_t in[5], uint8_t out[4]) {
    uint64_t bits = 0;
    for (int i = 0; i < 5; ++i) bits = (bits << 8) | in[i];
    bool ok = true;
    for (int i = 3; i >= 0; --i) {
        const int lo = fromGcr((unsigned)(bits & 0x1F)), hi = fromGcr((unsigned)((bits >> 5) & 0x1F));
        if (lo < 0 || hi < 0) ok = false;
        out[i] = (uint8_t)(((hi < 0 ? 0 : hi) << 4) | (lo < 0 ? 0 : lo));
        bits >>= 10;
    }
    return ok;
}

namespace {
void putGcr(std::vector<uint8_t>& t, const uint8_t* raw, int n) {
    for (int i = 0; i < n; i += 4) {
        uint8_t g[5];
        encode4(raw + i, g);
        t.insert(t.end(), g, g + 5);
    }
}
}  // namespace

std::vector<uint8_t> encodeTrack(int track, const uint8_t* sectors, uint8_t id1, uint8_t id2,
                                 const uint8_t* errors) {
    const int n = sectorsOnTrack(track), size = trackBytes(track);
    // Each sector: 5 SYNC + 10 header + 9 gap + 5 SYNC + 325 data = 354
    // bytes; the rest of the track is the gaps after them
    const int gap = (size - n * 354) / n;
    std::vector<uint8_t> t;
    t.reserve((size_t)size);
    for (int s = 0; s < n; ++s) {
        const uint8_t err = errors ? errors[s] : 1;
        const uint8_t* d = sectors + s * 256;
        if (err != 3) t.insert(t.end(), 5, 0xFF);
        else t.insert(t.end(), 5, 0x55);                    // no SYNC on the whole track
        uint8_t h[8] = {0x08, 0, (uint8_t)s, (uint8_t)track, id2, id1, 0x0F, 0x0F};
        if (err == 11) h[4] ^= 0xFF;                        // ID mismatch
        h[1] = (uint8_t)(h[2] ^ h[3] ^ h[4] ^ h[5]);
        if (err == 9) h[1] ^= 0xFF;
        if (err == 2) h[0] = 0x00;                          // no header
        putGcr(t, h, 8);
        t.insert(t.end(), 9, 0x55);
        t.insert(t.end(), 5, err == 3 ? 0x55 : 0xFF);
        uint8_t b[260];
        b[0] = err == 4 ? 0x00 : 0x07;
        uint8_t chk = 0;
        for (int i = 0; i < 256; ++i) { b[1 + i] = d[i]; chk ^= d[i]; }
        b[257] = err == 5 ? (uint8_t)(chk ^ 0xFF) : chk;
        b[258] = b[259] = 0;
        putGcr(t, b, 260);
        t.insert(t.end(), (size_t)gap, 0x55);
    }
    t.resize((size_t)size, 0x55);
    return t;
}

void decodeTrack(int track, const std::vector<uint8_t>& g, std::vector<uint8_t>& out, std::vector<bool>& found) {
    const int n = sectorsOnTrack(track);
    out.assign((size_t)n * 256, 0);
    found.assign((size_t)n, false);
    const size_t len = g.size();
    if (len < 20) return;
    auto at = [&](size_t i) { return g[i % len]; };
    auto read = [&](size_t from, uint8_t* raw, int bytes) {
        for (int i = 0; i < bytes; i += 4) {
            uint8_t in[5];
            for (int k = 0; k < 5; ++k) in[k] = at(from + (size_t)(i / 4) * 5 + (size_t)k);
            decode4(in, raw + i);
        }
    };
    int sector = -1;
    for (size_t i = 0; i < len; ++i) {
        // The first byte after a SYNC ($FF run)
        if (at(i) != 0xFF || at(i + 1) == 0xFF) continue;
        if (i == 0 || at(i - 1) != 0xFF) continue;
        const size_t p = i + 1;
        uint8_t first[4];
        read(p, first, 4);
        if (first[0] == 0x08) {
            uint8_t h[8];
            read(p, h, 8);
            sector = h[3] == track && h[2] < n ? h[2] : -1;
        } else if (first[0] == 0x07 && sector >= 0) {
            uint8_t b[260];
            read(p, b, 260);
            for (int k = 0; k < 256; ++k) out[(size_t)sector * 256 + (size_t)k] = b[1 + k];
            found[(size_t)sector] = true;
            sector = -1;
        }
    }
}

}  // namespace gcr
