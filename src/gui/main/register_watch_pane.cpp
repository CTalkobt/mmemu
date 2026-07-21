#include "register_watch_pane.h"
#include "libdevices/main/io_registry.h"
#include <iomanip>
#include <sstream>

static std::string toHex(uint32_t v, int width = 2) {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0') << std::setw(width) << v;
    return ss.str();
}

static std::string toBin(uint8_t v) {
    char buf[9];
    for (int i = 7; i >= 0; --i)
        buf[7 - i] = (v & (1 << i)) ? '1' : '0';
    buf[8] = '\0';
    return buf;
}

RegisterWatchPane::RegisterWatchPane(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    m_deviceSelector = new wxComboBox(this, wxID_ANY, "", wxDefaultPosition,
                                      wxDefaultSize, 0, nullptr, wxCB_READONLY);
    m_deviceSelector->Bind(wxEVT_COMBOBOX, &RegisterWatchPane::onDeviceSelected, this);
    sizer->Add(m_deviceSelector, 0, wxEXPAND | wxALL, 4);

    m_grid = new wxGrid(this, wxID_ANY);
    m_grid->CreateGrid(0, 5);
    m_grid->SetColLabelValue(0, "Addr");
    m_grid->SetColLabelValue(1, "Name");
    m_grid->SetColLabelValue(2, "Hex");
    m_grid->SetColLabelValue(3, "Binary");
    m_grid->SetColLabelValue(4, "Decoded");
    m_grid->SetColLabelSize(20);
    m_grid->SetRowLabelSize(0);
    m_grid->SetDefaultCellFont(wxFont(9, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
    m_grid->EnableEditing(false);
    m_grid->SetSelectionMode(wxGrid::wxGridSelectRows);

    m_grid->SetColSize(0, 55);
    m_grid->SetColSize(1, 75);
    m_grid->SetColSize(2, 35);
    m_grid->SetColSize(3, 75);
    m_grid->SetColSize(4, 220);

    sizer->Add(m_grid, 1, wxEXPAND | wxALL, 0);
    SetSizer(sizer);
}

void RegisterWatchPane::setMachine(MachineDescriptor* desc) {
    m_desc = desc;
    m_deviceSelector->Clear();
    m_prevValues.clear();
    if (m_desc && m_desc->ioRegistry) {
        std::vector<IOHandler*> handlers;
        m_desc->ioRegistry->enumerate(handlers);
        for (auto* h : handlers)
            m_deviceSelector->Append(h->name());
        if (m_deviceSelector->GetCount() > 0)
            m_deviceSelector->SetSelection(0);
    }
    rebuildGrid();
}

void RegisterWatchPane::onDeviceSelected(wxCommandEvent&) {
    m_prevValues.clear();
    rebuildGrid();
}

std::string RegisterWatchPane::decodeBitFields(const DeviceRegister& reg) const {
    if (reg.bitFields.empty()) {
        if (!reg.description.empty()) return reg.description;
        return "";
    }

    std::ostringstream ss;
    bool first = true;
    for (const auto& bf : reg.bitFields) {
        uint8_t mask = ((1 << bf.width) - 1) << bf.startBit;
        uint8_t val = (reg.value & mask) >> bf.startBit;
        if (!first) ss << " ";
        ss << bf.name << "=" << (int)val;
        first = false;
    }
    return ss.str();
}

void RegisterWatchPane::rebuildGrid() {
    if (m_grid->GetNumberRows() > 0)
        m_grid->DeleteRows(0, m_grid->GetNumberRows());

    if (!m_desc || !m_desc->ioRegistry) return;

    int sel = m_deviceSelector->GetSelection();
    if (sel == wxNOT_FOUND) return;

    std::string devName = m_deviceSelector->GetString(sel).ToStdString();
    IOHandler* handler = m_desc->ioRegistry->findHandler(devName);
    if (!handler) return;

    DeviceInfo info;
    handler->getDeviceInfo(info);

    m_grid->AppendRows(info.registers.size());

    for (size_t i = 0; i < info.registers.size(); ++i) {
        const auto& reg = info.registers[i];
        uint32_t addr = info.baseAddr + reg.offset;
        int row = (int)i;

        m_grid->SetCellValue(row, 0, "$" + toHex(addr, 4));
        m_grid->SetCellValue(row, 1, reg.name);
        m_grid->SetCellValue(row, 2, "$" + toHex(reg.value));
        m_grid->SetCellValue(row, 3, toBin(reg.value));
        m_grid->SetCellValue(row, 4, decodeBitFields(reg));
    }
}

void RegisterWatchPane::refreshValues() {
    if (!m_desc || !m_desc->ioRegistry) return;

    int sel = m_deviceSelector->GetSelection();
    if (sel == wxNOT_FOUND) return;

    std::string devName = m_deviceSelector->GetString(sel).ToStdString();
    IOHandler* handler = m_desc->ioRegistry->findHandler(devName);
    if (!handler) return;

    DeviceInfo info;
    handler->getDeviceInfo(info);

    // Ensure grid row count matches
    int rows = m_grid->GetNumberRows();
    if (rows != (int)info.registers.size()) {
        rebuildGrid();
        return;
    }

    ++m_changeAge;

    for (size_t i = 0; i < info.registers.size(); ++i) {
        const auto& reg = info.registers[i];
        uint32_t addr = info.baseAddr + reg.offset;
        int row = (int)i;

        m_grid->SetCellValue(row, 2, "$" + toHex(reg.value));
        m_grid->SetCellValue(row, 3, toBin(reg.value));
        m_grid->SetCellValue(row, 4, decodeBitFields(reg));

        // Change highlighting
        auto it = m_prevValues.find(addr);
        bool changed = (it != m_prevValues.end() && it->second != reg.value);
        if (changed) {
            wxColour highlight(255, 255, 180); // Light yellow
            for (int c = 0; c < 5; ++c)
                m_grid->SetCellBackgroundColour(row, c, highlight);
        } else {
            for (int c = 0; c < 5; ++c)
                m_grid->SetCellBackgroundColour(row, c, *wxWHITE);
        }

        m_prevValues[addr] = reg.value;
    }
}
