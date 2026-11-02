#pragma once

#include <cstdint>
#include <string>
#include <vector>

class IAudioInspect;
struct MachineDescriptor;

/**
 * Helpers for sound-chip scopes (#30), shared by the GUI SID Scope pane and
 * its tests.
 */
namespace scope {

/** Every IOHandler of the machine that implements IAudioInspect, in registry order. */
std::vector<IAudioInspect*> findAudioInspect(MachineDescriptor* desc);

/**
 * Name of the closest equal-tempered note (A4 = 440 Hz), e.g. "A4", "C#5".
 * Empty below 8 Hz. centsOut (optional) receives the offset, -50..+50.
 */
std::string noteName(double hz, int* centsOut = nullptr);

/**
 * SID control register as text: waveforms joined by '+', then SYNC, RING,
 * TEST and GATE flags, e.g. "Pulse+Saw Sync Gate". "-" when no waveform.
 */
std::string controlText(uint8_t control);

/**
 * Where a window of `window` samples should start in samples[0..count) so a
 * rising zero crossing sits at its left edge: the latest such crossing that
 * still leaves a full window. The crossing must follow a dip below -hyst,
 * so noise around zero does not trigger. -1 when there is none.
 */
int findTrigger(const float* samples, int count, int window, float hyst = 0.02f);

/** SID envelope times in ms for a rate nibble: attack, or decay/release (3x). */
int attackMs(int nibble);
int decayReleaseMs(int nibble);

} // namespace scope
