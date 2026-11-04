#pragma once
#include <wx/wx.h>
#include <wx/tglbtn.h>
#include "libdevices/main/ivideo_output.h"
#include "gui/main/ikeyboard_capture_pane.h"
#include <functional>
#include <vector>

/**
 * Panel that renders an IVideoOutput device (e.g. VIC-I) and scales it to fit.
 * Also implements IKeyboardCapturePane so the host can wire the "Capture
 * Keyboard" toggle button to the frame's key-routing logic.
 */
class ScreenPane : public wxPanel, public IKeyboardCapturePane {
public:
    explicit ScreenPane(wxWindow* parent);

    /** Set (or clear) the video device to render. */
    void SetVideoOutput(IVideoOutput* vid);

    /** Call once per timer tick to pull a fresh frame and queue a repaint. */
    void RefreshFrame();

    /**
     * Send the host mouse to the machine (MachineDescriptor::onMouse): moves
     * over the picture, in emulated pixels, with the buttons and wheel.
     * Empty to stop.
     */
    void SetMouseHandler(std::function<void(int dx, int dy, uint8_t buttons, int wheel)> fn);

    // IKeyboardCapturePane
    void SetCaptureCallback(std::function<void(bool)> fn) override { m_captureCallback = std::move(fn); }
    void SetCaptureActive(bool active) override;

private:
    void OnPaint(wxPaintEvent&);
    void OnSize(wxSizeEvent&);
    void OnCaptureToggle(wxCommandEvent&);
    void OnMouse(wxMouseEvent&);

    IVideoOutput*              m_video = nullptr;
    std::vector<uint32_t>      m_pixelBuf;
    wxToggleButton*            m_ratioBtn;
    wxToggleButton*            m_captureBtn;
    std::function<void(bool)>  m_captureCallback;

    std::function<void(int, int, uint8_t, int)> m_mouseHandler;
    wxRect   m_imgRect;               // where the picture was last drawn
    double   m_scale = 1.0;           // host pixels per emulated pixel
    wxPoint  m_lastPos;
    bool     m_haveLast = false;
    double   m_fracX = 0, m_fracY = 0;
    uint8_t  m_buttons = 0;
};
