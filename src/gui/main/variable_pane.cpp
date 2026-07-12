#include "variable_pane.h"
#include "libtoolchain/main/variable_symbol.h"
#include <sstream>
#include <iomanip>

VariablePane::VariablePane(wxWindow* parent, DebugContext* dbg)
    : wxPanel(parent), m_dbg(dbg) {
    setupUI();
}

void VariablePane::setupUI() {
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    // Function selection
    wxBoxSizer* selectSizer = new wxBoxSizer(wxHORIZONTAL);
    selectSizer->Add(new wxStaticText(this, wxID_ANY, "Function:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    m_functionChoice = new wxChoice(this, wxID_ANY);
    m_functionChoice->Append("(Global Variables)");
    selectSizer->Add(m_functionChoice, 1, wxEXPAND);
    sizer->Add(selectSizer, 0, wxEXPAND | wxALL, 5);

    // Variable list
    m_listCtrl = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
    m_listCtrl->InsertColumn(0, "Name", wxLIST_FORMAT_LEFT, 150);
    m_listCtrl->InsertColumn(1, "Address", wxLIST_FORMAT_CENTER, 80);
    m_listCtrl->InsertColumn(2, "Size", wxLIST_FORMAT_CENTER, 50);
    m_listCtrl->InsertColumn(3, "Type", wxLIST_FORMAT_LEFT, 100);
    m_listCtrl->InsertColumn(4, "Value", wxLIST_FORMAT_LEFT, 100);
    sizer->Add(m_listCtrl, 1, wxEXPAND | wxALL, 5);

    SetSizer(sizer);

    Bind(wxEVT_CHOICE, &VariablePane::onFunctionSelected, this, m_functionChoice->GetId());
}

void VariablePane::onFunctionSelected(wxCommandEvent& event) {
    refresh();
}

void VariablePane::refresh() {
    m_listCtrl->DeleteAllItems();
    if (!m_dbg || !m_dbg->symbols()) return;

    int selection = m_functionChoice->GetSelection();

    if (selection == 0) {
        // Show global variables
        auto globals = m_dbg->symbols()->getGlobalVariables();
        for (size_t i = 0; i < globals.size(); ++i) {
            const auto* var = globals[i];
            m_listCtrl->InsertItem(i, var->displayName);

            std::ostringstream addr;
            addr << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << var->address;
            m_listCtrl->SetItem(i, 1, addr.str());

            m_listCtrl->SetItem(i, 2, std::to_string(var->size));
            m_listCtrl->SetItem(i, 3, formatVariableType(var->type));

            // Show value if memory is available
            if (m_dbg->bus()) {
                std::ostringstream val;
                val << std::hex << std::uppercase << std::setfill('0');
                switch (var->type) {
                    case VariableType::UINT8:
                    case VariableType::INT8:
                        val << std::setw(2) << (int)m_dbg->bus()->peek8(var->address);
                        break;
                    case VariableType::UINT16:
                    case VariableType::INT16:
                        val << std::setw(4) << (m_dbg->bus()->peek8(var->address) | (m_dbg->bus()->peek8(var->address + 1) << 8));
                        break;
                    default:
                        val << "(complex type)";
                }
                m_listCtrl->SetItem(i, 4, val.str());
            }
        }
    } else if (selection > 0) {
        // Show function variables - not fully implemented in this skeleton
        m_listCtrl->InsertItem(0, "Function-local variables");
        m_listCtrl->SetItem(0, 1, "(requires function name mapping)");
    }
}
