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
