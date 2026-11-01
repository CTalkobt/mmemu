#pragma once

#include <wx/wx.h>
#include <wx/aui/aui.h>
#include <functional>
#include <map>
#include <string>
#include <vector>

/**
 * Dockable main-window layout (#146).
 *
 *   ┌──────────────────────────────┬─────────────────┐
 *   │  centre (the machine screen) │  side tabs      │
 *   ├──────────────────────────────┴─────────────────┤
 *   │  bottom tabs                                    │
 *   └─────────────────────────────────────────────────┘
 *
 * Panes are registered once with an id and a default area. Opening a pane
 * adds it as a tab to the bottom or side notebook (whichever it was last in);
 * closing it removes the tab without destroying the window. Tabs can be
 * dragged between the two notebooks. A notebook's dock is shown only while it
 * has tabs. Which panes are open, in which area, plus the AUI dock sizes, are
 * saved to wxConfig and restored on the next start.
 */
class DockLayout {
public:
    enum class Area { Bottom, Side };

    struct Pane {
        std::string id;
        wxString    title;
        wxWindow*   window = nullptr;
        Area        defaultArea = Area::Bottom;
        bool        defaultOpen = false;
        bool        available = true;   // false: hidden from the View menu (e.g. wrong machine)
    };

    explicit DockLayout(wxFrame* frame);
    ~DockLayout();

    /**
     * Set the window shown in the centre (replaces the previous one, which is
     * hidden). nullptr shows the "no machine loaded" placeholder.
     */
    void setCenter(wxWindow* window);

    /**
     * Register a pane. It starts closed (hidden) unless restore() or
     * resetToDefaults() opens it, or it was saved as open and restore() has
     * already run.
     */
    void addPane(const std::string& id, const wxString& title, wxWindow* window,
                 Area defaultArea, bool defaultOpen);
    /** Forget a pane (e.g. a plugin pane being destroyed); its tab is removed, the window is not deleted. */
    void removePane(const std::string& id);
    bool hasPane(const std::string& id) const;
    wxWindow* paneWindow(const std::string& id) const;

    void open(const std::string& id, bool select = true);
    void close(const std::string& id);
    void toggle(const std::string& id) { isOpen(id) ? close(id) : open(id); }
    bool isOpen(const std::string& id) const;
    /** Unavailable panes are closed and left out of panes() for menus. */
    void setAvailable(const std::string& id, bool available);

    /** Registered panes in registration order (for building the View menu). */
    const std::vector<Pane>& panes() const { return m_panes; }

    /** Open the saved layout, or the defaults if none is saved. Call once after registering the built-in panes. */
    void restore();
    void save();
    void resetToDefaults();

    /**
     * The current arrangement as a string (open panes per dock + AUI dock
     * sizes), and back. Used for the Edit / Debug layouts (#149).
     */
    std::string captureState();
    void applyState(const std::string& state);
    /** Open exactly these panes (others closed), keeping the dock sizes. */
    void showOnly(const std::vector<std::string>& bottom, const std::vector<std::string>& side);

    /** Called after any open/close/move so menus can update their check marks. */
    void setOnChange(std::function<void()> cb) { m_onChange = std::move(cb); }

    wxAuiNotebook* bottomBook() const { return m_bottom; }

private:
    Pane* find(const std::string& id);
    const Pane* find(const std::string& id) const;
    const Pane* findByWindow(wxWindow* w) const;
    wxAuiNotebook* bookFor(Area a) const { return a == Area::Side ? m_side : m_bottom; }
    wxAuiNotebook* bookContaining(wxWindow* w) const;
    void addToBook(Pane& p, wxAuiNotebook* book, bool select);
    std::vector<std::string> idsIn(wxAuiNotebook* book) const;
    void closeAllPanes();
    void syncDocks();   // show/hide each notebook's dock by whether it has tabs
    void changed();

    wxFrame*       m_frame;
    wxAuiManager   m_aui;
    wxAuiNotebook* m_bottom = nullptr;
    wxAuiNotebook* m_side   = nullptr;
    wxWindow*      m_center = nullptr;
    wxWindow*      m_placeholder = nullptr;   // centre when no machine screen exists
    wxString       m_defaultPerspective;       // dock sizes before any saved layout
    std::vector<Pane> m_panes;

    // Panes the saved layout had open; used for panes that register or become
    // available after restore() (plugin panes, machine-specific panes)
    bool m_restored = false;
    std::vector<std::string> m_savedBottom, m_savedSide;
    std::map<std::string, Area> m_lastArea;   // where a closed pane was, for reopening
    std::function<void()> m_onChange;
};

namespace dock_layout_detail {
/** Comma-joined list of ids, and back (exposed for unit tests). */
std::string joinIds(const std::vector<std::string>& ids);
std::vector<std::string> splitIds(const std::string& s);
}
