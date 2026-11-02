#include "audio_scope.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/iaudio_inspect.h"
#include "libdevices/main/io_registry.h"
#include <cmath>
#include <vector>

namespace scope {

std::vector<IAudioInspect*> findAudioInspect(MachineDescriptor* desc) {
    std::vector<IAudioInspect*> out;
    if (!desc || !desc->ioRegistry) return out;
    std::vector<IOHandler*> handlers;
    desc->ioRegistry->enumerate(handlers);
    for (auto* h : handlers)
        if (auto* a = dynamic_cast<IAudioInspect*>(h)) out.push_back(a);
    return out;
}

std::string noteName(double hz, int* centsOut) {
    static const char* const NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (centsOut) *centsOut = 0;
    if (!(hz >= 8.0)) return "";
    double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
    int n = (int)std::lround(midi);
    if (centsOut) *centsOut = (int)std::lround((midi - n) * 100.0);
    int octave = n / 12 - 1;   // MIDI 60 = C4
    return std::string(NAMES[((n % 12) + 12) % 12]) + std::to_string(octave);
}

std::string controlText(uint8_t cr) {
    std::string wave;
    auto add = [&](uint8_t bit, const char* name) {
        if (!(cr & bit)) return;
        if (!wave.empty()) wave += '+';
        wave += name;
    };
    add(0x10, "Tri");
    add(0x20, "Saw");
    add(0x40, "Pulse");
    add(0x80, "Noise");
    std::string s = wave.empty() ? "-" : wave;
    if (cr & 0x02) s += " Sync";
    if (cr & 0x04) s += " Ring";
    if (cr & 0x08) s += " Test";
    if (cr & 0x01) s += " Gate";
    return s;
}

int findTrigger(const float* s, int count, int window, float hyst) {
    if (!s || window <= 0 || count < window + 1) return -1;
    // Scan back from the latest start that leaves a full window. A crossing
    // counts once the signal has been below -hyst since the previous crossing.
    int last = count - window;
    for (int i = last; i >= 1; --i) {
        if (!(s[i - 1] < 0.0f && s[i] >= 0.0f)) continue;
        for (int j = i - 1; j >= 0 && s[j] < 0.0f; --j)
            if (s[j] < -hyst) return i;
    }
    return -1;
}

int attackMs(int nibble) {
    static const int MS[16] = {2, 8, 16, 24, 38, 56, 68, 80, 100, 250, 500, 800, 1000, 3000, 5000, 8000};
    return MS[nibble & 15];
}

int decayReleaseMs(int nibble) { return attackMs(nibble) * 3; }

} // namespace scope
