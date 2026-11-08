#include "drive_status_pane.h"
#include <wx/filedlg.h>
#include <wx/statline.h>
#include "libdevices/main/io_registry.h"
#include "libcore/main/machine_desc.h"
#include <filesystem>

namespace fs = std::filesystem;

DriveStatusPane::DriveStatusPane(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    m_scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_scroll->SetScrollRate(0, 10);
    m_mainSizer = new wxBoxSizer(wxVERTICAL);
    m_scroll->SetSizer(m_mainSizer);
    sizer->Add(m_scroll, 1, wxEXPAND | wxALL, 5);
    SetSizer(sizer);
}

namespace {
bool s_apple2 = false;     // the Apple II's Disk II drives (units 1, 2)
bool s_bbc = false;        // the BBC Micro's 8271 drives (units 0, 1)

/** "Unit 8: IDLE T:18 S:00"; Atari drives (units 1-4) as "D1: IDLE S:0001";
 *  the Apple II's as "S6,D1: IDLE T:17"; the BBC's as "Drv 0: IDLE T:00". */
wxString unitLabel(int unit, const char* state, int t, int s) {
    if (s_apple2) return wxString::Format("S6,D%d: %s T:%02d", unit, state, t);
    if (s_bbc) return wxString::Format("Drv %d: %s T:%02d", unit, state, t);
    if (unit < 8) return wxString::Format("D%d: %s S:%04d", unit, state, s);
    return wxString::Format("Unit %d: %s T:%02d S:%02d", unit, state, t, s);
}
}  // namespace

void DriveStatusPane::SetMachine(MachineDescriptor* md) {
    m_md = md;
    s_apple2 = md && md->machineId.rfind("apple2", 0) == 0;
    s_bbc = md && md->machineId.rfind("bbc", 0) == 0;
    m_drives.clear();
    m_mainSizer->Clear(true);

    if (m_md && m_md->ioRegistry) {
        std::vector<IOHandler*> handlers;
        m_md->ioRegistry->enumerate(handlers);
        // BBC drive 0, Atari D1:-D4: (units 1-4) and the common Commodore drive units
        std::vector<int> units = {0, 1, 2, 3, 4, 8, 9, 10, 11};
        for (int unit : units) {
            for (auto* h : handlers) {
                int t, s;
                bool led;
                if (h->getDiskStatus(unit, t, s, led)) {
                    // This handler supports this unit
                    auto* driveSizer = new wxBoxSizer(wxHORIZONTAL);
                    auto* infoSizer = new wxBoxSizer(wxVERTICAL);
                    
                    auto* label = new wxStaticText(m_scroll, wxID_ANY, unitLabel(unit, "IDLE", t, s));
                    label->SetFont(wxFont(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
                    
                    auto* fileLabel = new wxStaticText(m_scroll, wxID_ANY, "Empty");
                    fileLabel->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_ITALIC, wxFONTWEIGHT_NORMAL));
                    fileLabel->SetForegroundColour(*wxBLACK);

                    infoSizer->Add(label, 0, wxEXPAND);
                    infoSizer->Add(fileLabel, 0, wxEXPAND);

                    auto* ledPanel = new wxPanel(m_scroll, wxID_ANY, wxDefaultPosition, wxSize(16, 16));
                    ledPanel->SetBackgroundColour(*wxLIGHT_GREY);
                    
                    auto* mountBtn = new wxButton(m_scroll, unit, "Mount...");
                    auto* ejectBtn = new wxButton(m_scroll, unit, "Eject");

                    driveSizer->Add(ledPanel, 0, wxALIGN_CENTER | wxALL, 5);
                    driveSizer->Add(infoSizer, 1, wxALIGN_CENTER | wxALL, 5);
                    driveSizer->Add(mountBtn, 0, wxALIGN_CENTER | wxALL, 5);
                    driveSizer->Add(ejectBtn, 0, wxALIGN_CENTER | wxALL, 5);
                    
                    m_mainSizer->Add(driveSizer, 0, wxEXPAND | wxALL, 5);
                    m_mainSizer->Add(new wxStaticLine(m_scroll, wxID_ANY), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
                    
                    m_drives.push_back({unit, h, label, fileLabel, ledPanel});

                    mountBtn->Bind(wxEVT_BUTTON, &DriveStatusPane::OnMount, this);
                    ejectBtn->Bind(wxEVT_BUTTON, &DriveStatusPane::OnEject, this);
                    break; 
                }
            }
        }
    }
    
    if (m_drives.empty()) {
        m_mainSizer->Add(new wxStaticText(m_scroll, wxID_ANY, "No drives found"), 0, wxALL, 10);
    }

    m_mainSizer->Layout();
    m_scroll->FitInside();
    RefreshStatus();
}

void DriveStatusPane::RefreshStatus() {
    for (auto& drive : m_drives) {
        int t, s;
        bool led;
        if (drive.handler->getDiskStatus(drive.unit, t, s, led)) {
            std::string path = drive.handler->getMountedDiskPath(drive.unit);
            bool isMounted = !path.empty();
            
            drive.label->SetLabel(unitLabel(drive.unit, led ? "BUSY" : (isMounted ? "IDLE" : "OFF "), t, s));
            
            if (isMounted) {
                drive.fileLabel->SetLabel(fs::path(path).filename().string());
                drive.fileLabel->SetForegroundColour(*wxBLUE);
            } else {
                drive.fileLabel->SetLabel("Empty");
                drive.fileLabel->SetForegroundColour(*wxLIGHT_GREY);
            }

            // LED: Red if busy, Green if mounted, Grey if empty
            if (led) {
                drive.led->SetBackgroundColour(*wxRED);
            } else if (isMounted) {
                drive.led->SetBackgroundColour(wxColour(0, 180, 0)); // Green
            } else {
                drive.led->SetBackgroundColour(*wxLIGHT_GREY);
            }
            drive.led->Refresh();
        }
    }
}

void DriveStatusPane::OnMount(wxCommandEvent& event) {
    int unit = event.GetId();
    wxFileDialog openFileDialog(this, "Mount Disk", "", "",
                       s_apple2 ? "Apple II disk images (*.dsk;*.do;*.po;*.nib;*.woz)|"
                                  "*.dsk;*.do;*.po;*.nib;*.woz;*.DSK;*.DO;*.PO;*.NIB;*.WOZ"
                       : s_bbc ? "BBC DFS disc images (*.ssd;*.dsd)|*.ssd;*.dsd;*.SSD;*.DSD"
                       : unit < 8 ? "Atari disk images (*.atr)|*.atr;*.ATR"
                                : "Disk images (*.d64;*.d71;*.d80;*.d81;*.d82;*.g64;*.imd;*.img;*.prg)|"
                                  "*.d64;*.d71;*.d80;*.d81;*.d82;*.g64;*.imd;*.img;*.prg;"
                                  "*.D64;*.D71;*.D80;*.D81;*.D82;*.G64;*.IMD;*.IMG;*.PRG",
                       wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (openFileDialog.ShowModal() == wxID_OK) {
        std::string path = openFileDialog.GetPath().ToStdString();
        for (auto& drive : m_drives) {
            if (drive.unit == unit) {
                if (drive.handler->mountDisk(unit, path)) {
                    RefreshStatus();
                } else {
                    wxMessageBox("Failed to mount disk image.", "Error", wxICON_ERROR);
                }
                break;
            }
        }
    }
}

void DriveStatusPane::OnEject(wxCommandEvent& event) {
    int unit = event.GetId();
    for (auto& drive : m_drives) {
        if (drive.unit == unit) {
            drive.handler->ejectDisk(unit);
            RefreshStatus();
            break;
        }
    }
}
