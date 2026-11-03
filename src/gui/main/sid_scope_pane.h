#pragma once

#include <wx/wx.h>
#include <cstdint>
#include <vector>
#include "libdevices/main/iaudio_inspect.h"

struct MachineDescriptor;

/**
 * SID oscilloscope (#30).
 *
 * One trace per voice plus the chip's mixed output, read from the chip's
 * IAudioInspect scope history. Each trace is either triggered (a rising
 * zero crossing is held at the left edge, so steady tones stand still) or
 * free-running. Each voice row also shows its waveform, frequency and
 * nearest note, pulse width, ADSR settings and an envelope diagram with the
 * current phase and level.
 */
class SidScopePane : public wxPanel {
public:
    explicit SidScopePane(wxWindow* parent);

    /** Attach to a machine (nullptr detaches). */
    void SetMachine(MachineDescriptor* desc);
    /** Called from the GUI's update loop; redraws when shown and the machine has advanced. */
    void RefreshValues(uint64_t cycles);

private:
    class Canvas;
    struct Chip { IAudioInspect* insp; int index; };

    void OnSettingChanged();
    void Paint(wxDC& dc, const wxSize& size);
    void PaintRow(wxDC& dc, const wxRect& r, int channel, bool output);
    void PaintEnvelope(wxDC& dc, const wxRect& r, const IAudioInspect::VoiceInfo& vi,
                       const wxColour& colour);

    wxChoice* m_chip = nullptr;
    wxChoice* m_timebase = nullptr;
    wxChoice* m_trigger = nullptr;
    Canvas*   m_canvas = nullptr;

    std::vector<Chip> m_chips;
    std::vector<float> m_buf;
    uint64_t m_lastCycles = UINT64_MAX;
};
