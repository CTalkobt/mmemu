#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * Commodore GCR (group code recording) for 1541 disks: 4 bits to 5, so no
 * run of more than two 0 bits and no ten 1 bits outside a SYNC.
 */
namespace gcr {

/** Bytes on a track of a speed zone (1541 at 300 rpm): tracks 1-17 7692,
 *  18-24 7142, 25-30 6666, 31 and up 6250. */
int trackBytes(int track);
/** The speed zone the DOS uses for a track (3: tracks 1-17 ... 0: 31-). */
int speedZone(int track);
/** The gap after each sector's data block (VICE's: 8, 17, 12, 9 bytes). */
int gapSize(int track);
/** Sectors on a track of a 1541 disk (21, 19, 18, 17). */
int sectorsOnTrack(int track);

/** 4 bytes to 5 GCR bytes. */
void encode4(const uint8_t in[4], uint8_t out[5]);
/** 5 GCR bytes to 4; false if a 5-bit group isn't a GCR code. */
bool decode4(const uint8_t in[5], uint8_t out[4]);

/**
 * One track of a D64 as a 1541 formats it: for each sector a SYNC, the
 * header block ($08, checksum, sector, track, ID2, ID1, $0F, $0F), 9 bytes
 * of $55, a SYNC, the data block ($07, 256 bytes, checksum) and the zone's
 * gap; $55 fills the rest of the track. The track is turned `skew` bytes on
 * (VICE's layout, trackSkew). `sectors` holds the track's
 * sectors' 256 bytes each; `errors` (or null) the D64's error byte for each
 * (1 = OK; 2 header not found, 3 no SYNC, 4 data block not found,
 * 5 checksum error, 9 header checksum error, 11 ID mismatch as the 1541's
 * error codes 20, 21, 22, 23, 27, 29).
 */
std::vector<uint8_t> encodeTrack(int track, const uint8_t* sectors, uint8_t id1, uint8_t id2,
                                 const uint8_t* errors = nullptr, size_t skew = 0);

/**
 * Where VICE starts a D64's track (in bytes) given where the track before
 * started (`prev`, 0 before track 1): its sectors' length on from there,
 * and the time a step takes. Gives skew1's track-to-track offsets.
 */
size_t trackSkew(int track, size_t prev);

/**
 * The sectors of a GCR track back into 256-byte blocks: `out` gets
 * sectorsOnTrack(track) * 256 bytes; `found` marks the sectors read. A
 * header (or data block) split over the track's end is read across it.
 */
void decodeTrack(int track, const std::vector<uint8_t>& gcrTrack, std::vector<uint8_t>& out,
                 std::vector<bool>& found);

}  // namespace gcr
