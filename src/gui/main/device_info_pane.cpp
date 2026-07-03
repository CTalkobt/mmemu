#include "device_info_pane.h"
#include <iomanip>
#include <sstream>
#include <wx/splitter.h>
#include <wx/dcbuffer.h>

static std::string toHex(uint32_t v, int width = 4) {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0') << std::setw(width) << v;
    return ss.str();
}

DeviceInfoPane::DeviceInfoPane(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    
    m_deviceSelector = new wxComboBox(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
    m_deviceSelector->Bind(wxEVT_COMBOBOX, &DeviceInfoPane::onDeviceSelected, this);
    sizer->Add(m_deviceSelector, 0, wxEXPAND | wxALL, 5);

    auto* splitter = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE);
    sizer->Add(splitter, 1, wxEXPAND | wxALL, 0);

    m_tree = new wxTreeCtrl(splitter, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTR_DEFAULT_STYLE | wxTR_HIDE_ROOT);
    m_tree->Bind(wxEVT_TREE_ITEM_RIGHT_CLICK, &DeviceInfoPane::onTreeRightClick, this);
    m_tree->Bind(wxEVT_TREE_ITEM_ACTIVATED, &DeviceInfoPane::onTreeActivated, this);

    m_bitmapWindow = new wxScrolledWindow(splitter, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxHSCROLL);
    m_bitmapWindow->SetBackgroundColour(*wxBLACK);
    m_bitmapWindow->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_bitmapWindow->Bind(wxEVT_PAINT, &DeviceInfoPane::onPaintBitmaps, this);
    
    splitter->SplitHorizontally(m_tree, m_bitmapWindow, 300);
    
    SetSizer(sizer);
}

void DeviceInfoPane::setMachine(MachineDescriptor* desc) {
    m_desc = desc;
    m_deviceSelector->Clear();
    if (m_desc && m_desc->ioRegistry) {
        std::vector<IOHandler*> handlers;
        m_desc->ioRegistry->enumerate(handlers);
        for (auto* h : handlers) {
            m_deviceSelector->Append(h->name());
        }
        if (m_deviceSelector->GetCount() > 0) {
            m_deviceSelector->SetSelection(0);
        }
    }
    refreshValues();
}

void DeviceInfoPane::onDeviceSelected(wxCommandEvent& /*evt*/) {
    refreshValues();
}

void DeviceInfoPane::refreshValues() {
    m_tree->DeleteAllItems();
    m_regItemMap.clear();
    if (!m_desc || !m_desc->ioRegistry) return;

    int sel = m_deviceSelector->GetSelection();
    if (sel == wxNOT_FOUND) {
        m_currentInfo = DeviceInfo();
        m_bitmapWindow->Refresh();
        return;
    }

    std::string name = m_deviceSelector->GetString(sel).ToStdString();
    IOHandler* handler = m_desc->ioRegistry->findHandler(name);
    if (!handler) {
        m_currentInfo = DeviceInfo();
        m_bitmapWindow->Refresh();
        return;
    }

    m_currentInfo = DeviceInfo();
    handler->getDeviceInfo(m_currentInfo);
    const DeviceInfo& info = m_currentInfo;

    wxTreeItemId root = m_tree->AddRoot("Device");
    
    wxTreeItemId identity = m_tree->AppendItem(root, "Identity");
    m_tree->AppendItem(identity, "Name: " + info.name);
    m_tree->AppendItem(identity, "Base Address: $" + toHex(info.baseAddr));
    m_tree->AppendItem(identity, "Address Mask: $" + toHex(info.addrMask));
    m_tree->Expand(identity);

    if (!info.dependencies.empty()) {
        wxTreeItemId deps = m_tree->AppendItem(root, "Dependencies");
        for (const auto& d : info.dependencies) {
            m_tree->AppendItem(deps, d.first + ": " + d.second);
        }
        m_tree->Expand(deps);
    }

    if (!info.state.empty()) {
        wxTreeItemId stateNode = m_tree->AppendItem(root, "Internal State");
        for (const auto& s : info.state) {
            m_tree->AppendItem(stateNode, s.first + ": " + s.second);
        }
        m_tree->Expand(stateNode);
    }

    if (!info.registers.empty()) {
        wxTreeItemId regsNode = m_tree->AppendItem(root, "Registers");
        for (int i = 0; i < (int)info.registers.size(); ++i) {
            const auto& r = info.registers[i];
            std::string label = r.name + " ($" + toHex(r.offset, 2) + "): $" + toHex(r.value, 2);
            if (!r.description.empty()) label += "  " + r.description;
            wxTreeItemId item = m_tree->AppendItem(regsNode, label);
            m_regItemMap[item] = i;
        }
        m_tree->Expand(regsNode);
    }

    // Update bitmap window virtual size
    int totalHeight = 10;
    int maxWidth = 0;
    int margin = 20;
    int gridScale = 12;
    
    for (const auto& bm : info.bitmaps) {
        totalHeight += 20 + (bm.height * gridScale) + margin;
        maxWidth = std::max(maxWidth, bm.width * gridScale + 20);
    }
    
    m_bitmapWindow->SetVirtualSize(maxWidth, totalHeight);
    m_bitmapWindow->SetScrollRate(10, 10);
    m_bitmapWindow->Refresh();
}

void DeviceInfoPane::onTreeRightClick(wxTreeEvent& evt) {
    wxTreeItemId item = evt.GetItem();
    auto it = m_regItemMap.find(item);
    if (it == m_regItemMap.end()) return;

    wxMenu menu;
    menu.Append(wxID_EDIT, "Edit Value...");
    menu.Bind(wxEVT_MENU, [this, idx = it->second](wxCommandEvent&) {
        editRegister(idx);
    }, wxID_EDIT);
    PopupMenu(&menu);
}

void DeviceInfoPane::onTreeActivated(wxTreeEvent& evt) {
    wxTreeItemId item = evt.GetItem();
    auto it = m_regItemMap.find(item);
    if (it != m_regItemMap.end()) {
        editRegister(it->second);
    }
}

void DeviceInfoPane::editRegister(int regIndex) {
    if (!m_desc || !m_desc->ioRegistry) return;
    if (regIndex < 0 || regIndex >= (int)m_currentInfo.registers.size()) return;

    const auto& reg = m_currentInfo.registers[regIndex];
    uint32_t addr = m_currentInfo.baseAddr + reg.offset;

    char curVal[8];
    std::snprintf(curVal, sizeof(curVal), "%02X", reg.value);

    wxTextEntryDialog dlg(this,
        wxString::Format("Edit %s at $%04X\nCurrent value: $%02X",
                         reg.name.c_str(), addr, reg.value),
        "Edit Register", curVal);

    if (dlg.ShowModal() == wxID_OK) {
        unsigned long newVal = 0;
        wxString input = dlg.GetValue();
        // Strip optional $ prefix
        if (input.StartsWith("$")) input = input.Mid(1);
        if (input.ToULong(&newVal, 16) && newVal <= 0xFF) {
            m_desc->ioRegistry->dispatchWrite(nullptr, addr, (uint8_t)newVal);
            refreshValues();
        } else {
            wxMessageBox("Invalid hex value (00-FF)", "Error", wxOK | wxICON_ERROR);
        }
    }
}

void DeviceInfoPane::onPaintBitmaps(wxPaintEvent& /*evt*/) {
    wxAutoBufferedPaintDC dc(m_bitmapWindow);
    m_bitmapWindow->DoPrepareDC(dc);
    dc.Clear();

    if (m_currentInfo.bitmaps.empty()) return;

    int x = 10;
    int y = 10;
    int gridScale = 12; // Zoom factor
    int margin = 20;

    dc.SetFont(*wxSMALL_FONT);
    
    for (const auto& bm : m_currentInfo.bitmaps) {
        dc.SetTextForeground(*wxWHITE);
        dc.DrawText(bm.name, x, y);
        y += 20;

        // Draw grid
        dc.SetPen(wxPen(wxColour(64, 64, 64)));
        for (int r = 0; r <= bm.height; ++r) {
            dc.DrawLine(x, y + r * gridScale, x + bm.width * gridScale, y + r * gridScale);
        }
        for (int c = 0; c <= bm.width; ++c) {
            dc.DrawLine(x + c * gridScale, y, x + c * gridScale, y + bm.height * gridScale);
        }

        // Draw pixels
        for (int r = 0; r < bm.height; ++r) {
            for (int c = 0; c < bm.width; ++c) {
                uint32_t pixel = bm.pixels[r * bm.width + c];
                if ((pixel >> 24) != 0) { // Alpha check
                    uint8_t blue  = (pixel >> 16) & 0xFF;
                    uint8_t green = (pixel >> 8)  & 0xFF;
                    uint8_t red   = pixel         & 0xFF;
                    dc.SetBrush(wxBrush(wxColour(red, green, blue)));
                    dc.SetPen(*wxTRANSPARENT_PEN);
                    dc.DrawRectangle(x + c * gridScale + 1, y + r * gridScale + 1, gridScale - 1, gridScale - 1);
                }
            }
        }

        y += bm.height * gridScale + margin;
    }
}
