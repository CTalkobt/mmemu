#include "trace_pane.h"
#include <iomanip>
#include <sstream>

static std::string toHex(uint32_t v, int width = 4) {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0') << std::setw(width) << v;
    return ss.str();
}

TracePane::TracePane(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                            wxLC_REPORT | wxLC_SINGLE_SEL);
    m_list->SetFont(wxFont(9, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));

    m_list->AppendColumn("#", wxLIST_FORMAT_RIGHT, 45);
    m_list->AppendColumn("PC", wxLIST_FORMAT_LEFT, 55);
    m_list->AppendColumn("Instruction", wxLIST_FORMAT_LEFT, 150);
    m_list->AppendColumn("A", wxLIST_FORMAT_LEFT, 35);
    m_list->AppendColumn("X", wxLIST_FORMAT_LEFT, 35);
    m_list->AppendColumn("Y", wxLIST_FORMAT_LEFT, 35);
    m_list->AppendColumn("SP", wxLIST_FORMAT_LEFT, 35);

    m_list->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, &TracePane::onRightClick, this);

    sizer->Add(m_list, 1, wxEXPAND);
    SetSizer(sizer);
}

static std::string regVal(const std::map<std::string, uint32_t>& regs, const char* name) {
    auto it = regs.find(name);
    if (it == regs.end()) return "--";
    return "$" + toHex(it->second, 2);
}

void TracePane::RefreshValues() {
    m_list->Freeze();
    m_list->DeleteAllItems();

    if (!m_dbg) {
        m_list->Thaw();
        return;
    }

    auto& tb = m_dbg->trace();
    long count = (long)tb.size();

    for (long i = 0; i < count; ++i) {
        const auto& entry = tb.at(i);
        long idx = m_list->InsertItem(i, wxString::Format("%ld", i + 1));
        m_list->SetItem(idx, 1, "$" + toHex(entry.addr));
        m_list->SetItem(idx, 2, entry.mnemonic);
        m_list->SetItem(idx, 3, regVal(entry.regs, "A"));
        m_list->SetItem(idx, 4, regVal(entry.regs, "X"));
        m_list->SetItem(idx, 5, regVal(entry.regs, "Y"));
        m_list->SetItem(idx, 6, regVal(entry.regs, "SP"));
    }

    // Scroll to bottom
    if (count > 0)
        m_list->EnsureVisible(count - 1);

    m_list->Thaw();
}

void TracePane::onRightClick(wxListEvent& evt) {
    m_selectedIndex = evt.GetIndex();
    if (m_selectedIndex < 0) return;

    wxMenu menu;
    menu.Append(ID_REWIND_TO_HERE, "Rewind to here");
    menu.Bind(wxEVT_MENU, &TracePane::onRewindToHere, this, ID_REWIND_TO_HERE);
    PopupMenu(&menu);
}

void TracePane::onRewindToHere(wxCommandEvent&) {
    if (!m_dbg || !m_cpu || m_selectedIndex < 0) return;

    auto& tb = m_dbg->trace();
    if (m_selectedIndex >= (int)tb.size()) return;

    // Reverse-step until the trace buffer shrinks to the selected index
    int stepsToUndo = (int)tb.size() - m_selectedIndex;
    int reversed = 0;
    for (int i = 0; i < stepsToUndo; ++i) {
        if (!m_dbg->reverseStep()) break;
        ++reversed;
    }

    RefreshValues();

    // Update status bar
    wxWindow* frame = GetParent();
    while (frame && !frame->IsTopLevel()) frame = frame->GetParent();
    if (auto* f = dynamic_cast<wxFrame*>(frame)) {
        f->SetStatusText(wxString::Format("Rewound %d instructions to $%s",
                                           reversed, toHex(m_cpu->pc()).c_str()));
    }
}
