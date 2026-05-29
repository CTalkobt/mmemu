#pragma once

#include <wx/wx.h>
#include <wx/listctrl.h>
#include "libdebug/main/debug_context.h"

/**
 * A pane that displays the instruction trace buffer with a right-click
 * "Rewind to here" context menu for time-travel debugging.
 */
class TracePane : public wxPanel {
public:
    TracePane(wxWindow* parent);

    void SetDebugContext(DebugContext* dbg) { m_dbg = dbg; }
    void SetCPU(ICore* cpu) { m_cpu = cpu; }
    void RefreshValues();

private:
    void onRightClick(wxListEvent& evt);
    void onRewindToHere(wxCommandEvent& evt);

    wxListCtrl* m_list;
    DebugContext* m_dbg = nullptr;
    ICore* m_cpu = nullptr;
    int m_selectedIndex = -1;

    enum { ID_REWIND_TO_HERE = wxID_HIGHEST + 500 };
};
