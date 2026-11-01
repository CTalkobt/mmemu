#pragma once

#include <wx/wx.h>
#include <wx/aui/auibook.h>
#include <wx/stc/stc.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class DebugContext;

/**
 * Source editor pane (#147): one wxStyledTextCtrl tab per file.
 *
 * - Syntax highlighting by extension: 6502/45GS02 assembly, BASIC, C (cc45).
 * - Clicking the symbol margin toggles an execution breakpoint at the line's
 *   first mapped address (DebugContext source map); breakpoints show as red
 *   dots, the line being executed as an arrow with a highlighted background.
 * - Ctrl-S saves; unsaved tabs are marked with '*'.
 *
 * Files are identified by their absolute, normalised path, the same form the
 * source map stores (SourceMap::normalizePath).
 */
class SourcePane : public wxPanel {
public:
    explicit SourcePane(wxWindow* parent);

    void SetDebugContext(DebugContext* dbg);

    /** Open a file in a tab (or focus it if already open). */
    bool OpenFile(const std::string& path);
    /** Absolute path of the selected tab, or "" if none. */
    std::string CurrentFile() const;
    bool HasOpenFiles() const { return !m_docs.empty(); }

    /**
     * Mark the source line mapped to `pc`, opening its file if needed.
     * Returns true if the PC has a mapping (and the file could be shown).
     */
    bool ShowExecutionLine(uint32_t pc);
    void ClearExecutionLine();
    /**
     * Redraw breakpoint dots from the debugger's execution breakpoints. Cheap
     * when nothing changed (compares a signature), so it can run every tick.
     */
    void RefreshBreakpointMarkers(bool force = false);

    bool SaveCurrent();
    bool SaveAll();
    /** True if any open file has unsaved changes. */
    bool HasUnsavedChanges() const;

    /** Status messages (e.g. "No code at line 12"). */
    void SetStatusCallback(std::function<void(const wxString&)> cb) { m_status = std::move(cb); }
    /** Called after a margin click adds or removes a breakpoint. */
    void SetBreakpointsChangedCallback(std::function<void()> cb) { m_bpChanged = std::move(cb); }

private:
    struct Doc {
        std::string       path;
        wxStyledTextCtrl* stc = nullptr;
    };

    Doc* findDoc(const std::string& path);
    Doc* docForCtrl(wxObject* ctrl);
    void configureEditor(wxStyledTextCtrl* stc, const std::string& path);
    void updateTabTitle(Doc& doc);
    bool saveDoc(Doc& doc);
    void onMarginClick(wxStyledTextEvent& e);

    wxAuiNotebook*    m_book = nullptr;
    std::vector<Doc>  m_docs;
    DebugContext*     m_dbg = nullptr;
    std::string       m_execFile;   // file holding the execution marker, if any
    std::string       m_bpSignature; // breakpoints + open files the markers were drawn for
    std::function<void(const wxString&)> m_status;
    std::function<void()> m_bpChanged;
};
