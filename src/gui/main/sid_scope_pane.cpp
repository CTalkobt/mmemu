#include "sid_scope_pane.h"
#include "libdebug/main/audio_scope.h"
#include <wx/dcbuffer.h>
#include <algorithm>
#include <cmath>

namespace {

const int TIMEBASES_MS[] = {2, 5, 10, 20, 50};
const int DEFAULT_TIMEBASE = 3;   // 20 ms

// Scope colours: a dark screen in both light and dark themes
const wxColour BG(16, 20, 24);
const wxColour GRID(40, 48, 56);
const wxColour TEXT(200, 200, 200);
const wxColour DIM(120, 128, 136);
const wxColour TRACE[4] = {wxColour(80, 220, 100), wxColour(80, 190, 255),
                           wxColour(255, 170, 70), wxColour(225, 225, 225)};

const char* phaseName(IAudioInspect::EnvPhase p) {
    switch (p) {
        case IAudioInspect::EnvPhase::Attack:  return "Attack";
        case IAudioInspect::EnvPhase::Decay:   return "Decay";
        case IAudioInspect::EnvPhase::Sustain: return "Sustain";
        case IAudioInspect::EnvPhase::Release: return "Release";
        default:                               return "Idle";
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Canvas: double-buffered drawing surface; the pane does the painting
// ---------------------------------------------------------------------------

class SidScopePane::Canvas : public wxPanel {
public:
    Canvas(SidScopePane* owner, wxWindow* parent) : wxPanel(parent, wxID_ANY), m_owner(owner) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(300, 280));
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            m_owner->Paint(dc, GetClientSize());
        });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Refresh(false); e.Skip(); });
    }

private:
    SidScopePane* m_owner;
};

// ---------------------------------------------------------------------------

SidScopePane::SidScopePane(wxWindow* parent) : wxPanel(parent, wxID_ANY) {
    auto* top = new wxBoxSizer(wxVERTICAL);
    auto* row = new wxBoxSizer(wxHORIZONTAL);

    m_chip = new wxChoice(this, wxID_ANY);
    m_chip->SetToolTip("Sound chip to show");

    wxArrayString tbs;
    for (int ms : TIMEBASES_MS) tbs.Add(wxString::Format("%d ms", ms));
    m_timebase = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, tbs);
    m_timebase->SetSelection(DEFAULT_TIMEBASE);
    m_timebase->SetToolTip("Time across each trace");

    wxArrayString trig;
    trig.Add("Triggered");
    trig.Add("Free run");
    m_trigger = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, trig);
    m_trigger->SetSelection(0);
    m_trigger->SetToolTip("Triggered: each trace starts at a rising zero crossing, so steady tones "
                          "stand still. Free run: the latest samples, scrolling.");

    row->Add(m_chip, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    row->Add(m_timebase, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    row->Add(m_trigger, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(row, 0, wxALL, 4);

    m_canvas = new Canvas(this, this);
    top->Add(m_canvas, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
    SetSizer(top);

    auto changed = [this](wxCommandEvent&) { OnSettingChanged(); };
    m_chip->Bind(wxEVT_CHOICE, changed);
    m_timebase->Bind(wxEVT_CHOICE, changed);
    m_trigger->Bind(wxEVT_CHOICE, changed);

    SetMachine(nullptr);
}

void SidScopePane::SetMachine(MachineDescriptor* desc) {
    m_chips.clear();
    m_chip->Clear();
    for (IAudioInspect* insp : scope::findAudioInspect(desc))
        for (int c = 0; c < insp->chipCount(); ++c) {
            m_chips.push_back({insp, c});
            m_chip->Append(wxString::FromUTF8(insp->chipName(c)));
        }
    if (m_chips.empty()) m_chip->Append("(no sound chip)");
    m_chip->SetSelection(0);
    m_chip->Enable(m_chips.size() > 1);
    m_lastCycles = UINT64_MAX;
    Layout();
    m_canvas->Refresh(false);
}

void SidScopePane::RefreshValues(uint64_t cycles) {
    if (!IsShownOnScreen() || cycles == m_lastCycles) return;   // nothing new to show
    m_lastCycles = cycles;
    m_canvas->Refresh(false);
}

void SidScopePane::OnSettingChanged() {
    m_canvas->Refresh(false);
}

void SidScopePane::Paint(wxDC& dc, const wxSize& size) {
    dc.SetBackground(wxBrush(BG));
    dc.Clear();
    dc.SetFont(GetFont().Smaller());
    dc.SetTextForeground(TEXT);

    int sel = m_chip->GetSelection();
    if (m_chips.empty() || sel < 0 || sel >= (int)m_chips.size()) {
        dc.SetTextForeground(DIM);
        dc.DrawText("No sound chip with a scope on this machine.", 8, 8);
        return;
    }
    const Chip& chip = m_chips[sel];
    int voices = chip.insp->voiceCount(chip.index);
    int rows = voices + 1;
    int rowH = size.y / rows;
    for (int ch = 0; ch < rows; ++ch)
        PaintRow(dc, wxRect(0, ch * rowH, size.x, rowH), ch, ch == voices);
}

void SidScopePane::PaintRow(wxDC& dc, const wxRect& r, int channel, bool output) {
    const Chip& chip = m_chips[m_chip->GetSelection()];
    const int lineH = dc.GetCharHeight();
    const wxColour& colour = TRACE[std::min(channel, 3)];

    // Separator
    dc.SetPen(wxPen(GRID));
    if (r.y > 0) dc.DrawLine(r.x, r.y, r.GetRight() + 1, r.y);

    // Text header: one line for the output, two for a voice
    IAudioInspect::VoiceInfo vi;
    int headerH = lineH + 4;
    dc.SetTextForeground(colour);
    if (output) {
        dc.DrawText("Output", r.x + 6, r.y + 2);
    } else {
        chip.insp->voiceInfo(chip.index, channel, vi);
        headerH = 2 * lineH + 4;
        dc.DrawText(wxString::Format("Voice %d", channel + 1), r.x + 6, r.y + 2);
        int x = r.x + 6 + dc.GetTextExtent("Voice 9 ").x + 6;
        dc.SetTextForeground(TEXT);
        wxString line1 = wxString::FromUTF8(scope::controlText(vi.control));
        if (vi.freq) {
            int cents = 0;
            std::string note = scope::noteName(vi.freqHz, &cents);
            line1 += wxString::Format("   %.1f Hz", vi.freqHz);
            if (!note.empty()) line1 += wxString::Format("  %s %+dc", note, cents);
        }
        dc.DrawText(line1, x, r.y + 2);

        wxString line2 = wxString::Format("PW %.1f%%   ADSR %X %X %X %X   %s %d",
                                          vi.pulseWidth * 100.0 / 4096.0, vi.attack, vi.decay,
                                          vi.sustain, vi.release, phaseName(vi.envPhase), vi.envLevel);
        if (vi.filtered) line2 += "   Filter";
        if (vi.muted) line2 += "   Off";
        dc.DrawText(line2, x, r.y + 2 + lineH);
    }

    // Trace area (voices leave room for the envelope diagram on the right)
    const int envW = output ? 0 : std::min(90, r.width / 4);
    wxRect tr(r.x + 6, r.y + headerH, r.width - 12 - (envW ? envW + 8 : 0), r.height - headerH - 4);
    if (tr.width < 20 || tr.height < 10) return;
    if (envW) PaintEnvelope(dc, wxRect(tr.GetRight() + 9, tr.y, envW, tr.height), vi, colour);

    const int midY = tr.y + tr.height / 2;
    const double halfH = tr.height / 2.0 - 1;
    dc.SetPen(wxPen(GRID));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRectangle(tr);
    dc.DrawLine(tr.x, midY, tr.GetRight(), midY);
    for (int i = 1; i < 10; ++i) {   // 10 time divisions
        int gx = tr.x + tr.width * i / 10;
        dc.DrawLine(gx, midY - 3, gx, midY + 4);
    }

    // Samples
    int cap = chip.insp->scopeCapacity();
    if ((int)m_buf.size() < cap) m_buf.resize(cap);
    int n = chip.insp->scopeSamples(chip.index, channel, m_buf.data(), cap);
    int rate = chip.insp->scopeSampleRate();
    int window = std::min(n, std::max(2, rate * TIMEBASES_MS[m_timebase->GetSelection()] / 1000));
    if (n < 2 || window < 2) return;

    int start = n - window;
    if (m_trigger->GetSelection() == 0) {
        int t = scope::findTrigger(m_buf.data(), n, window);
        bool silent = std::all_of(m_buf.begin() + start, m_buf.begin() + n,
                                  [](float v) { return std::fabs(v) < 0.01f; });
        if (t >= 0) start = t;
        else if (!silent) {   // a flat trace needs no warning
            dc.SetTextForeground(DIM);
            wxString msg = "no trigger";
            dc.DrawText(msg, tr.GetRight() - dc.GetTextExtent(msg).x - 3, tr.y + 2);
        }
    }

    auto yOf = [&](float v) {
        v = std::max(-1.0f, std::min(1.0f, v));
        return midY - (int)std::lround(v * halfH);
    };
    const float* s = m_buf.data() + start;
    dc.SetPen(wxPen(colour));
    if (window <= tr.width) {
        std::vector<wxPoint> pts(window);
        for (int i = 0; i < window; ++i)
            pts[i] = wxPoint(tr.x + (int)((int64_t)i * (tr.width - 1) / (window - 1)), yOf(s[i]));
        dc.DrawLines((int)pts.size(), pts.data());
    } else {
        // More samples than pixels: one vertical span per column, joined to the previous column
        float prev = s[0];
        for (int x = 0; x < tr.width; ++x) {
            int a = (int)((int64_t)x * window / tr.width);
            int b = std::max(a + 1, (int)((int64_t)(x + 1) * window / tr.width));
            float lo = prev, hi = prev;
            for (int i = a; i < b; ++i) { lo = std::min(lo, s[i]); hi = std::max(hi, s[i]); }
            dc.DrawLine(tr.x + x, yOf(hi), tr.x + x, yOf(lo) + 1);
            prev = s[b - 1];
        }
    }
}

void SidScopePane::PaintEnvelope(wxDC& dc, const wxRect& r, const IAudioInspect::VoiceInfo& vi,
                                 const wxColour& colour) {
    // ADSR shape with segment widths on a log scale of their durations, the
    // current phase highlighted, and the envelope level as a bar on the right.
    const int barW = 6;
    wxRect shape(r.x, r.y + 2, r.width - barW - 4, r.height - 4);
    if (shape.width < 20 || shape.height < 8) return;

    double wa = std::log2(1.0 + scope::attackMs(vi.attack));
    double wd = std::log2(1.0 + scope::decayReleaseMs(vi.decay));
    double wr = std::log2(1.0 + scope::decayReleaseMs(vi.release));
    double ws = (wa + wd + wr) / 3.0;   // sustain has no duration: a fixed hold
    double total = wa + wd + ws + wr;
    auto xAt = [&](double t) { return shape.x + (int)std::lround(t / total * (shape.width - 1)); };
    int top = shape.y, bottom = shape.GetBottom();
    int susY = bottom - (int)std::lround(vi.sustain / 15.0 * (bottom - top));

    wxPoint p0(shape.x, bottom), p1(xAt(wa), top), p2(xAt(wa + wd), susY),
            p3(xAt(wa + wd + ws), susY), p4(shape.GetRight(), bottom);
    wxPoint seg[4][2] = {{p0, p1}, {p1, p2}, {p2, p3}, {p3, p4}};
    int active = -1;
    switch (vi.envPhase) {
        case IAudioInspect::EnvPhase::Attack:  active = 0; break;
        case IAudioInspect::EnvPhase::Decay:   active = 1; break;
        case IAudioInspect::EnvPhase::Sustain: active = 2; break;
        case IAudioInspect::EnvPhase::Release: active = 3; break;
        default: break;
    }
    for (int i = 0; i < 4; ++i) {
        dc.SetPen(i == active ? wxPen(colour, 2) : wxPen(DIM));
        dc.DrawLine(seg[i][0], seg[i][1]);
    }

    wxRect bar(r.GetRight() - barW + 1, shape.y, barW, shape.height);
    dc.SetPen(wxPen(GRID));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRectangle(bar);
    int fill = (int)std::lround(vi.envLevel / 255.0 * (bar.height - 2));
    if (fill > 0) {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(colour));
        dc.DrawRectangle(bar.x + 1, bar.GetBottom() - fill, bar.width - 2, fill);
    }
}
