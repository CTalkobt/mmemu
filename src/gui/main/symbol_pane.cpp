#include "symbol_pane.h"
#include "libdebug/main/expression_evaluator.h"
#include <wx/filedlg.h>
#include <sstream>
#include <iomanip>
#include <algorithm>

SymbolPane::SymbolPane(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // Toolbar
    auto* toolbar = new wxBoxSizer(wxHORIZONTAL);
    
    auto* addBtn = new wxButton(this, wxID_ANY, "Add...");
    auto* delBtn = new wxButton(this, wxID_ANY, "Delete");
    auto* clearBtn = new wxButton(this, wxID_ANY, "Clear");
    auto* loadBtn = new wxButton(this, wxID_ANY, "Load...");
    
    toolbar->Add(addBtn, 0, wxALL, 2);
    toolbar->Add(delBtn, 0, wxALL, 2);
    toolbar->Add(clearBtn, 0, wxALL, 2);
    toolbar->Add(loadBtn, 0, wxALL, 2);
    
    toolbar->AddStretchSpacer();
    toolbar->Add(new wxStaticText(this, wxID_ANY, "Search:"), 0, wxALIGN_CENTER_VERTICAL | wxALL, 2);
    m_searchCtrl = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(150, -1));
    toolbar->Add(m_searchCtrl, 0, wxALL, 2);

    sizer->Add(toolbar, 0, wxEXPAND | wxALL, 2);

    // List
    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                            wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SUNKEN);
    m_list->InsertColumn(0, "Address", wxLIST_FORMAT_LEFT, 80);
    m_list->InsertColumn(1, "Label",   wxLIST_FORMAT_LEFT, 200);
    sizer->Add(m_list, 1, wxEXPAND | wxALL, 2);

    SetSizer(sizer);

    addBtn->Bind(wxEVT_BUTTON, &SymbolPane::OnAdd, this);
    delBtn->Bind(wxEVT_BUTTON, &SymbolPane::OnDelete, this);
    clearBtn->Bind(wxEVT_BUTTON, &SymbolPane::OnClear, this);
    loadBtn->Bind(wxEVT_BUTTON, &SymbolPane::OnLoad, this);
    m_searchCtrl->Bind(wxEVT_TEXT, &SymbolPane::OnSearch, this);
    m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, &SymbolPane::OnItemActivated, this);
}

void SymbolPane::SetDebugContext(DebugContext* dbg) {
    m_dbg = dbg;
    RefreshValues();
}

void SymbolPane::SetGotoCallback(std::function<void(uint32_t)> cb) {
    m_gotoCallback = std::move(cb);
}

void SymbolPane::RefreshValues() {
    m_list->DeleteAllItems();
    if (!m_dbg) return;

    std::string search = m_searchCtrl->GetValue().ToStdString();
    std::transform(search.begin(), search.end(), search.begin(), ::tolower);

    auto syms = m_dbg->symbols().symbols();
    int row = 0;
    for (const auto& pair : syms) {
        std::string label = pair.second;
        std::string labelLower = label;
        std::transform(labelLower.begin(), labelLower.end(), labelLower.begin(), ::tolower);

        if (!search.empty() && labelLower.find(search) == std::string::npos) {
            continue;
        }

        std::ostringstream ss;
        int w = (pair.first > 0xFFFF) ? 7 : 4;
        ss << "$" << std::hex << std::uppercase << std::setfill('0') << std::setw(w) << pair.first;
        
        m_list->InsertItem(row, ss.str());
        m_list->SetItem(row, 1, label);
        m_list->SetItemData(row, (long)pair.first);
        row++;
    }
}

void SymbolPane::OnAdd(wxCommandEvent&) {
    if (!m_dbg) return;
    
    wxString label = wxGetTextFromUser("Enter symbol label:", "Add Symbol");
    if (label.IsEmpty()) return;
    
    wxString expr = wxGetTextFromUser("Enter address expression:", "Add Symbol");
    if (expr.IsEmpty()) return;
    
    uint32_t addr;
    if (ExpressionEvaluator::evaluate(expr.ToStdString(), m_dbg, addr)) {
        m_dbg->symbols().addSymbol(addr, label.ToStdString());
        RefreshValues();
    } else {
        wxMessageBox("Invalid address expression!", "Error", wxOK | wxICON_ERROR);
    }
}

void SymbolPane::OnDelete(wxCommandEvent&) {
    if (!m_dbg) return;
    long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel == -1) return;
    
    std::string label = m_list->GetItemText(sel, 1).ToStdString();
    m_dbg->symbols().removeSymbol(label);
    RefreshValues();
}

void SymbolPane::OnClear(wxCommandEvent&) {
    if (!m_dbg) return;
    if (wxMessageBox("Clear all symbols?", "Confirm", wxYES_NO | wxICON_QUESTION) == wxYES) {
        m_dbg->symbols().clear();
        RefreshValues();
    }
}

void SymbolPane::OnLoad(wxCommandEvent&) {
    if (!m_dbg) return;
    wxFileDialog openFileDialog(this, "Open Symbol File", "", "",
                               "Symbol files (*.sym)|*.sym|All files (*.*)|*.*",
                               wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (openFileDialog.ShowModal() == wxID_OK) {
        if (m_dbg->symbols().loadSym(openFileDialog.GetPath().ToStdString())) {
            RefreshValues();
        } else {
            wxMessageBox("Failed to load symbols.", "Error", wxOK | wxICON_ERROR);
        }
    }
}

void SymbolPane::OnSearch(wxCommandEvent&) {
    RefreshValues();
}

void SymbolPane::OnItemActivated(wxListEvent& event) {
    if (m_gotoCallback) {
        uint32_t addr = (uint32_t)event.GetData();
        m_gotoCallback(addr);
    }
}
