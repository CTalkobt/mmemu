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

    m_nmiBtn = new wxButton(this, wxID_ANY, "Trigger NMI");
    m_nmiBtn->SetToolTip("Pulls the CPU's NMI line low while held. The PET had no RESTORE key; "
                         "this stands in for the NMI pushbutton owners often added, so programs "
                         "for a stock PET will not expect it.");
    m_nmiBtn->Hide();

    auto* right = new wxBoxSizer(wxVERTICAL);
    right->Add(m_captureBtn, 0, wxEXPAND | wxALL, 4);
    right->Add(m_nmiBtn,     0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

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
    // Held while the mouse button is down, so the CPU sees the NMI edge
    m_nmiBtn->Bind(wxEVT_LEFT_DOWN,    [this](wxMouseEvent& e) { setNmi(true);  e.Skip(); });
    m_nmiBtn->Bind(wxEVT_LEFT_UP,      [this](wxMouseEvent& e) { setNmi(false); e.Skip(); });
    m_nmiBtn->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) { setNmi(false); e.Skip(); });
    for (auto t : {wxEVT_MOTION, wxEVT_LEFT_DOWN, wxEVT_LEFT_UP, wxEVT_RIGHT_DOWN, wxEVT_RIGHT_UP,
                   wxEVT_MIDDLE_DOWN, wxEVT_MIDDLE_UP, wxEVT_MOUSEWHEEL, wxEVT_LEAVE_WINDOW})
        Bind(t, &ScreenPane::OnMouse, this);
}

void ScreenPane::OnCaptureToggle(wxCommandEvent&) {
    bool active = m_captureBtn->GetValue();
    m_captureBtn->SetLabel(active ? "Keyboard Captured \u25cf" : "Capture Keyboard");
    if (m_captureCallback) m_captureCallback(active);
}

void ScreenPane::setNmi(bool down) {
    if (down == m_nmiDown || !m_nmiHandler) return;
    m_nmiDown = down;
    m_nmiHandler(down);
}

void ScreenPane::SetNmiHandler(std::function<void(bool)> fn) {
    setNmi(false);
    m_nmiHandler = std::move(fn);
    m_nmiBtn->Show((bool)m_nmiHandler);
    Layout();
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

    m_video->renderFrame(m_pixelBuf.data());

    auto dim = m_video->getDimensions();
    int srcW = dim.width;
    int srcH = dim.height;

    auto* rgb = static_cast<unsigned char*>(malloc((size_t)srcW * srcH * 3));
    for (int i = 0; i < srcW * srcH; ++i) {
        uint32_t px  = m_pixelBuf[i];
        rgb[i*3]     = (px >> 16) & 0xFF;  // R
        rgb[i*3 + 1] = (px >>  8) & 0xFF;  // G
        rgb[i*3 + 2] =  px        & 0xFF;  // B
    }
    wxImage img(srcW, srcH, rgb);

    wxRect canvas = GetClientRect();
    // Below the controls (taller when the NMI button is shown)
    int btnH = m_ratioBtn->GetSize().y + 8;
    if (m_nmiBtn->IsShown()) btnH = std::max(btnH, m_nmiBtn->GetRect().GetBottom() + 5);
    canvas.y      += btnH;
    canvas.height -= btnH;
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
