#include <wx/wx.h>
#include <wx/filename.h>
#include "rom_discovery.h"
#include "rom_importer.h"
#include <vector>
#include <string>

using namespace mega65_importer;

class Mega65RomImportPane : public wxPanel {
public:
    Mega65RomImportPane(wxWindow* parent) : wxPanel(parent) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        sizer->Add(new wxStaticText(this, wxID_ANY, "MEGA65 ROM Discovery:"), 0, wxALL, 5);

        m_sourceList = new wxChoice(this, wxID_ANY);
        sizer->Add(m_sourceList, 0, wxEXPAND | wxALL, 5);

        m_fileDisplay = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
        sizer->Add(m_fileDisplay, 1, wxEXPAND | wxALL, 5);

        auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
        
        m_browseBtn = new wxButton(this, wxID_ANY, "Manual Selection...");
        btnSizer->Add(m_browseBtn, 0, wxALL, 5);

        m_refreshBtn = new wxButton(this, wxID_ANY, "Refresh Sources");
        btnSizer->Add(m_refreshBtn, 0, wxALL, 5);

        m_importBtn = new wxButton(this, wxID_ANY, "Import Selected");
        m_importBtn->Disable();
        btnSizer->Add(m_importBtn, 0, wxALL, 5);

        sizer->Add(btnSizer, 0, wxALIGN_RIGHT);

        m_statusText = new wxStaticText(this, wxID_ANY, "Ready.");
        sizer->Add(m_statusText, 0, wxALL | wxEXPAND, 5);

        SetSizer(sizer);

        Bind(wxEVT_CHOICE, &Mega65RomImportPane::OnSourceSelect, this, m_sourceList->GetId());
        Bind(wxEVT_BUTTON, &Mega65RomImportPane::OnImport, this, m_importBtn->GetId());
        Bind(wxEVT_BUTTON, &Mega65RomImportPane::OnRefresh, this, m_refreshBtn->GetId());
        Bind(wxEVT_BUTTON, &Mega65RomImportPane::OnBrowse, this, m_browseBtn->GetId());

        RefreshSources();
    }

private:
    void RefreshSources() {
        m_sources = discoverSources("mega65");
        m_sourceList->Clear();
        for (const auto& s : m_sources) {
            m_sourceList->Append(s.label);
        }
        if (!m_sources.empty()) {
            m_sourceList->SetSelection(0);
            UpdateFileDisplay();
        } else {
            m_fileDisplay->SetValue("No MEGA65 ROMs found in standard locations.\n"
                                    "Use 'Manual Selection...' to browse for a MEGA65.ROM file.");
            m_importBtn->Disable();
        }
    }

    void UpdateFileDisplay() {
        int sel = m_sourceList->GetSelection();
        if (sel == wxNOT_FOUND) return;

        std::string text = "Files to be copied from " + m_sources[sel].basePath + ":\n\n";
        auto specs = romFilesFor("mega65");
        for (const auto& spec : specs) {
            text += "  " + spec.srcRelPath + " -> roms/mega65/" + spec.destName + " (" + std::to_string(spec.expectedSize) + " bytes)\n";
        }
        m_fileDisplay->SetValue(text);
        m_importBtn->Enable();
    }

    void OnSourceSelect(wxCommandEvent& event) {
        UpdateFileDisplay();
    }

    void OnRefresh(wxCommandEvent& event) {
        RefreshSources();
    }

    void OnBrowse(wxCommandEvent& event) {
        wxFileDialog openFileDialog(this, "Select MEGA65.ROM", "", "",
                       "ROM files (*.rom;*.ROM)|*.rom;*.ROM", 
                       wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (openFileDialog.ShowModal() == wxID_OK) {
            wxFileName fn(openFileDialog.GetPath());
            std::string path = fn.GetPath().ToStdString();
            std::string file = fn.GetFullName().ToStdString();
            
            // Add as a new source
            m_sources.push_back({ "Manual: " + file, path });
            m_sourceList->Append("Manual: " + file);
            m_sourceList->SetSelection(m_sources.size() - 1);
            UpdateFileDisplay();
        }
    }

    void OnImport(wxCommandEvent& event) {
        int sel = m_sourceList->GetSelection();
        if (sel == wxNOT_FOUND) return;

        m_statusText->SetLabel("Importing...");
        m_importBtn->Disable();
        
        ImportResult res = importRoms("mega65", m_sources[sel].basePath, "roms/mega65", true);
        
        if (res.success) {
            m_statusText->SetLabel("Success! Please reset the machine.");
            wxMessageBox("MEGA65 ROM imported successfully. Please reset the machine to apply changes.", "Import Success", wxOK | wxICON_INFORMATION);
        } else {
            m_statusText->SetLabel("Error: " + res.message);
            wxMessageBox("Import failed: " + res.message, "Import Error", wxOK | wxICON_ERROR);
        }
        m_importBtn->Enable();
    }

    wxChoice* m_sourceList;
    wxTextCtrl* m_fileDisplay;
    wxButton* m_importBtn;
    wxButton* m_refreshBtn;
    wxButton* m_browseBtn;
    wxStaticText* m_statusText;
    std::vector<RomSource> m_sources;
};

extern "C" void* createMega65RomImportPane(void* parent, void* ctx) {
    (void)ctx;
    return new Mega65RomImportPane(static_cast<wxWindow*>(parent));
}
