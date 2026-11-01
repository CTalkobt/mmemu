#include "dock_layout.h"
#include <wx/config.h>
#include <algorithm>
#include <sstream>

namespace dock_layout_detail {

std::string joinIds(const std::vector<std::string>& ids) {
    std::string out;
    for (const auto& id : ids) {
        if (!out.empty()) out += ',';
        out += id;
    }
    return out;
}

std::vector<std::string> splitIds(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
        if (!item.empty()) out.push_back(item);
    return out;
}

}  // namespace dock_layout_detail

using dock_layout_detail::joinIds;
using dock_layout_detail::splitIds;

// Bump when the saved layout format or the dock structure changes; older
// saved layouts are then ignored in favour of the defaults.
static constexpr long kLayoutVersion = 1;

static bool contains(const std::vector<std::string>& v, const std::string& id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}

DockLayout::DockLayout(wxFrame* frame) : m_frame(frame) {
    m_aui.SetManagedWindow(frame);

    const long nbStyle = wxAUI_NB_DEFAULT_STYLE | wxAUI_NB_TAB_EXTERNAL_MOVE | wxAUI_NB_WINDOWLIST_BUTTON;
    m_bottom = new wxAuiNotebook(frame, wxID_ANY, wxDefaultPosition, wxSize(-1, 260), nbStyle);
    m_side   = new wxAuiNotebook(frame, wxID_ANY, wxDefaultPosition, wxSize(460, -1), nbStyle);

    m_aui.AddPane(m_bottom, wxAuiPaneInfo().Name("bottom").Caption("Tools").Bottom()
                                .BestSize(-1, 260).MinSize(-1, 80).CaptionVisible(false)
                                .CloseButton(false).Hide());
    m_aui.AddPane(m_side, wxAuiPaneInfo().Name("side").Caption("Side").Right()
                              .BestSize(460, -1).MinSize(200, -1).CaptionVisible(false)
                              .CloseButton(false).Hide());

    auto* placeholder = new wxPanel(frame);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddStretchSpacer();
    sizer->Add(new wxStaticText(placeholder, wxID_ANY, "No machine loaded - File > Load Machine (Ctrl-L)"),
               0, wxALIGN_CENTER);
    sizer->AddStretchSpacer();
    placeholder->SetSizer(sizer);
    m_placeholder = placeholder;
    setCenter(nullptr);

    for (wxAuiNotebook* book : {m_bottom, m_side}) {
        // Closing a tab hides the pane; the window is kept for reopening
        book->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, [this, book](wxAuiNotebookEvent& e) {
            e.Veto();
            if (const Pane* p = findByWindow(book->GetPage(e.GetSelection()))) close(p->id);
        });
        // Tabs may be dragged between the bottom and side notebooks
        book->Bind(wxEVT_AUINOTEBOOK_ALLOW_DND, [](wxAuiNotebookEvent& e) { e.Allow(); });
        book->Bind(wxEVT_AUINOTEBOOK_DRAG_DONE, [this](wxAuiNotebookEvent& e) {
            e.Skip();
            m_frame->CallAfter([this]() { syncDocks(); changed(); });
        });
    }

    m_aui.Update();
    m_defaultPerspective = m_aui.SavePerspective();
}

DockLayout::~DockLayout() {
    m_aui.UnInit();
}

void DockLayout::setCenter(wxWindow* window) {
    if (!window) window = m_placeholder;
    if (window == m_center) return;
    if (m_center) {
        m_aui.DetachPane(m_center);
        m_center->Hide();
    }
    if (window->GetParent() != m_frame) window->Reparent(m_frame);
    m_aui.AddPane(window, wxAuiPaneInfo().Name("center").CenterPane());
    window->Show();
    m_center = window;
    m_aui.Update();
}

DockLayout::Pane* DockLayout::find(const std::string& id) {
    for (auto& p : m_panes) if (p.id == id) return &p;
    return nullptr;
}

const DockLayout::Pane* DockLayout::find(const std::string& id) const {
    for (const auto& p : m_panes) if (p.id == id) return &p;
    return nullptr;
}

const DockLayout::Pane* DockLayout::findByWindow(wxWindow* w) const {
    for (const auto& p : m_panes) if (p.window == w) return &p;
    return nullptr;
}

wxAuiNotebook* DockLayout::bookContaining(wxWindow* w) const {
    if (!w) return nullptr;
    if (m_bottom->GetPageIndex(w) != wxNOT_FOUND) return m_bottom;
    if (m_side->GetPageIndex(w) != wxNOT_FOUND) return m_side;
    return nullptr;
}

bool DockLayout::hasPane(const std::string& id) const { return find(id) != nullptr; }

wxWindow* DockLayout::paneWindow(const std::string& id) const {
    const Pane* p = find(id);
    return p ? p->window : nullptr;
}

bool DockLayout::isOpen(const std::string& id) const {
    const Pane* p = find(id);
    return p && bookContaining(p->window) != nullptr;
}

void DockLayout::addToBook(Pane& p, wxAuiNotebook* book, bool select) {
    p.window->Show();
    book->AddPage(p.window, p.title, select);   // AddPage reparents the window
}

void DockLayout::addPane(const std::string& id, const wxString& title, wxWindow* window,
                         Area defaultArea, bool defaultOpen) {
    if (!window || find(id)) return;
    window->Hide();
    if (window->GetParent() != m_frame) window->Reparent(m_frame);
    m_panes.push_back({id, title, window, defaultArea, defaultOpen, true});

    // Late registration (plugin panes): reopen where the saved layout had it
    if (m_restored) {
        Pane& p = m_panes.back();
        if (contains(m_savedBottom, id))    addToBook(p, m_bottom, false);
        else if (contains(m_savedSide, id)) addToBook(p, m_side, false);
        syncDocks();
        changed();
    }
}

void DockLayout::removePane(const std::string& id) {
    auto it = std::find_if(m_panes.begin(), m_panes.end(), [&](const Pane& p) { return p.id == id; });
    if (it == m_panes.end()) return;
    if (wxAuiNotebook* book = bookContaining(it->window))
        book->RemovePage(book->GetPageIndex(it->window));
    m_panes.erase(it);
    syncDocks();
    changed();
}

void DockLayout::open(const std::string& id, bool select) {
    Pane* p = find(id);
    if (!p || !p->available) return;
    if (wxAuiNotebook* book = bookContaining(p->window)) {
        if (select) book->SetSelection(book->GetPageIndex(p->window));
    } else {
        Area area = p->defaultArea;
        if (auto it = m_lastArea.find(id); it != m_lastArea.end()) area = it->second;
        else if (contains(m_savedSide, id)) area = Area::Side;
        else if (contains(m_savedBottom, id)) area = Area::Bottom;
        addToBook(*p, bookFor(area), select);
    }
    syncDocks();
    changed();
}

void DockLayout::close(const std::string& id) {
    Pane* p = find(id);
    if (!p) return;
    if (wxAuiNotebook* book = bookContaining(p->window)) {
        // Remember the area so reopening puts it back where it was; it is no
        // longer "saved as open"
        m_lastArea[id] = (book == m_side) ? Area::Side : Area::Bottom;
        for (auto* v : {&m_savedBottom, &m_savedSide})
            v->erase(std::remove(v->begin(), v->end(), id), v->end());

        book->RemovePage(book->GetPageIndex(p->window));
        p->window->Hide();
        p->window->Reparent(m_frame);
    }
    syncDocks();
    changed();
}

void DockLayout::setAvailable(const std::string& id, bool available) {
    Pane* p = find(id);
    if (!p || p->available == available) return;
    if (!available && isOpen(id)) {
        // Keep it "saved as open" so it returns when it becomes available again
        bool side = bookContaining(p->window) == m_side;
        close(id);
        (side ? m_savedSide : m_savedBottom).push_back(id);
    }
    p->available = available;
    if (available && m_restored) {
        if (contains(m_savedBottom, id))    addToBook(*p, m_bottom, false);
        else if (contains(m_savedSide, id)) addToBook(*p, m_side, false);
        syncDocks();
    }
    changed();
}

void DockLayout::syncDocks() {
    m_aui.GetPane(m_bottom).Show(m_bottom->GetPageCount() > 0);
    m_aui.GetPane(m_side).Show(m_side->GetPageCount() > 0);
    m_aui.Update();
}

void DockLayout::changed() {
    if (m_onChange) m_onChange();
}

void DockLayout::restore() {
    wxConfigBase* cfg = wxConfigBase::Get();
    wxString bottom, side, perspective;
    bool haveSaved = cfg && cfg->ReadLong("Layout/Version", 0) == kLayoutVersion &&
                     cfg->Read("Layout/Bottom", &bottom) && cfg->Read("Layout/Side", &side);
    m_restored = true;
    if (!haveSaved) {
        resetToDefaults();
        return;
    }
    m_savedBottom = splitIds(bottom.ToStdString());
    m_savedSide   = splitIds(side.ToStdString());
    for (const auto& id : m_savedBottom)
        if (Pane* p = find(id); p && p->available) addToBook(*p, m_bottom, false);
    for (const auto& id : m_savedSide)
        if (Pane* p = find(id); p && p->available) addToBook(*p, m_side, false);
    if (cfg->Read("Layout/Perspective", &perspective) && !perspective.empty())
        m_aui.LoadPerspective(perspective, false);
    if (m_bottom->GetPageCount() > 0) m_bottom->SetSelection(0);
    if (m_side->GetPageCount() > 0) m_side->SetSelection(0);
    syncDocks();
    changed();
}

void DockLayout::save() {
    wxConfigBase* cfg = wxConfigBase::Get();
    if (!cfg) return;
    auto idsIn = [this](wxAuiNotebook* book) {
        std::vector<std::string> ids;
        for (size_t i = 0; i < book->GetPageCount(); ++i)
            if (const Pane* p = findByWindow(book->GetPage(i))) ids.push_back(p->id);
        return ids;
    };
    // Keep panes that are open but not registered right now (plugin panes for
    // another machine) so they come back when that machine is loaded
    auto merge = [this](std::vector<std::string> open, const std::vector<std::string>& saved) {
        for (const auto& id : saved)
            if (!find(id) && !contains(open, id)) open.push_back(id);
        return open;
    };
    cfg->Write("Layout/Version", kLayoutVersion);
    cfg->Write("Layout/Bottom", wxString(joinIds(merge(idsIn(m_bottom), m_savedBottom))));
    cfg->Write("Layout/Side", wxString(joinIds(merge(idsIn(m_side), m_savedSide))));
    cfg->Write("Layout/Perspective", m_aui.SavePerspective());
    cfg->Flush();
}

void DockLayout::resetToDefaults() {
    for (auto& p : m_panes)
        if (wxAuiNotebook* book = bookContaining(p.window)) {
            book->RemovePage(book->GetPageIndex(p.window));
            p.window->Hide();
            p.window->Reparent(m_frame);
        }
    m_savedBottom.clear();
    m_savedSide.clear();
    m_lastArea.clear();
    for (auto& p : m_panes)
        if (p.defaultOpen && p.available) addToBook(p, bookFor(p.defaultArea), false);
    if (m_bottom->GetPageCount() > 0) m_bottom->SetSelection(0);
    if (m_side->GetPageCount() > 0) m_side->SetSelection(0);
    m_aui.LoadPerspective(m_defaultPerspective, false);
    syncDocks();
    changed();
}
