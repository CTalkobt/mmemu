#include "gif_writer.h"
#include <cstdio>
#include <unordered_map>

namespace gif {

namespace {

void put16(std::vector<uint8_t>& out, int v) {
    out.push_back((uint8_t)(v & 0xFF));
    out.push_back((uint8_t)((v >> 8) & 0xFF));
}

// Packs variable-width codes LSB-first into 255-byte sub-blocks.
class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : m_out(out) {}
    void put(int code, int bits) {
        m_acc |= (uint32_t)code << m_nbits;
        m_nbits += bits;
        while (m_nbits >= 8) { byte((uint8_t)(m_acc & 0xFF)); m_acc >>= 8; m_nbits -= 8; }
    }
    void finish() {
        if (m_nbits > 0) byte((uint8_t)(m_acc & 0xFF));
        m_acc = 0; m_nbits = 0;
        flushBlock();
        m_out.push_back(0);   // block terminator
    }
private:
    void byte(uint8_t b) { m_block.push_back(b); if (m_block.size() == 255) flushBlock(); }
    void flushBlock() {
        if (m_block.empty()) return;
        m_out.push_back((uint8_t)m_block.size());
        m_out.insert(m_out.end(), m_block.begin(), m_block.end());
        m_block.clear();
    }
    std::vector<uint8_t>& m_out;
    std::vector<uint8_t> m_block;
    uint32_t m_acc = 0;
    int m_nbits = 0;
};

// LZW-compress palette indices (GIF variant: clear/end codes, 12-bit maximum).
void lzw(std::vector<uint8_t>& out, const std::vector<uint8_t>& idx, int minCode) {
    out.push_back((uint8_t)minCode);
    BitWriter bw(out);
    const int clear = 1 << minCode, eoi = clear + 1;
    int codeSize = minCode + 1, next = eoi + 1;
    std::unordered_map<uint32_t, int> dict;   // (prefix << 8 | byte) -> code
    auto reset = [&] { dict.clear(); codeSize = minCode + 1; next = eoi + 1; };

    bw.put(clear, codeSize);
    int prefix = -1;
    for (uint8_t c : idx) {
        if (prefix < 0) { prefix = c; continue; }
        uint32_t key = ((uint32_t)prefix << 8) | c;
        auto it = dict.find(key);
        if (it != dict.end()) { prefix = it->second; continue; }
        bw.put(prefix, codeSize);
        if (next == 4096) {                 // table full: start over
            bw.put(clear, codeSize);
            reset();
        } else {
            dict[key] = next++;
            // The decoder's table lags one entry behind ours: widen codes
            // once our next code no longer fits the current width + 1 entry.
            if (next > (1 << codeSize) && codeSize < 12) ++codeSize;
        }
        prefix = c;
    }
    if (prefix >= 0) bw.put(prefix, codeSize);
    bw.put(eoi, codeSize);
    bw.finish();
}

} // namespace

std::vector<uint8_t> encode(const std::vector<Frame>& frames, int width, int height, std::string* err) {
    auto fail = [&](const char* msg) { if (err) *err = msg; return std::vector<uint8_t>{}; };
    if (frames.empty()) return fail("no frames");
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535) return fail("bad size");
    const size_t n = (size_t)width * height;

    // One global palette from every colour used
    std::unordered_map<uint32_t, uint8_t> index;
    std::vector<uint32_t> palette;
    for (const Frame& f : frames) {
        if (f.argb.size() != n) return fail("frame size mismatch");
        for (uint32_t p : f.argb) {
            uint32_t rgb = p & 0xFFFFFF;
            if (index.count(rgb)) continue;
            if (palette.size() == 256) return fail("more than 256 colours");
            index[rgb] = (uint8_t)palette.size();
            palette.push_back(rgb);
        }
    }
    int bits = 1;
    while ((1 << bits) < (int)palette.size()) ++bits;   // table holds 2^bits entries
    const int minCode = bits < 2 ? 2 : bits;

    std::vector<uint8_t> out;
    const char* sig = "GIF89a";
    out.insert(out.end(), sig, sig + 6);
    put16(out, width);
    put16(out, height);
    out.push_back((uint8_t)(0x80 | ((bits - 1) << 4) | (bits - 1)));   // global table, its size
    out.push_back(0);   // background colour index
    out.push_back(0);   // pixel aspect ratio
    for (int i = 0; i < (1 << bits); ++i) {
        uint32_t c = i < (int)palette.size() ? palette[i] : 0;
        out.push_back((uint8_t)(c >> 16)); out.push_back((uint8_t)(c >> 8)); out.push_back((uint8_t)c);
    }

    const bool animated = frames.size() > 1;
    if (animated) {   // NETSCAPE2.0 extension: loop forever
        const uint8_t app[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0',
                               0x03, 0x01, 0x00, 0x00, 0x00};
        out.insert(out.end(), app, app + sizeof app);
    }

    std::vector<uint8_t> idx(n);
    for (const Frame& f : frames) {
        if (animated) {   // graphic control extension: delay, no transparency
            const uint8_t gce[] = {0x21, 0xF9, 0x04, 0x04};   // disposal 1: leave in place
            out.insert(out.end(), gce, gce + sizeof gce);
            put16(out, f.delayCs < 0 ? 0 : (f.delayCs > 65535 ? 65535 : f.delayCs));
            out.push_back(0); out.push_back(0);
        }
        out.push_back(0x2C);   // image descriptor
        put16(out, 0); put16(out, 0); put16(out, width); put16(out, height);
        out.push_back(0);      // no local table, not interlaced
        for (size_t i = 0; i < n; ++i) idx[i] = index[f.argb[i] & 0xFFFFFF];
        lzw(out, idx, minCode);
    }
    out.push_back(0x3B);   // trailer
    return out;
}

bool write(const std::string& path, const std::vector<Frame>& frames, int width, int height, std::string* err) {
    std::vector<uint8_t> data = encode(frames, width, height, err);
    if (data.empty()) return false;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (std::fclose(f) == 0) && ok;
    if (!ok && err) *err = "write failed: " + path;
    return ok;
}

} // namespace gif
