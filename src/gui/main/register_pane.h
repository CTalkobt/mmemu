#pragma once
#include <wx/wx.h>
#include <wx/grid.h>
#include "libcore/main/icore.h"
#include <vector>
#include <map>

class RegisterPane : public wxPanel {
public:
    RegisterPane(wxWindow* parent);
    void SetCPU(ICore* cpu);
    void RefreshValues();

private:
    void rebuildAnnotations();

    ICore* m_cpu = nullptr;
    wxGrid* m_grid;
    wxStaticText* m_annotationLabel;
    std::vector<uint32_t> m_prevValues;
    wxFont m_fixedFont;
    bool m_is45GS02 = false;
};
