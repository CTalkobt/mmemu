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

/** Envelope phase as text: "Idle", "Attack", "Decay", "Sustain", "Release". */
const char* phaseName(int envPhase);

/** Scope colours (0xAARRGGBB), shared by the GUI pane and image snapshots. */
constexpr uint32_t COLOUR_BG    = 0xFF101418;
constexpr uint32_t COLOUR_GRID  = 0xFF283038;
constexpr uint32_t COLOUR_TEXT  = 0xFFC8C8C8;
constexpr uint32_t COLOUR_DIM   = 0xFF788088;
/** Trace colour for a channel: voices 0-2, then the output. */
uint32_t traceColour(int channel);

/** Timebases offered by the scope, in ms. */
constexpr int TIMEBASES_MS[] = {2, 5, 10, 20, 50};

/** Which samples of a channel's history one trace shows. */
struct TraceWindow {
    int  start = 0;           ///< first sample index in the history buffer
    int  length = 0;          ///< samples across the trace (0: nothing to draw)
    bool untriggered = false; ///< trigger wanted, signal present, but no crossing found
};

/**
 * Pick the samples for a trace: the last timebaseMs of history, or with
 * `triggered` the window starting at the latest rising zero crossing.
 */
TraceWindow traceWindow(const float* samples, int count, int sampleRate, int timebaseMs, bool triggered);

/** One line per voice describing its state, e.g. "Voice 1: Pulse Gate, 440.0 Hz (A4 +0c), ...". */
std::string voiceSummary(IAudioInspect* insp, int chip);

/** Options for renderScope(). */
struct RenderOptions {
    int  chip = 0;
    int  timebaseMs = 20;
    bool triggered = true;
    int  width = 480;       ///< image width in pixels
    int  rowHeight = 100;   ///< height of each voice / output row
};

/** An ARGB image of the scope: same layout as the GUI pane, with a built-in 5x7 font. */
struct Image {
    int width = 0, height = 0;
    std::vector<uint32_t> argb;
};

/** Draw the scope for one chip. Empty image if the chip does not exist. */
Image renderScope(IAudioInspect* insp, const RenderOptions& opt);

} // namespace scope
