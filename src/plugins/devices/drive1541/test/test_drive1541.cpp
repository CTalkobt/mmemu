#include "test_harness.h"
#include "plugins/devices/drive1541/main/gcr.h"
#include <cstdlib>
#include <vector>

// GCR (#341): a track encoded as the 1541 formats it decodes back to its
// sectors; four bytes are five GCR bytes
TEST_CASE(gcr_track_round_trip) {
    for (int track : {1, 18, 25, 35}) {
        const int n = gcr::sectorsOnTrack(track);
        std::vector<uint8_t> sectors((size_t)n * 256);
        for (size_t i = 0; i < sectors.size(); ++i) sectors[i] = (uint8_t)(i * 7 + track);
        const auto t = gcr::encodeTrack(track, sectors.data(), 'A', 'B');
        ASSERT_EQ((int)t.size(), gcr::trackBytes(track));
        std::vector<uint8_t> out;
        std::vector<bool> found;
        gcr::decodeTrack(track, t, out, found);
        for (int s = 0; s < n; ++s) ASSERT(found[(size_t)s]);
        ASSERT(out == sectors);
    }
    const uint8_t in[4] = {0x08, 0x12, 0x00, 0x01};
    uint8_t g[5], back[4];
    gcr::encode4(in, g);
    ASSERT_EQ((int)g[0], 0x52);                     // $0 -> 01010, $8 -> 01001
    ASSERT(gcr::decode4(g, back));
    ASSERT(back[0] == 0x08 && back[1] == 0x12 && back[2] == 0x00 && back[3] == 0x01);
}

#include "plugins/devices/drive1541/main/drive1541.h"
#include "libmem/main/ibus.h"
#include "plugins/6502/main/cpu6502.h"
#include <cstdio>
#include <fstream>
#include <string>

namespace {
// A 35-track D64 with an error map: every sector of track t has the error
// byte codes[t] (1 if none), and its bytes are (track, sector, i)
std::string writeErrorD64(const std::string& path, const std::vector<std::pair<int, uint8_t>>& codes) {
    std::vector<uint8_t> img, err;
    for (int t = 1; t <= 35; ++t) {
        uint8_t code = 1;
        for (auto& c : codes) if (c.first == t) code = c.second;
        for (int s = 0; s < gcr::sectorsOnTrack(t); ++s) {
            for (int i = 0; i < 256; ++i) img.push_back((uint8_t)(t * 16 + s + i));
            err.push_back(code);
        }
    }
    const size_t bam = 357 * 256;
    img[bam + 0xA2] = 'A';
    img[bam + 0xA3] = 'B';
    img.insert(img.end(), err.begin(), err.end());
    std::ofstream(path, std::ios::binary).write((const char*)img.data(), (std::streamsize)img.size());
    return path;
}

// Run a DOS job on the drive (buffer 0-4, at $0300 + buffer * $100); its
// result code
int runJob(Drive1541& d, uint8_t job, int track, int sector, int buffer = 0) {
    IBus* b = d.bus();
    b->write8((uint32_t)(0x06 + buffer * 2), (uint8_t)track);
    b->write8((uint32_t)(0x07 + buffer * 2), (uint8_t)sector);
    b->write8((uint32_t)buffer, job);
    for (int i = 0; i < 400; ++i) {
        d.tick(10000);
        const uint8_t r = b->peek8((uint32_t)buffer);
        if (r < 0x80) return r;
    }
    return -1;
}
}  // namespace

// The D64 error map (#360): a read job on a sector with an error byte gets
// that code back from the DOS, as VICE's drive/scanner*e expect
TEST_CASE(drive1541_error_map_job_codes) {
    Drive1541 d;
    if (!d.romLoaded()) return;                     // no DOS ROM: nothing to run
    const std::vector<std::pair<int, uint8_t>> codes = {{2, 2}, {3, 3}, {4, 4}, {5, 5}, {9, 9}, {11, 11}};
    const char* tmp = std::getenv("TMPDIR");
    const std::string path = writeErrorD64(std::string(tmp ? tmp : "/tmp") + "/mmemu_errmap.d64", codes);
    ASSERT(d.mountDisk(8, path));
    d.reset();
    d.setDdr(0x3F);
    d.writePort(0x00);                              // the computer holds no line
    d.tick(2000000);                                // the DOS starts up
    ASSERT_EQ(runJob(d, 0xB0, 18, 0), 1);           // SEEK: the disk ID from a header
    // As drive/scanner35e: sector 16 of each track in turn, into buffer 4.
    // Errors 20, 21, 27 and 29 lose the sector; 22 and 23 still read it
    IBus* b = d.bus();
    for (int t = 1; t <= 12; ++t) {
        uint8_t code = 1;
        for (auto& c : codes) if (c.first == t) code = c.second;
        for (int i = 0; i < 256; ++i) b->write8((uint32_t)(0x0700 + i), 0);
        const int r = runJob(d, 0x80, t, 16, 4);
        bool same = true;
        for (int i = 0; i < 256; ++i) same &= b->peek8((uint32_t)(0x0700 + i)) == (uint8_t)(t * 16 + 16 + i);
        printf("    track %d: error byte %d, job code %d, data %s\n", t, code, r, same ? "ok" : "wrong");
        ASSERT_EQ(r, (int)code);
        if (code == 1 || code == 4 || code == 5) ASSERT(same);
    }
    std::remove(path.c_str());
}
