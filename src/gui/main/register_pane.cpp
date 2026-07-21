#include "register_pane.h"
#include <wx/settings.h>
#include <iomanip>
#include <sstream>
#include <cstring>

RegisterPane::RegisterPane(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    m_fixedFont = wxFont(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    m_grid = new wxGrid(this, wxID_ANY);
    m_grid->CreateGrid(0, 2);
    m_grid->SetColLabelValue(0, "Register");
    m_grid->SetColLabelValue(1, "Value");
    m_grid->SetRowLabelSize(0);
    m_grid->HideRowLabels();
    m_grid->SetColFormatCustom(1, wxGRID_VALUE_STRING);
    m_grid->SetDefaultCellFont(m_fixedFont);
    m_grid->DisableDragColSize();
    m_grid->DisableDragRowSize();
    m_grid->EnableEditing(false);

    sizer->Add(m_grid, 1, wxEXPAND | wxALL, 5);

    m_annotationLabel = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0);
    m_annotationLabel->SetFont(m_fixedFont);
    sizer->Add(m_annotationLabel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

    SetSizer(sizer);
}

void RegisterPane::SetCPU(ICore* cpu) {
    m_cpu = cpu;
    if (m_grid->GetNumberRows() > 0) m_grid->DeleteRows(0, m_grid->GetNumberRows());

    m_is45GS02 = m_cpu && m_cpu->isaName() && strcmp(m_cpu->isaName(), "45GS02") == 0;

    if (m_cpu) {
        int count = m_cpu->regCount();
        m_grid->AppendRows(count);
        m_prevValues.assign(count, 0);

        wxColour fg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
        wxColour bg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
        for (int i = 0; i < count; ++i) {
            const auto* desc = m_cpu->regDescriptor(i);
            m_grid->SetCellValue(i, 0, desc->name);
            m_grid->SetCellTextColour(i, 0, fg);
            m_grid->SetCellBackgroundColour(i, 0, bg);
            m_prevValues[i] = m_cpu->regRead(i);
        }
    }
    RefreshValues();
}

void RegisterPane::RefreshValues() {
    if (!m_cpu) return;

    int count = m_cpu->regCount();
    for (int i = 0; i < count; ++i) {
        const auto* desc = m_cpu->regDescriptor(i);
        uint32_t val = m_cpu->regRead(i);

        std::stringstream ss;
        ss << std::hex << std::uppercase << std::setfill('0');
        if (desc->width == RegWidth::R32) ss << "$" << std::setw(8) << val;
        else if (desc->width == RegWidth::R16) ss << "$" << std::setw(4) << val;
        else ss << "$" << std::setw(2) << val;

        // For status registers with named flags, append the flag display.
        if ((desc->flags & REGFLAG_STATUS) && desc->flagNames) {
            ss << "  ";
            const char* fn = desc->flagNames;
            int nbits = (int)(desc->width == RegWidth::R8 ? 8 : 16);
            for (int b = 0; b < nbits; ++b) {
                char letter = fn[b] ? fn[b] : '?';
                if (letter == '-') {
                    ss << '-';
                } else {
                    bool set = (val >> (nbits - 1 - b)) & 1;
                    ss << (set ? letter : '.');
                }
            }
        }

        m_grid->SetCellValue(i, 1, ss.str());

        wxColour fg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
        wxColour bg;
        if (val != m_prevValues[i]) {
            wxColour winBg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
            bg = wxColour(
                (winBg.Red()   * 2 + 255) / 3,
                (winBg.Green() * 2 + 80)  / 3,
                (winBg.Blue()  * 2 + 80)  / 3);
            m_prevValues[i] = val;
        } else {
            bg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
        }
        m_grid->SetCellTextColour(i, 1, fg);
        m_grid->SetCellBackgroundColour(i, 1, bg);
    }

    rebuildAnnotations();
}

void RegisterPane::rebuildAnnotations() {
    if (!m_is45GS02) {
        m_annotationLabel->SetLabel("");
        return;
    }

    // 45GS02-specific annotations:
    // B register (idx 4) = base page
    // P register (idx 7) = flags (E flag = bit 5)
    // SP (idx 5)
    uint32_t b = m_cpu->regRead(4);
    uint32_t p = m_cpu->regRead(7);
    uint32_t sp = m_cpu->regRead(5);
    bool eFlag = (p & 0x20) != 0;

    std::stringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0');
    ss << "BP=$" << std::setw(2) << b << "00";
    ss << "  Stk=" << (eFlag ? "8bit" : "16bit");
    ss << " @$" << std::setw(4) << sp;

    m_annotationLabel->SetLabel(ss.str());
}
