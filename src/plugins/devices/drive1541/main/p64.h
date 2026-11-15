#pragma once

#include <cstdint>
#include <vector>

/**
 * P64 disk images (flux pulses), read into the GCR bit cells the 1541's head
 * works on. The format and the decoder are BeRo's P64 reference
 * implementation (VICE src/lib/p64, zlib licence; see p64.cpp).
 */
namespace p64 {

/** Pulse positions per turn of the disk. */
constexpr uint32_t SAMPLES_PER_ROTATION = 3200000;

/**
 * The pulse positions of each half-track of a P64 (side 0): `halfTracks[h]`
 * for the drive's half-track h (0 = track 1), sorted. False if the data
 * isn't a valid P64 (signature, version, CRCs).
 */
bool readPulses(const std::vector<uint8_t>& data, std::vector<std::vector<uint32_t>>& halfTracks,
                bool* writeProtected = nullptr);

/**
 * A half-track's pulses as `bits` bit cells (packed, MSB first), as VICE's
 * P64PulseStreamConvertToGCR: the cell clock restarts at each pulse.
 */
std::vector<uint8_t> toGcr(const std::vector<uint32_t>& pulses, uint32_t bits);

}  // namespace p64
