#include <wx/wx.h>
#include <wx/listctrl.h>
#include "rom_discovery.h"
#include "rom_importer.h"
#include <vector>
#include <string>

using namespace vice_importer;

class RomImportPane : public wxPanel {
public:
    RomImportPane(wxWindow* parent) : wxPanel(parent) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        sizer->Add(new wxStaticText(this, wxID_ANY, "VICE Installation Sources:"), 0, wxALL, 5);

        m_sourceList = new wxChoice(this, wxID_ANY);
        sizer->Add(m_sourceList, 0, wxEXPAND | wxALL, 5);

        m_fileDisplay = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
        sizer->Add(m_fileDisplay, 1, wxEXPAND | wxALL, 5);

        auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
        m_importBtn = new wxButton(this, wxID_ANY, "Import ROMs");
        m_importBtn->Disable();
        btnSizer->Add(m_importBtn, 0, wxALL, 5);

        m_refreshBtn = new wxButton(this, wxID_ANY, "Refresh Sources");
        btnSizer->Add(m_refreshBtn, 0, wxALL, 5);

        sizer->Add(btnSizer, 0, wxALIGN_RIGHT);

        m_statusText = new wxStaticText(this, wxID_ANY, "Ready.");
        sizer->Add(m_statusText, 0, wxALL | wxEXPAND, 5);

        SetSizer(sizer);

        Bind(wxEVT_CHOICE, &RomImportPane::OnSourceSelect, this, m_sourceList->GetId());
        Bind(wxEVT_BUTTON, &RomImportPane::OnImport, this, m_importBtn->GetId());
        Bind(wxEVT_BUTTON, &RomImportPane::OnRefresh, this, m_refreshBtn->GetId());

        RefreshSources();
    }

private:
    void RefreshSources() {
        m_sources = discoverSources("vic20");
        m_sourceList->Clear();
        for (const auto& s : m_sources) {
            m_sourceList->Append(s.label);
        }
        if (!m_sources.empty()) {
            m_sourceList->SetSelection(0);
            UpdateFileDisplay();
        } else {
            m_fileDisplay->SetValue("No VICE installations found.");
            m_importBtn->Disable();
        }
    }

    void UpdateFileDisplay() {
        int sel = m_sourceList->GetSelection();
        if (sel == wxNOT_FOUND) return;

        std::string text = "Files to be copied from " + m_sources[sel].basePath + ":\n\n";
        auto specs = romFilesFor("vic20");
        for (const auto& spec : specs) {
            text += "  " + spec.srcRelPath + " -> roms/vic20/" + spec.destName + " (" + std::to_string(spec.expectedSize) + " bytes)\n";
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

    void OnImport(wxCommandEvent& event) {
        int sel = m_sourceList->GetSelection();
        if (sel == wxNOT_FOUND) return;

        m_statusText->SetLabel("Importing...");
        m_importBtn->Disable();
        
        ImportResult res = importRoms(m_sources[sel], "vic20", "roms/vic20", true);
        
        if (res.success) {
            m_statusText->SetLabel("Success! Please reset the machine.");
            wxMessageBox("ROMs imported successfully. Please reset the machine to apply changes.", "Import Success", wxOK | wxICON_INFORMATION);
        } else {
            m_statusText->SetLabel("Error: " + res.errorMessage);
            wxMessageBox("Import failed: " + res.errorMessage, "Import Error", wxOK | wxICON_ERROR);
        }
        m_importBtn->Enable();
    }

    wxChoice* m_sourceList;
    wxTextCtrl* m_fileDisplay;
    wxButton* m_importBtn;
    wxButton* m_refreshBtn;
    wxStaticText* m_statusText;
    std::vector<RomSource> m_sources;
};

extern "C" void* createRomImportPane(void* parent, void* ctx) {
    (void)ctx;
    return new RomImportPane(static_cast<wxWindow*>(parent));
}
