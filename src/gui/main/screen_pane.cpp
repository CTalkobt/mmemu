#include "screen_pane.h"
#include <wx/dcbuffer.h>
#include <wx/settings.h>
#include <algorithm>
#include <cstdlib>

ScreenPane::ScreenPane(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
              wxFULL_REPAINT_ON_RESIZE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);

    m_ratioBtn   = new wxToggleButton(this, wxID_ANY, "Lock Ratio");
    m_captureBtn = new wxToggleButton(this, wxID_ANY, "Capture Keyboard");
    m_ratioBtn->SetValue(true);

    // Capture Keyboard, then the machine's key buttons below it
    auto* right = new wxBoxSizer(wxVERTICAL);
    right->Add(m_captureBtn, 0, wxEXPAND | wxALL, 4);
    m_rightSizer = right;

    auto* toolbar = new wxBoxSizer(wxHORIZONTAL);
    toolbar->Add(m_ratioBtn,   0, wxALL, 4);
    toolbar->AddStretchSpacer();
    toolbar->Add(right, 0);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(toolbar, 0, wxEXPAND);
    SetSizer(sizer);

    Bind(wxEVT_PAINT, &ScreenPane::OnPaint,         this);
    Bind(wxEVT_SIZE,  &ScreenPane::OnSize,           this);
    m_ratioBtn->Bind(wxEVT_TOGGLEBUTTON,   [this](wxCommandEvent&) { Refresh(); });
    m_captureBtn->Bind(wxEVT_TOGGLEBUTTON, &ScreenPane::OnCaptureToggle, this);
    m_releaseTimer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { releaseDue(); }, m_releaseTimer.GetId());
    for (auto t : {wxEVT_MOTION, wxEVT_LEFT_DOWN, wxEVT_LEFT_UP, wxEVT_RIGHT_DOWN, wxEVT_RIGHT_UP,
                   wxEVT_MIDDLE_DOWN, wxEVT_MIDDLE_UP, wxEVT_MOUSEWHEEL, wxEVT_LEAVE_WINDOW})
        Bind(t, &ScreenPane::OnMouse, this);
}

void ScreenPane::OnCaptureToggle(wxCommandEvent&) {
    bool active = m_captureBtn->GetValue();
    m_captureBtn->SetLabel(active ? "Keyboard Captured \u25cf" : "Capture Keyboard");
    if (m_captureCallback) m_captureCallback(active);
}

void ScreenPane::buttonDown(LiveButton& b) {
    b.releasePending = false;
    if (b.down) return;
    b.down = true;
    b.since = std::chrono::steady_clock::now();
    b.press(true);
}

void ScreenPane::buttonUp(LiveButton& b) {
    if (!b.down) return;
    // A quick click would press and release between two emulation slices,
    // so the machine would not see it: hold for at least MIN_HOLD_MS
    auto held = std::chrono::steady_clock::now() - b.since;
    if (held < std::chrono::milliseconds(MIN_HOLD_MS)) {
        b.releasePending = true;
        if (!m_releaseTimer.IsRunning()) m_releaseTimer.Start(20);
        return;
    }
    b.down = false;
    b.press(false);
}

void ScreenPane::releaseDue() {
    bool waiting = false;
    for (auto& b : m_keyButtons) {
        if (!b->releasePending) continue;
        auto held = std::chrono::steady_clock::now() - b->since;
        if (held >= std::chrono::milliseconds(MIN_HOLD_MS)) {
            b->releasePending = false;
            b->down = false;
            b->press(false);
        } else {
            waiting = true;
        }
    }
    if (!waiting) m_releaseTimer.Stop();
}

void ScreenPane::SetKeyButtons(std::vector<KeyButton> buttons) {
    m_releaseTimer.Stop();
    for (auto& b : m_keyButtons) {
        if (b->down) b->press(false);
        m_rightSizer->Detach(b->btn);
        b->btn->Destroy();
    }
    m_keyButtons.clear();
    for (auto& kb : buttons) {
        auto live = std::make_unique<LiveButton>();
        live->btn = new wxButton(this, wxID_ANY, wxString::FromUTF8(kb.label));
        if (!kb.tooltip.empty()) live->btn->SetToolTip(wxString::FromUTF8(kb.tooltip));
        live->press = std::move(kb.press);
        LiveButton* lb = live.get();
        lb->btn->Bind(wxEVT_LEFT_DOWN,    [this, lb](wxMouseEvent& e) { buttonDown(*lb); e.Skip(); });
        lb->btn->Bind(wxEVT_LEFT_UP,      [this, lb](wxMouseEvent& e) { buttonUp(*lb);   e.Skip(); });
        lb->btn->Bind(wxEVT_LEAVE_WINDOW, [this, lb](wxMouseEvent& e) { buttonUp(*lb);   e.Skip(); });
        m_rightSizer->Add(lb->btn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
        m_keyButtons.push_back(std::move(live));
    }
    Layout();
    Refresh();
}

void ScreenPane::SetMouseHandler(std::function<void(int, int, uint8_t, int)> fn) {
    m_mouseHandler = std::move(fn);
    m_haveLast = false;
    m_buttons = 0;
    m_fracX = m_fracY = 0;
}

void ScreenPane::OnMouse(wxMouseEvent& ev) {
    ev.Skip();
    if (!m_mouseHandler || m_imgRect.IsEmpty()) return;
    wxPoint pos = ev.GetPosition();
    bool inside = m_imgRect.Contains(pos);
    uint8_t buttons = m_buttons;
    if (ev.LeftDown() && inside) buttons |= 1;
    if (ev.RightDown() && inside) buttons |= 2;
    if (ev.MiddleDown() && inside) buttons |= 4;
    if (ev.LeftUp()) buttons &= ~1;
    if (ev.RightUp()) buttons &= ~2;
    if (ev.MiddleUp()) buttons &= ~4;
    if (ev.Leaving()) { m_haveLast = false; buttons = 0; }

    // Movement over the picture (or while a button is held), in emulated pixels
    int dx = 0, dy = 0;
    if ((inside || buttons) && (ev.Dragging() || ev.Moving() || ev.ButtonDown())) {
        if (m_haveLast) {
            m_fracX += (pos.x - m_lastPos.x) / m_scale;
            m_fracY += (pos.y - m_lastPos.y) / m_scale;
            dx = (int)m_fracX; dy = (int)m_fracY;
            m_fracX -= dx; m_fracY -= dy;
        }
        m_lastPos = pos;
        m_haveLast = true;
    }
    int wheel = 0;
    if (ev.GetEventType() == wxEVT_MOUSEWHEEL && inside && ev.GetWheelDelta())
        wheel = ev.GetWheelRotation() / ev.GetWheelDelta();

    if (dx || dy || wheel || buttons != m_buttons) {
        m_buttons = buttons;
        m_mouseHandler(dx, dy, buttons, wheel);
    }
    if (ev.ButtonDown() && inside) SetFocus();
}

void ScreenPane::SetCaptureActive(bool active) {
    m_captureBtn->SetValue(active);
    m_captureBtn->SetLabel(active ? "Keyboard Captured \u25cf" : "Capture Keyboard");
}

void ScreenPane::SetVideoOutput(IVideoOutput* vid) {
    m_video = vid;
    if (m_video) {
        auto dim = m_video->getDimensions();
        m_pixelBuf.assign((size_t)dim.width * dim.height, 0u);
    } else {
        m_pixelBuf.clear();
    }
    Refresh();
}

void ScreenPane::RefreshFrame() {
    if (m_video) Refresh();
}

void ScreenPane::OnSize(wxSizeEvent& event) {
    Refresh();
    event.Skip();
}

void ScreenPane::OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);

    wxColour bgCol = wxSystemSettings::GetColour(wxSYS_COLOUR_APPWORKSPACE);
    wxColour fgCol = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    dc.SetBackground(wxBrush(bgCol));
    dc.Clear();

    if (!m_video || m_pixelBuf.empty()) {
        dc.SetTextForeground(fgCol);
        dc.DrawText("No video output", 10, 40);
        return;
    }

    // The size can change from frame to frame (the SNES's hi-res and interlace)
    auto dim = m_video->getDimensions();
    int srcW = dim.width;
    int srcH = dim.height;
    if (m_pixelBuf.size() != (size_t)srcW * srcH) m_pixelBuf.assign((size_t)srcW * srcH, 0u);
    m_video->renderFrame(m_pixelBuf.data());

    auto* rgb = static_cast<unsigned char*>(malloc((size_t)srcW * srcH * 3));
    for (int i = 0; i < srcW * srcH; ++i) {
        uint32_t px  = m_pixelBuf[i];
        rgb[i*3]     = (px >> 16) & 0xFF;  // R
        rgb[i*3 + 1] = (px >>  8) & 0xFF;  // G
        rgb[i*3 + 2] =  px        & 0xFF;  // B
    }
    wxImage img(srcW, srcH, rgb);

    // Below the controls, or (with key buttons stacked under Capture
    // Keyboard) beside that column, whichever gives the larger picture
    const wxRect client = GetClientRect();
    const int rowH = m_ratioBtn->GetSize().y + 8;
    wxRect canvas = client;
    canvas.y += rowH;
    canvas.height -= rowH;
    if (!m_keyButtons.empty()) {
        const int colBottom = std::max(rowH, m_keyButtons.back()->btn->GetRect().GetBottom() + 5);
        int colLeft = client.GetRight();
        for (const auto& kb : m_keyButtons) colLeft = std::min(colLeft, kb->btn->GetRect().GetLeft());
        colLeft = std::min(colLeft, m_captureBtn->GetRect().GetLeft()) - 4;
        wxRect below = client, beside = client;
        below.y += colBottom;
        below.height -= colBottom;
        beside.y += rowH;
        beside.height -= rowH;
        beside.width = colLeft - beside.x;
        auto fit = [&](const wxRect& r) {
            return r.width <= 0 || r.height <= 0 ? 0.0 : std::min((double)r.width / srcW, (double)r.height / srcH);
        };
        canvas = fit(beside) > fit(below) ? beside : below;
    }
    if (canvas.width <= 0 || canvas.height <= 0) return;

    int dstX = canvas.x;
    int dstY = canvas.y;
    int dstW = canvas.width;
    int dstH = canvas.height;

    if (m_ratioBtn->GetValue()) {
        float scale = std::min((float)dstW / srcW, (float)dstH / srcH);
        int fitW = (int)(srcW * scale);
        int fitH = (int)(srcH * scale);
        dstX += (dstW - fitW) / 2;
        dstY += (dstH - fitH) / 2;
        dstW = fitW;
        dstH = fitH;
    }

    m_imgRect = wxRect(dstX, dstY, dstW, dstH);
    m_scale = std::max(0.01, (double)dstW / srcW);

    wxBitmap bmp(img.Scale(dstW, dstH, wxIMAGE_QUALITY_NEAREST));
    dc.DrawBitmap(bmp, dstX, dstY, false);
}
