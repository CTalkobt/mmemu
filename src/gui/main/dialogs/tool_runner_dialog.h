#pragma once
#include <wx/wx.h>
#include <wx/listctrl.h>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <cstdint>

class DebugContext;
class ICore;
class IBus;
struct MachineDescriptor;

/**
 * ToolRunnerDialog — generic GUI dialog for MCP-style tools.
 *
 * Auto-generates input controls based on a field definition list,
 * runs a callback to execute the tool, and renders results as either
 * plain text or a sortable table.
 */

struct ToolField {
    std::string name;        // internal key
    std::string label;       // display label
    std::string type;        // "integer", "string", "boolean", "expression", "address"
    std::string defaultVal;  // default value as string
    std::string tooltip;     // description
};

struct ToolResultColumn {
    std::string key;         // JSON-like key in result rows
    std::string header;      // column header
    int width;               // pixel width
    bool rightAlign;
};

struct ToolResultRow {
    std::map<std::string, std::string> values;
};

struct ToolResult {
    std::string summary;                    // text shown above table
    std::vector<ToolResultColumn> columns;  // table columns (empty = text-only)
    std::vector<ToolResultRow> rows;        // table rows
};

using ToolCallback = std::function<ToolResult(const std::map<std::string, std::string>& args)>;

class ToolRunnerDialog : public wxDialog {
public:
    ToolRunnerDialog(wxWindow* parent,
                     const std::string& title,
                     const std::vector<ToolField>& fields,
                     ToolCallback callback);

private:
    void OnRun(wxCommandEvent& event);
    void OnClose(wxCommandEvent& event);
    void PopulateTable(const ToolResult& result);

    std::vector<ToolField> m_fields;
    std::map<std::string, wxTextCtrl*> m_textInputs;
    std::map<std::string, wxCheckBox*> m_boolInputs;
    ToolCallback m_callback;

    wxTextCtrl*   m_summaryCtrl = nullptr;
    wxListCtrl*   m_tableCtrl   = nullptr;

    // Sort state
    int  m_sortColumn = -1;
    bool m_sortAscending = true;
    std::vector<ToolResultRow> m_currentRows;
    std::vector<ToolResultColumn> m_currentColumns;
    void OnColumnClick(wxListEvent& event);
    void RefreshTable();
};

/**
 * ExpressionTextCtrl — wxTextCtrl with register/symbol autocomplete popup.
 */
class ExpressionTextCtrl : public wxTextCtrl {
public:
    ExpressionTextCtrl(wxWindow* parent, wxWindowID id,
                       const wxString& value = "",
                       const wxPoint& pos = wxDefaultPosition,
                       const wxSize& size = wxDefaultSize);

    void SetDebugContext(DebugContext* dbg) { m_dbg = dbg; }

private:
    void OnChar(wxKeyEvent& event);
    void OnText(wxCommandEvent& event);
    void ShowCompletions(const std::string& prefix);
    void InsertCompletion(const std::string& text);

    DebugContext* m_dbg = nullptr;
    wxListBox*    m_popup = nullptr;
};
