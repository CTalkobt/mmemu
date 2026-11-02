#include "graphics_pane.h"
#include "libcore/main/machine_desc.h"
#include "libdebug/main/expression_evaluator.h"
#include "libdevices/main/ivideo_inspect.h"
#include "libmem/main/ibus.h"
#include <wx/clipbrd.h>
#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/wrapsizer.h>
#include <algorithm>

namespace {
const int ZOOMS[] = {1, 2, 3, 4, 6, 8};
}

// ---------------------------------------------------------------------------
// Canvas: draws the sheet scaled by the zoom, reports the item under the mouse
// ---------------------------------------------------------------------------

class GraphicsPane::Canvas : public wxScrolledWindow {
public:
    Canvas(GraphicsPane* owner, wxWindow* parent)
        : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHSCROLL | wxVSCROLL),
          m_owner(owner) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetScrollRate(8, 8);
        Bind(wxEVT_PAINT, &Canvas::OnPaint, this);
        Bind(wxEVT_MOTION, [this](wxMouseEvent& e) { m_owner->OnCanvasMotion(ItemAt(e.GetPosition())); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { m_owner->OnCanvasMotion(-1); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) { SetFocus(); m_owner->OnCanvasClick(ItemAt(e.GetPosition()), false); });
        Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent& e) { m_owner->OnCanvasClick(ItemAt(e.GetPosition()), true); });
        Bind(wxEVT_RIGHT_DOWN, [this](wxMouseEvent& e) {
            int item = ItemAt(e.GetPosition());
            m_owner->OnCanvasClick(item, false);
            m_owner->OnCanvasContext(item, e.GetPosition());
        });
    }

    void SetImage(const wxImage& img, int zoom) {
        m_zoom = zoom;
        m_bitmap = img.IsOk() ? wxBitmap(img.Scale(img.GetWidth() * zoom, img.GetHeight() * zoom, wxIMAGE_QUALITY_NEAREST))
                              : wxBitmap();
        SetVirtualSize(m_bitmap.IsOk() ? m_bitmap.GetSize() : wxSize(0, 0));
        Refresh(false);
    }

    int Zoom() const { return m_zoom; }

private:
    int ItemAt(const wxPoint& client) const {
        int x, y;
        CalcUnscrolledPosition(client.x, client.y, &x, &y);
        return gfx::itemAt(m_owner->m_sheet, x / m_zoom, y / m_zoom);
    }

    void OnPaint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        DoPrepareDC(dc);
        dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_APPWORKSPACE)));
        dc.Clear();
        if (!m_bitmap.IsOk()) {
            dc.SetTextForeground(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
            dc.DrawText(m_owner->m_bus ? "Nothing to show" : "No machine loaded", 10, 10);
            return;
        }
        dc.DrawBitmap(m_bitmap, 0, 0, false);

        const gfx::Sheet& s = m_owner->m_sheet;
        auto outline = [&](int item, const wxColour& c) {
            if (item < 0 || item >= s.count) return;
            int x = s.gap + (item % s.columns) * (s.cellW + s.gap);
            int y = s.gap + (item / s.columns) * (s.cellH + s.gap);
            dc.SetPen(wxPen(c, 2));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRectangle(x * m_zoom - 1, y * m_zoom - 1, s.cellW * m_zoom + 2, s.cellH * m_zoom + 2);
        };
        if (m_owner->m_hover != m_owner->m_selected) outline(m_owner->m_hover, wxColour(160, 160, 160));
        outline(m_owner->m_selected, wxColour(255, 200, 0));
    }

    GraphicsPane* m_owner;
    wxBitmap m_bitmap;
    int m_zoom = 2;
};

// ---------------------------------------------------------------------------

GraphicsPane::GraphicsPane(wxWindow* parent) : wxPanel(parent, wxID_ANY) {
    auto* top = new wxBoxSizer(wxVERTICAL);
    auto label = [this](const wxString& s) { return new wxStaticText(this, wxID_ANY, s); };
    auto addRow = [&](std::initializer_list<std::pair<wxWindow*, int>> items) {
        // Wraps onto further lines when the pane is narrow
        // (a label stays on the same line as the control after it)
        auto* row = new wxWrapSizer(wxHORIZONTAL);
        wxBoxSizer* pair = nullptr;
        for (auto& [w, border] : items) {
            if (dynamic_cast<wxStaticText*>(w)) {
                pair = new wxBoxSizer(wxHORIZONTAL);
                pair->Add(w, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, border);
                continue;
            }
            if (pair) {
                pair->Add(w, 0, wxALIGN_CENTER_VERTICAL);
                row->Add(pair, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, border ? border : 6);
                pair = nullptr;
            } else {
                row->Add(w, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, border ? border : 6);
            }
        }
        top->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
    };

    wxArrayString sources;
    sources.Add("Address");
    for (int i = 0; i < 8; ++i) sources.Add(wxString::Format("VIC sprite %d", i));
    sources.Add("VIC character set");
    m_source = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, sources);
    m_source->SetSelection(SRC_ADDRESS);
    m_source->SetToolTip("Show graphics at an address, or follow a sprite / the character set the video chip is using");

    m_addr = new wxTextCtrl(this, wxID_ANY, "$2000", wxDefaultPosition, wxSize(110, -1), wxTE_PROCESS_ENTER);
    m_addr->SetToolTip("Start address: any expression ($2000, label, *$FB:16 ...). Enter to apply.");

    wxArrayString fmts;
    for (const auto& f : gfx::formats()) fmts.Add(f.label);
    m_format = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, fmts);
    m_format->SetSelection(0);

    // A fixed pixel width narrower than GTK3's entry + "-" + "+" buttons makes
    // GTK log "gtk_box_gadget_distribute: assertion 'size >= 0' failed in
    // GtkSpinButton" (#161). Size each control to fit its widest value.
    auto makeSpin = [this](int lo, int hi, int initial) {
        auto* spin = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                    wxSP_ARROW_KEYS, lo, hi, initial);
        spin->SetInitialSize(spin->GetSizeFromTextSize(spin->GetTextExtent(wxString::Format("%d", hi)).x));
        return spin;
    };
    m_count = makeSpin(1, 1024, 64);
    m_count->SetToolTip("Number of items to show");
    m_cols = makeSpin(1, 64, 8);
    m_cols->SetToolTip("Items per row");
    m_stride = makeSpin(0, 65536, 0);
    m_stride->SetToolTip("Bytes from one item to the next (0 = the format's size: 64 for sprites, 8 for characters)");

    wxArrayString zooms;
    for (int z : ZOOMS) zooms.Add(wxString::Format("%dx", z));
    m_zoom = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, zooms);
    m_zoom->SetSelection(1);

    wxArrayString mems;
    mems.Add("CPU view");
    mems.Add("Video chip view");
    m_memSel = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, mems);
    m_memSel->SetSelection(0);
    m_memSel->SetToolTip("CPU view reads through the CPU's memory map; video chip view reads what the "
                         "VIC fetches (e.g. the character ROM at $1000/$9000 on a C64)");

    m_palBase = makeSpin(0, 255, 0);
    m_palBase->SetToolTip("16-colour formats: palette index added to each nibble (sprite number x 16, or the colour RAM high nibble for NCM)");
    m_swap = new wxCheckBox(this, wxID_ANY, "Swap nibbles");
    m_live = new wxCheckBox(this, wxID_ANY, "Live");
    m_live->SetValue(true);
    m_live->SetToolTip("Redraw as memory changes");
    auto* exportBtn = new wxButton(this, wxID_ANY, "Export PNG...");

    addRow({{m_source, 6}, {label("Address"), 3}, {m_addr, 6}, {m_format, 0}});
    addRow({{label("Count"), 3}, {m_count, 6}, {label("Columns"), 3}, {m_cols, 6},
            {label("Stride"), 3}, {m_stride, 6}, {label("Zoom"), 3}, {m_zoom, 0}});
    addRow({{m_memSel, 6}, {label("Palette +"), 3}, {m_palBase, 6}, {m_swap, 6}, {m_live, 6}, {exportBtn, 0}});

    m_canvas = new Canvas(this, this);
    top->Add(m_canvas, 1, wxEXPAND | wxALL, 4);
    m_status = new wxStaticText(this, wxID_ANY, "");
    top->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
    SetSizer(top);

    auto changed = [this](wxEvent&) { OnSettingChanged(); };
    m_source->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { OnSourceChanged(); });
    m_addr->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { OnAddressEnter(); });
    m_format->Bind(wxEVT_CHOICE, changed);
    m_zoom->Bind(wxEVT_CHOICE, changed);
    m_memSel->Bind(wxEVT_CHOICE, changed);
    for (auto* s : {m_count, m_cols, m_stride, m_palBase}) s->Bind(wxEVT_SPINCTRL, changed);
    m_swap->Bind(wxEVT_CHECKBOX, changed);
    m_live->Bind(wxEVT_CHECKBOX, changed);
    exportBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnExport(); });

    m_start = 0x2000;
}

void GraphicsPane::SetMachine(MachineDescriptor* desc, IBus* cpuBus, DebugContext* dbg) {
    m_bus = cpuBus;
    m_dbg = dbg;
    m_video = gfx::findVideoInspect(desc);
    // Following the VIC needs a video chip that supports inspection
    for (unsigned i = SRC_SPRITE0; i <= SRC_CHARSET; ++i)
        m_source->SetString(i, m_video ? (i == SRC_CHARSET ? wxString("VIC character set")
                                                           : wxString::Format("VIC sprite %d", i - SRC_SPRITE0))
                                        : wxString("(no video chip)"));
    m_memSel->Enable(m_video != nullptr);
    if (!m_video) {
        m_source->SetSelection(SRC_ADDRESS);
        m_memSel->SetSelection(0);
    }
    m_selected = m_hover = -1;
    m_dirty = true;
    Rebuild();
}

gfx::Format GraphicsPane::SelectedFormat() const {
    int sel = m_format->GetSelection();
    const auto& all = gfx::formats();
    return (sel >= 0 && sel < (int)all.size()) ? all[sel].format : gfx::Format::SpriteHires;
}

void GraphicsPane::SelectFormat(gfx::Format f) {
    const auto& all = gfx::formats();
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i].format == f) m_format->SetSelection((int)i);
}

void GraphicsPane::OnSettingChanged() {
    m_dirty = true;
    Rebuild();
}

void GraphicsPane::OnSourceChanged() {
    int src = m_source->GetSelection();
    if (src != SRC_ADDRESS && !m_video) { m_source->SetSelection(SRC_ADDRESS); return; }
    bool follow = src != SRC_ADDRESS;
    m_addr->Enable(!follow);
    m_format->Enable(!follow);
    if (follow) {
        m_memSel->SetSelection(1);   // the VIC's own view
        if (src == SRC_CHARSET) { m_count->SetValue(256); m_cols->SetValue(16); m_stride->SetValue(0); }
        else                    { m_count->SetValue(1);   m_cols->SetValue(1); }
    }
    m_selected = -1;
    OnSettingChanged();
}

void GraphicsPane::OnAddressEnter() {
    uint32_t addr = 0;
    std::string text = m_addr->GetValue().ToStdString();
    if (!ExpressionEvaluator::evaluate(text, m_dbg, addr, 16)) {
        UpdateStatus(-1);
        m_status->SetLabel("Can't evaluate address: " + m_addr->GetValue());
        return;
    }
    m_start = addr;
    m_selected = -1;
    OnSettingChanged();
}

void GraphicsPane::RefreshValues() {
    if (!IsShownOnScreen()) return;
    auto now = std::chrono::steady_clock::now();
    if (!m_dirty) {
        if (!m_live->GetValue()) return;
        if (now - m_lastDraw < std::chrono::milliseconds(100)) return;
    }
    Rebuild();
}

void GraphicsPane::Rebuild() {
    m_dirty = false;
    m_lastDraw = std::chrono::steady_clock::now();
    if (!m_bus) {
        m_sheet = gfx::Sheet{};
        m_canvas->SetImage(wxImage(), 1);
        return;
    }

    gfx::ViewRequest req;
    int src = m_source->GetSelection();
    if (src >= SRC_SPRITE0 && src < SRC_CHARSET) { req.source = gfx::Source::Sprite; req.sprite = src - SRC_SPRITE0; }
    else if (src == SRC_CHARSET)                  req.source = gfx::Source::Charset;
    req.addr        = m_start;
    req.format      = SelectedFormat();
    req.count       = m_count->GetValue();
    req.columns     = m_cols->GetValue();
    req.stride      = (uint32_t)m_stride->GetValue();
    req.videoView   = m_memSel->GetSelection() == 1;
    req.swapNibbles = m_swap->GetValue();
    req.paletteBase = req.source == gfx::Source::Sprite ? -1 : m_palBase->GetValue();

    gfx::View view = gfx::buildView(req, m_bus, m_video);
    if (!view.error.empty()) {
        m_sheet = gfx::Sheet{};
        m_canvas->SetImage(wxImage(), 1);
        m_status->SetLabel(view.error);
        return;
    }
    m_sheet = std::move(view.sheet);
    if (src != SRC_ADDRESS) {
        m_start = view.start;
        SelectFormat(view.format);
        m_addr->ChangeValue(wxString::Format("$%X", m_start));
        m_palBase->SetValue(view.paletteBase);
    }

    wxImage img;
    if (m_sheet.width > 0 && m_sheet.height > 0) {
        img.Create(m_sheet.width, m_sheet.height, false);
        unsigned char* rgb = img.GetData();
        for (size_t i = 0; i < m_sheet.argb.size(); ++i) {
            uint32_t px = m_sheet.argb[i];
            rgb[i * 3]     = (px >> 16) & 0xFF;
            rgb[i * 3 + 1] = (px >> 8) & 0xFF;
            rgb[i * 3 + 2] = px & 0xFF;
        }
    }
    int zi = m_zoom->GetSelection();
    m_canvas->SetImage(img, ZOOMS[(zi >= 0 && zi < (int)(sizeof(ZOOMS) / sizeof(ZOOMS[0]))) ? zi : 1]);
    if (m_selected >= m_sheet.count) m_selected = -1;
    UpdateStatus(m_hover);
}

void GraphicsPane::UpdateStatus(int item) {
    int shown = item >= 0 ? item : m_selected;
    wxString s = wxString::Format("%s at $%X, %d item%s, %u bytes each",
                                  gfx::info(SelectedFormat()).label, m_start, m_sheet.count,
                                  m_sheet.count == 1 ? "" : "s", m_sheet.stride);
    if (shown >= 0 && shown < m_sheet.count)
        s = wxString::Format("#%d at $%X    ", shown, gfx::itemAddress(m_sheet, shown)) + s;
    m_status->SetLabel(s);
}

void GraphicsPane::OnCanvasMotion(int item) {
    if (item == m_hover) return;
    m_hover = item;
    UpdateStatus(item);
    m_canvas->Refresh(false);
}

void GraphicsPane::OnCanvasClick(int item, bool dbl) {
    if (item < 0) return;
    m_selected = item;
    m_canvas->Refresh(false);
    UpdateStatus(item);
    uint32_t addr = gfx::itemAddress(m_sheet, item);
    if (dbl && m_onMemory) m_onMemory(addr);
    else if (m_onStatus) m_onStatus(wxString::Format("Graphics item #%d at $%X", item, addr));
}

void GraphicsPane::OnCanvasContext(int item, const wxPoint& pos) {
    if (item < 0) return;
    uint32_t addr = gfx::itemAddress(m_sheet, item);
    wxMenu menu;
    const int ID_MEM = wxWindow::NewControlId(), ID_COPY = wxWindow::NewControlId(), ID_START = wxWindow::NewControlId();
    menu.Append(ID_MEM, wxString::Format("Show $%X in memory pane", addr));
    menu.Append(ID_COPY, "Copy address");
    if (m_source->GetSelection() == SRC_ADDRESS) menu.Append(ID_START, "Start the view here");
    int id = m_canvas->GetPopupMenuSelectionFromUser(menu, pos);
    if (id == ID_MEM && m_onMemory) {
        m_onMemory(addr);
    } else if (id == ID_COPY && wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(wxString::Format("$%X", addr)));
        wxTheClipboard->Close();
    } else if (id == ID_START) {
        m_start = addr;
        m_addr->ChangeValue(wxString::Format("$%X", addr));
        m_selected = 0;
        OnSettingChanged();
    }
}

void GraphicsPane::OnExport() {
    if (m_sheet.width <= 0) { m_status->SetLabel("Nothing to export."); return; }
    wxFileDialog dlg(this, "Export graphics", "", wxString::Format("gfx_%X.png", m_start),
                     "PNG files (*.png)|*.png", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    wxImage img(m_sheet.width, m_sheet.height, false);
    unsigned char* rgb = img.GetData();
    for (size_t i = 0; i < m_sheet.argb.size(); ++i) {
        uint32_t px = m_sheet.argb[i];
        rgb[i * 3] = (px >> 16) & 0xFF; rgb[i * 3 + 1] = (px >> 8) & 0xFF; rgb[i * 3 + 2] = px & 0xFF;
    }
    int zoom = m_canvas->Zoom();
    if (zoom > 1) img = img.Scale(img.GetWidth() * zoom, img.GetHeight() * zoom, wxIMAGE_QUALITY_NEAREST);
    if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG)) wxImage::AddHandler(new wxPNGHandler);
    bool ok = img.SaveFile(dlg.GetPath(), wxBITMAP_TYPE_PNG);
    m_status->SetLabel(ok ? "Saved " + dlg.GetPath() : "Could not save " + dlg.GetPath());
}
