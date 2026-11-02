#include "test_harness.h"
#include "libdebug/main/gif_writer.h"
#include <cstdint>
#include <random>
#include <vector>

// GIF writer (#30): decode the output with an independent GIF/LZW reader.

namespace {

struct Decoded {
    int width = 0, height = 0, loop = -1;
    std::vector<uint32_t> palette;
    std::vector<std::vector<uint8_t>> frames;   // palette indices
    std::vector<int> delays;
    bool ok = false;
};

std::vector<uint8_t> lzwDecode(const std::vector<uint8_t>& data, int minCode, size_t expect) {
    std::vector<uint8_t> out;
    const int clear = 1 << minCode, eoi = clear + 1;
    int size = minCode + 1, next = eoi + 1, prev = -1;
    std::vector<std::vector<uint8_t>> table;
    auto reset = [&] {
        table.assign(4096, {});
        for (int i = 0; i < clear; ++i) table[i] = {(uint8_t)i};
        size = minCode + 1; next = eoi + 1; prev = -1;
    };
    reset();
    size_t bit = 0;
    while (bit + size <= data.size() * 8) {
        int code = 0;
        for (int i = 0; i < size; ++i, ++bit) code |= ((data[bit >> 3] >> (bit & 7)) & 1) << i;
        if (code == clear) { reset(); continue; }
        if (code == eoi) break;
        std::vector<uint8_t> entry;
        if (code < next && !table[code].empty()) entry = table[code];
        else if (code == next && prev >= 0) { entry = table[prev]; entry.push_back(table[prev][0]); }
        else return {};   // invalid stream
        out.insert(out.end(), entry.begin(), entry.end());
        if (prev >= 0 && next < 4096) {
            table[next] = table[prev];
            table[next].push_back(entry[0]);
            ++next;
            if (next == (1 << size) && size < 12) ++size;
        }
        prev = code;
    }
    if (out.size() != expect) return {};
    return out;
}

Decoded decode(const std::vector<uint8_t>& g) {
    Decoded d;
    size_t p = 0;
    auto u8 = [&]() -> int { return p < g.size() ? g[p++] : -1; };
    auto u16 = [&]() { int lo = u8(), hi = u8(); return lo | (hi << 8); };
    if (g.size() < 13 || std::string(g.begin(), g.begin() + 6) != "GIF89a") return d;
    p = 6;
    d.width = u16(); d.height = u16();
    int flags = u8(); u8(); u8();
    if (!(flags & 0x80)) return d;
    int entries = 1 << ((flags & 7) + 1);
    for (int i = 0; i < entries; ++i) { int r = u8(), gg = u8(), b = u8(); d.palette.push_back((uint32_t)(r << 16 | gg << 8 | b)); }
    int delay = 0;
    for (;;) {
        int b = u8();
        if (b == 0x3B) { d.ok = true; return d; }
        if (b == 0x21) {
            int label = u8();
            std::vector<uint8_t> blk;
            for (int n = u8(); n > 0; n = u8()) for (int i = 0; i < n; ++i) blk.push_back((uint8_t)u8());
            if (label == 0xF9 && blk.size() >= 3) delay = blk[1] | (blk[2] << 8);
            if (label == 0xFF && blk.size() >= 14) d.loop = blk[12] | (blk[13] << 8);   // "NETSCAPE2.0" 01 lo hi
            continue;
        }
        if (b != 0x2C) return d;
        u16(); u16(); int w = u16(), h = u16(); u8();
        int minCode = u8();
        std::vector<uint8_t> data;
        for (int n = u8(); n > 0; n = u8()) for (int i = 0; i < n; ++i) data.push_back((uint8_t)u8());
        d.frames.push_back(lzwDecode(data, minCode, (size_t)w * h));
        d.delays.push_back(delay);
        if (d.frames.back().empty()) return d;
    }
}

} // namespace

TEST_CASE(gif_writer_round_trip_animated) {
    const int W = 200, H = 150;   // 30000 random pixels: the LZW table fills and resets
    std::mt19937 rng(42);
    const uint32_t cols[5] = {0xFF000000, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF};
    std::vector<gif::Frame> frames(3);
    for (int f = 0; f < 3; ++f) {
        frames[f].argb.resize(W * H);
        for (auto& px : frames[f].argb) px = cols[rng() % (f == 2 ? 2 : 5)];
        frames[f].delayCs = 4 + f;
    }
    std::string err;
    std::vector<uint8_t> g = gif::encode(frames, W, H, &err);
    ASSERT(!g.empty());
    Decoded d = decode(g);
    ASSERT(d.ok);
    ASSERT(d.width == W && d.height == H && d.loop == 0);
    ASSERT(d.palette.size() == 8);   // 5 colours -> 8-entry table
    ASSERT(d.frames.size() == 3);
    for (int f = 0; f < 3; ++f) {
        ASSERT(d.delays[f] == 4 + f);
        ASSERT(d.frames[f].size() == (size_t)W * H);
        for (int i = 0; i < W * H; ++i)
            ASSERT(d.palette[d.frames[f][i]] == (frames[f].argb[i] & 0xFFFFFF));
    }
}

TEST_CASE(gif_writer_single_pixel_and_errors) {
    std::vector<gif::Frame> one(1);
    one[0].argb = {0xFF123456};
    std::vector<uint8_t> g = gif::encode(one, 1, 1);
    Decoded d = decode(g);
    ASSERT(d.ok && d.frames.size() == 1 && d.loop == -1);   // a still: no loop extension
    ASSERT(d.palette[d.frames[0][0]] == 0x123456);

    std::string err;
    ASSERT(gif::encode({}, 1, 1, &err).empty() && err == "no frames");
    ASSERT(gif::encode(one, 2, 1, &err).empty() && err == "frame size mismatch");
    std::vector<gif::Frame> many(1);
    for (uint32_t i = 0; i < 257; ++i) many[0].argb.push_back(0xFF000000 | i);
    ASSERT(gif::encode(many, 257, 1, &err).empty() && err == "more than 256 colours");
}
