#pragma once

#include <wx/wx.h>
#include <wx/spinctrl.h>
#include <chrono>
#include <functional>
#include "libdebug/main/gfx_view.h"

class IBus;
class IVideoInspect;
class DebugContext;
struct MachineDescriptor;

/**
 * Sprite / character-set viewer (#28).
 *
 * Shows graphics at any address in a grid (hires, multicolour, VIC-IV 64-wide,
 * 16-colour, FCM and NCM formats), or follows a VIC sprite or the current
 * character set. Reads either through the CPU bus or as the video chip sees
 * memory. Hover shows an item's address, double-click opens it in the memory
 * pane, and the sheet can be exported to PNG.
 */
class GraphicsPane : public wxPanel {
public:
    explicit GraphicsPane(wxWindow* parent);

    /** Attach to a machine (nullptr detaches). */
    void SetMachine(MachineDescriptor* desc, IBus* cpuBus, DebugContext* dbg);
    /** Called from the GUI's update loop; redraws when live (throttled) or when settings changed. */
    void RefreshValues();
    /** Open an address in the memory pane (double-click / context menu). */
    void SetMemoryCallback(std::function<void(uint32_t)> cb) { m_onMemory = std::move(cb); }
    void SetStatusCallback(std::function<void(const wxString&)> cb) { m_onStatus = std::move(cb); }

private:
    class Canvas;
    enum Source { SRC_ADDRESS = 0, SRC_SPRITE0 = 1, SRC_CHARSET = 9 };

    void Rebuild();
    void OnSettingChanged();
    void OnSourceChanged();
    void OnAddressEnter();
    void OnExport();
    void OnCanvasMotion(int item);
    void OnCanvasClick(int item, bool dbl);
    void OnCanvasContext(int item, const wxPoint& pos);
    gfx::Format SelectedFormat() const;
    void SelectFormat(gfx::Format f);
    void UpdateStatus(int item);

    // Controls
    wxChoice*   m_source = nullptr;
    wxTextCtrl* m_addr = nullptr;
    wxChoice*   m_format = nullptr;
    wxSpinCtrl* m_count = nullptr;
    wxSpinCtrl* m_cols = nullptr;
    wxSpinCtrl* m_stride = nullptr;
    wxChoice*   m_zoom = nullptr;
    wxChoice*   m_memSel = nullptr;
    wxSpinCtrl* m_palBase = nullptr;
    wxCheckBox* m_swap = nullptr;
    wxCheckBox* m_live = nullptr;
    Canvas*     m_canvas = nullptr;
    wxStaticText* m_status = nullptr;

    // Machine
    IBus*          m_bus = nullptr;
    IVideoInspect* m_video = nullptr;
    DebugContext*  m_dbg = nullptr;

    // State
    uint32_t   m_start = 0;
    gfx::Sheet m_sheet;
    int        m_selected = -1;
    int        m_hover = -1;
    bool       m_dirty = true;
    std::chrono::steady_clock::time_point m_lastDraw{};

    std::function<void(uint32_t)> m_onMemory;
    std::function<void(const wxString&)> m_onStatus;
};
