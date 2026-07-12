#pragma once

#include <wx/wx.h>
#include <wx/listctrl.h>
#include "libdebug/main/debug_context.h"

class VariablePane : public wxPanel {
public:
    VariablePane(wxWindow* parent, DebugContext* dbg);
    void refresh();

private:
    DebugContext* m_dbg;
    wxListCtrl* m_listCtrl;
    wxChoice* m_functionChoice;

    void setupUI();
    void onFunctionSelected(wxCommandEvent& event);
};
