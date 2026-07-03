#include "tool_runner_dialog.h"
#include "libdebug/main/debug_context.h"
#include "libcore/main/icore.h"
#include <algorithm>
#include <sstream>

// ---------------------------------------------------------------------------
// ToolRunnerDialog
// ---------------------------------------------------------------------------

ToolRunnerDialog::ToolRunnerDialog(wxWindow* parent,
                                   const std::string& title,
                                   const std::vector<ToolField>& fields,
                                   ToolCallback callback)
    : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxSize(700, 500),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_fields(fields), m_callback(std::move(callback))
{
    auto* mainSizer = new wxBoxSizer(wxVERTICAL);

    // Input fields
    auto* inputSizer = new wxFlexGridSizer(2, 5, 10);
    inputSizer->AddGrowableCol(1, 1);

    for (const auto& f : m_fields) {
        auto* label = new wxStaticText(this, wxID_ANY, f.label + ":");
        inputSizer->Add(label, 0, wxALIGN_CENTER_VERTICAL);

        if (f.type == "boolean") {
            auto* cb = new wxCheckBox(this, wxID_ANY, "");
            cb->SetValue(f.defaultVal == "true" || f.defaultVal == "1");
            if (!f.tooltip.empty()) cb->SetToolTip(f.tooltip);
            m_boolInputs[f.name] = cb;
            inputSizer->Add(cb, 1, wxEXPAND);
        } else {
            auto* tc = new wxTextCtrl(this, wxID_ANY, f.defaultVal);
            if (!f.tooltip.empty()) tc->SetToolTip(f.tooltip);
            m_textInputs[f.name] = tc;
            inputSizer->Add(tc, 1, wxEXPAND);
        }
    }
    mainSizer->Add(inputSizer, 0, wxEXPAND | wxALL, 10);

    // Buttons
    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* runBtn = new wxButton(this, wxID_OK, "Run");
    auto* closeBtn = new wxButton(this, wxID_CANCEL, "Close");
    btnSizer->Add(runBtn, 0, wxRIGHT, 5);
    btnSizer->Add(closeBtn);
    mainSizer->Add(btnSizer, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    // Summary text
    m_summaryCtrl = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition,
                                   wxSize(-1, 60), wxTE_MULTILINE | wxTE_READONLY);
    m_summaryCtrl->SetFont(wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE)));
    mainSizer->Add(m_summaryCtrl, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    // Table
    m_tableCtrl = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxLC_REPORT | wxLC_SINGLE_SEL);
    m_tableCtrl->SetFont(wxFont(wxFontInfo(9).Family(wxFONTFAMILY_TELETYPE)));
    mainSizer->Add(m_tableCtrl, 1, wxEXPAND | wxALL, 10);

    SetSizer(mainSizer);

    Bind(wxEVT_BUTTON, &ToolRunnerDialog::OnRun, this, wxID_OK);
    Bind(wxEVT_BUTTON, &ToolRunnerDialog::OnClose, this, wxID_CANCEL);
    m_tableCtrl->Bind(wxEVT_LIST_COL_CLICK, &ToolRunnerDialog::OnColumnClick, this);

    runBtn->SetDefault();
}

void ToolRunnerDialog::OnRun(wxCommandEvent&) {
    std::map<std::string, std::string> args;
    for (const auto& f : m_fields) {
        if (f.type == "boolean") {
            args[f.name] = m_boolInputs[f.name]->GetValue() ? "1" : "0";
        } else {
            args[f.name] = m_textInputs[f.name]->GetValue().ToStdString();
        }
    }

    ToolResult result = m_callback(args);
    m_summaryCtrl->SetValue(result.summary);

    if (!result.columns.empty()) {
        m_currentColumns = result.columns;
        m_currentRows = result.rows;
        m_sortColumn = -1;
        PopulateTable(result);
    } else {
        m_tableCtrl->ClearAll();
    }
}

void ToolRunnerDialog::PopulateTable(const ToolResult& result) {
    m_tableCtrl->ClearAll();

    for (int i = 0; i < (int)result.columns.size(); ++i) {
        const auto& col = result.columns[i];
        int fmt = col.rightAlign ? wxLIST_FORMAT_RIGHT : wxLIST_FORMAT_LEFT;
        m_tableCtrl->InsertColumn(i, col.header, fmt, col.width);
    }

    for (int row = 0; row < (int)result.rows.size(); ++row) {
        const auto& r = result.rows[row];
        long idx = m_tableCtrl->InsertItem(row, "");
        for (int col = 0; col < (int)result.columns.size(); ++col) {
            auto valIt = r.values.find(result.columns[col].key);
            if (valIt != r.values.end())
                m_tableCtrl->SetItem(idx, col, valIt->second);
        }
    }
}

void ToolRunnerDialog::OnColumnClick(wxListEvent& event) {
    int col = event.GetColumn();
    if (col == m_sortColumn)
        m_sortAscending = !m_sortAscending;
    else {
        m_sortColumn = col;
        m_sortAscending = true;
    }
    RefreshTable();
}

void ToolRunnerDialog::RefreshTable() {
    if (m_sortColumn < 0 || m_sortColumn >= (int)m_currentColumns.size()) return;

    std::string key = m_currentColumns[m_sortColumn].key;
    bool asc = m_sortAscending;
    bool numeric = m_currentColumns[m_sortColumn].rightAlign;

    std::sort(m_currentRows.begin(), m_currentRows.end(),
        [&](const ToolResultRow& a, const ToolResultRow& b) {
            auto itA = a.values.find(key), itB = b.values.find(key);
            std::string sa = (itA != a.values.end()) ? itA->second : "";
            std::string sb = (itB != b.values.end()) ? itB->second : "";
            if (numeric) {
                double da = 0, db = 0;
                try { da = std::stod(sa); } catch (...) {}
                try { db = std::stod(sb); } catch (...) {}
                return asc ? da < db : da > db;
            }
            return asc ? sa < sb : sa > sb;
        });

    ToolResult sorted;
    sorted.columns = m_currentColumns;
    sorted.rows = m_currentRows;
    PopulateTable(sorted);
}

void ToolRunnerDialog::OnClose(wxCommandEvent&) {
    EndModal(wxID_CANCEL);
}

// ---------------------------------------------------------------------------
// ExpressionTextCtrl — autocomplete for register/symbol names
// ---------------------------------------------------------------------------

ExpressionTextCtrl::ExpressionTextCtrl(wxWindow* parent, wxWindowID id,
                                       const wxString& value,
                                       const wxPoint& pos, const wxSize& size)
    : wxTextCtrl(parent, id, value, pos, size)
{
    Bind(wxEVT_CHAR, &ExpressionTextCtrl::OnChar, this);
}

void ExpressionTextCtrl::OnChar(wxKeyEvent& event) {
    if (m_popup && m_popup->IsShown()) {
        if (event.GetKeyCode() == WXK_DOWN) {
            int sel = m_popup->GetSelection();
            if (sel < (int)m_popup->GetCount() - 1)
                m_popup->SetSelection(sel + 1);
            return;
        }
        if (event.GetKeyCode() == WXK_UP) {
            int sel = m_popup->GetSelection();
            if (sel > 0) m_popup->SetSelection(sel - 1);
            return;
        }
        if (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_TAB) {
            int sel = m_popup->GetSelection();
            if (sel >= 0) {
                InsertCompletion(m_popup->GetString(sel).ToStdString());
            }
            m_popup->Hide();
            return;
        }
        if (event.GetKeyCode() == WXK_ESCAPE) {
            m_popup->Hide();
            return;
        }
    }

    event.Skip();

    // After the character is processed, check for completion trigger
    CallAfter([this]() {
        long ip = GetInsertionPoint();
        std::string text = GetValue().ToStdString();
        if (ip == 0 || text.empty()) {
            if (m_popup) m_popup->Hide();
            return;
        }

        // Find the word being typed (letters, digits, @, .)
        int start = (int)ip - 1;
        while (start > 0 && (std::isalnum(text[start-1]) || text[start-1] == '@'
               || text[start-1] == '.' || text[start-1] == '_'))
            start--;

        std::string prefix = text.substr(start, ip - start);
        if (prefix.length() >= 1) {
            ShowCompletions(prefix);
        } else if (m_popup) {
            m_popup->Hide();
        }
    });
}

void ExpressionTextCtrl::OnText(wxCommandEvent& event) {
    event.Skip();
}

void ExpressionTextCtrl::ShowCompletions(const std::string& prefix) {
    std::vector<std::string> matches;
    std::string upperPrefix = prefix;
    std::transform(upperPrefix.begin(), upperPrefix.end(), upperPrefix.begin(), ::toupper);

    // Register names
    if (m_dbg && m_dbg->cpu()) {
        ICore* cpu = m_dbg->cpu();
        for (int i = 0; i < cpu->regCount(); ++i) {
            const auto* d = cpu->regDescriptor(i);
            if (d->flags & 0x02) continue; // REGFLAG_INTERNAL
            std::string name = d->name;
            std::string upperName = name;
            std::transform(upperName.begin(), upperName.end(), upperName.begin(), ::toupper);
            if (upperName.find(upperPrefix) == 0)
                matches.push_back(name);

            // Status flags: .N, .Z, .C, etc.
            if (prefix[0] == '.' && (d->flags & 0x04) && d->flagNames) {
                int nbits = (d->width == RegWidth::R8) ? 8 : 16;
                for (int b = 0; b < nbits; ++b) {
                    if (d->flagNames[b] && d->flagNames[b] != '-') {
                        std::string flag = std::string(".") + d->flagNames[b];
                        std::string upperFlag = flag;
                        std::transform(upperFlag.begin(), upperFlag.end(), upperFlag.begin(), ::toupper);
                        if (upperFlag.find(upperPrefix) == 0 || prefix.length() == 1)
                            matches.push_back(flag);
                    }
                }
            }
        }
    }

    // Symbol names (symbols() returns map<uint32_t, string>)
    if (m_dbg && prefix[0] != '.' && prefix[0] != '@') {
        auto& syms = m_dbg->symbols();
        for (const auto& [addr, name] : syms.symbols()) {
            std::string upperName = name;
            std::transform(upperName.begin(), upperName.end(), upperName.begin(), ::toupper);
            if (upperName.find(upperPrefix) == 0)
                matches.push_back(name);
            if (matches.size() > 20) break;
        }
    }

    // Syntax hints for operators
    if (prefix == "*") {
        matches.push_back("*$addr   (byte deref)");
        matches.push_back("*$addr:16 (word deref)");
    }

    if (matches.empty()) {
        if (m_popup) m_popup->Hide();
        return;
    }

    // Sort and deduplicate
    std::sort(matches.begin(), matches.end());
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());

    // Create or reuse popup
    if (!m_popup) {
        m_popup = new wxListBox(GetParent(), wxID_ANY, wxDefaultPosition,
                                wxSize(200, 150), 0, nullptr,
                                wxLB_SINGLE | wxBORDER_SIMPLE);
        m_popup->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) {
            int sel = m_popup->GetSelection();
            if (sel >= 0) {
                InsertCompletion(m_popup->GetString(sel).ToStdString());
                m_popup->Hide();
            }
        });
    }

    m_popup->Clear();
    for (const auto& m : matches)
        m_popup->Append(m);
    if (!matches.empty()) m_popup->SetSelection(0);

    // Position below this text control
    wxPoint pos = GetScreenPosition();
    pos.y += GetSize().GetHeight();
    m_popup->SetPosition(GetParent()->ScreenToClient(pos));
    m_popup->Show();
    m_popup->Raise();
}

void ExpressionTextCtrl::InsertCompletion(const std::string& text) {
    // Strip any description suffix (e.g., "  (byte deref)")
    std::string completion = text;
    auto spacePos = completion.find("   ");
    if (spacePos != std::string::npos)
        completion = completion.substr(0, spacePos);

    long ip = GetInsertionPoint();
    std::string current = GetValue().ToStdString();

    // Find start of the word being completed
    int start = (int)ip - 1;
    while (start > 0 && (std::isalnum(current[start-1]) || current[start-1] == '@'
           || current[start-1] == '.' || current[start-1] == '_'))
        start--;

    // Replace the prefix with the completion
    std::string newText = current.substr(0, start) + completion + current.substr(ip);
    SetValue(newText);
    SetInsertionPoint(start + completion.length());
}
