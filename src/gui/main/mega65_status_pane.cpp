#include "mega65_status_pane.h"
#include "plugins/devices/map_mmu/main/map_mmu.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/io_handler.h"
#include "include/imap_controller.h"
#include <wx/settings.h>
#include <iomanip>
#include <sstream>
#include <cstring>

Mega65StatusPane::Mega65StatusPane(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    wxFont fixedFont(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);

    // I/O Personality section
    auto* persSizer = new wxBoxSizer(wxHORIZONTAL);
    persSizer->Add(new wxStaticText(this, wxID_ANY, "I/O Mode: "), 0, wxALIGN_CENTER_VERTICAL);
    m_personalityLabel = new wxStaticText(this, wxID_ANY, wxString::FromUTF8("\xe2\x80\x94"));
    m_personalityLabel->SetFont(fixedFont.Bold());
    persSizer->Add(m_personalityLabel, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(persSizer, 0, wxEXPAND | wxALL, 5);

    // MAP state grid
    sizer->Add(new wxStaticText(this, wxID_ANY, "MAP State (8x8KB blocks):"), 0, wxLEFT | wxTOP, 5);

    m_mapGrid = new wxGrid(this, wxID_ANY);
    m_mapGrid->CreateGrid(8, 3);
    m_mapGrid->SetColLabelValue(0, "Virtual");
    m_mapGrid->SetColLabelValue(1, "Physical");
    m_mapGrid->SetColLabelValue(2, "Offset");
    m_mapGrid->SetRowLabelSize(30);
    m_mapGrid->SetDefaultCellFont(fixedFont);
    m_mapGrid->DisableDragColSize();
    m_mapGrid->DisableDragRowSize();
    m_mapGrid->EnableEditing(false);

    for (int i = 0; i < 8; ++i) {
        m_mapGrid->SetRowLabelValue(i, wxString::Format("%d", i));
    }

    sizer->Add(m_mapGrid, 1, wxEXPAND | wxALL, 5);
    SetSizer(sizer);
}

void Mega65StatusPane::SetMachine(MachineDescriptor* machine, ICore* cpu) {
    m_machine = machine;
    m_cpu = cpu;
    m_keyHandler = nullptr;
    m_mapCtrl = nullptr;

    if (!m_machine || !m_cpu) return;

    // Find KEY register handler in IORegistry (by name, no RTTI needed)
    if (m_machine->ioRegistry) {
        m_keyHandler = m_machine->ioRegistry->findHandler("KEY");
    }

    // Get MapController from CPU
    m_mapCtrl = m_cpu->getMapMmu();

    RefreshValues();
}

void Mega65StatusPane::RefreshValues() {
    // I/O Personality — read via IOHandler dispatch ($D02F)
    if (m_keyHandler) {
        uint8_t val = 0;
        m_keyHandler->ioRead(nullptr, 0xD02F, &val);
        // The KEY register returns last-written byte; decode personality from known values
        // We use the IORegistry dispatch to read which also returns the state.
        // Actually we need the personality mode. Since we can't dynamic_cast to KeyRegister
        // without linking the plugin, read the register value and decode:
        // After knock: $00→C64, $96→C65, $53→MEGA65, $54→Ethernet
        switch (val) {
            case 0x53: m_personalityLabel->SetLabel("MEGA65"); break;
            case 0x96: m_personalityLabel->SetLabel("C65"); break;
            case 0x54: m_personalityLabel->SetLabel("Ethernet"); break;
            default:   m_personalityLabel->SetLabel("C64"); break;
        }
    } else {
        m_personalityLabel->SetLabel("N/A");
    }

    // MAP State
    wxColour enabledBg(200, 255, 200);
    wxColour disabledBg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);

    static const uint16_t blockBase[8] = {
        0x0000, 0x2000, 0x4000, 0x6000, 0x8000, 0xA000, 0xC000, 0xE000
    };

    if (m_mapCtrl) {
        const MapState& ms = m_mapCtrl->getMapState();
        for (int i = 0; i < 8; ++i) {
            bool enabled = (ms.enables & (1 << i)) != 0;
            uint16_t vBase = blockBase[i];
            uint16_t vEnd = vBase + 0x1FFF;

            std::stringstream ssVirt;
            ssVirt << std::hex << std::uppercase << std::setfill('0');
            ssVirt << "$" << std::setw(4) << vBase << "-$" << std::setw(4) << vEnd;
            m_mapGrid->SetCellValue(i, 0, ssVirt.str());

            if (enabled) {
                uint32_t offset = ms.offsets[i] & 0xFFFFF;
                uint32_t physBase = (offset << 8) | (vBase & 0x1FFF);
                uint32_t physEnd = (offset << 8) | (vEnd & 0x1FFF);

                std::stringstream ssPhys;
                ssPhys << std::hex << std::uppercase << std::setfill('0');
                ssPhys << "$" << std::setw(7) << physBase << "-$" << std::setw(7) << physEnd;
                m_mapGrid->SetCellValue(i, 1, ssPhys.str());

                std::stringstream ssOff;
                ssOff << std::hex << std::uppercase << std::setfill('0');
                ssOff << "$" << std::setw(5) << offset;
                m_mapGrid->SetCellValue(i, 2, ssOff.str());
            } else {
                m_mapGrid->SetCellValue(i, 1, "(passthrough)");
                std::stringstream ssDash;
                ssDash << "\xe2\x80\x94";
                m_mapGrid->SetCellValue(i, 2, wxString::FromUTF8(ssDash.str()));
            }

            wxColour bg = enabled ? enabledBg : disabledBg;
            for (int c = 0; c < 3; ++c)
                m_mapGrid->SetCellBackgroundColour(i, c, bg);
        }
    } else {
        for (int i = 0; i < 8; ++i) {
            uint16_t vBase = blockBase[i];
            uint16_t vEnd = vBase + 0x1FFF;
            std::stringstream ssVirt;
            ssVirt << std::hex << std::uppercase << std::setfill('0');
            ssVirt << "$" << std::setw(4) << vBase << "-$" << std::setw(4) << vEnd;
            m_mapGrid->SetCellValue(i, 0, ssVirt.str());
            m_mapGrid->SetCellValue(i, 1, "(no MMU)");
            m_mapGrid->SetCellValue(i, 2, wxString::FromUTF8("\xe2\x80\x94"));
            for (int c = 0; c < 3; ++c)
                m_mapGrid->SetCellBackgroundColour(i, c, disabledBg);
        }
    }

    m_mapGrid->AutoSizeColumns();
    m_mapGrid->ForceRefresh();
}
