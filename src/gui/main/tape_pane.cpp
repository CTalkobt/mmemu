#include "tape_pane.h"
#include <wx/filedlg.h>

TapePane::TapePane(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    m_statusLabel = new wxStaticText(this, wxID_ANY, "No tape mounted");
    sizer->Add(m_statusLabel, 0, wxALL | wxEXPAND, 10);

    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    m_mountBtn  = new wxButton(this, wxID_ANY, "Mount...");
    m_playBtn   = new wxButton(this, wxID_ANY, "Play");
    m_stopBtn   = new wxButton(this, wxID_ANY, "Stop");
    m_rewindBtn = new wxButton(this, wxID_ANY, "Rewind");
    m_recordBtn = new wxButton(this, wxID_ANY, "Record");
    m_saveBtn   = new wxButton(this, wxID_ANY, "Save...");

    btnSizer->Add(m_mountBtn,  0, wxALL, 5);
    btnSizer->Add(m_playBtn,   0, wxALL, 5);
    btnSizer->Add(m_stopBtn,   0, wxALL, 5);
    btnSizer->Add(m_rewindBtn, 0, wxALL, 5);
    btnSizer->Add(m_recordBtn, 0, wxALL, 5);
    btnSizer->Add(m_saveBtn,   0, wxALL, 5);

    sizer->Add(btnSizer, 0, wxCENTER);
    SetSizer(sizer);

    m_mountBtn->Bind(wxEVT_BUTTON,  &TapePane::OnMount,         this);
    m_playBtn->Bind(wxEVT_BUTTON,   &TapePane::OnPlay,          this);
    m_stopBtn->Bind(wxEVT_BUTTON,   &TapePane::OnStop,          this);
    m_rewindBtn->Bind(wxEVT_BUTTON, &TapePane::OnRewind,        this);
    m_recordBtn->Bind(wxEVT_BUTTON, &TapePane::OnRecord,        this);
    m_saveBtn->Bind(wxEVT_BUTTON,   &TapePane::OnSaveRecording, this);
}

void TapePane::SetTapeDevice(IOHandler* tape) {
    m_tape = tape;
    RefreshStatus();
}

void TapePane::RefreshStatus() {
    if (!m_tape) {
        m_statusLabel->SetLabel("No Datasette found");
        m_mountBtn->Enable(false);
        m_playBtn->Enable(false);
        m_stopBtn->Enable(false);
        m_rewindBtn->Enable(false);
        m_recordBtn->Enable(false);
        m_saveBtn->Enable(false);
        return;
    }
    m_mountBtn->Enable(true);
    m_playBtn->Enable(true);
    m_stopBtn->Enable(true);
    m_rewindBtn->Enable(true);
    m_recordBtn->Enable(true);
    m_saveBtn->Enable(true);
}

void TapePane::OnMount(wxCommandEvent&) {
    if (!m_tape) return;
    wxFileDialog openFileDialog(this, "Mount Tape", "", "",
                       "Tapes (*.tap;*.tzx;*.uef)|*.tap;*.tzx;*.uef;*.TAP;*.TZX;*.UEF;*.tzx.gz;*.tap.gz;*.zip|"
                       "Commodore tapes (*.tap)|*.tap;*.TAP|BBC Micro tapes (*.uef)|*.uef;*.UEF|"
                       "ZX Spectrum tapes (*.tap;*.tzx)|*.tap;*.tzx;*.TAP;*.TZX;*.tap.gz;*.tzx.gz;*.zip",
                       wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (openFileDialog.ShowModal() == wxID_OK) {
        std::string path = openFileDialog.GetPath().ToStdString();
        if (m_tape->mountTape(path)) {
            m_statusLabel->SetLabel("Tape mounted: " + openFileDialog.GetFilename().ToStdString());
        } else {
            wxMessageBox("Failed to mount tape.", "Error", wxICON_ERROR);
        }
    }
}

void TapePane::OnPlay(wxCommandEvent&) {
    if (m_tape) m_tape->controlTape("play");
}

void TapePane::OnStop(wxCommandEvent&) {
    if (m_tape) m_tape->controlTape("stop");
}

void TapePane::OnRewind(wxCommandEvent&) {
    if (m_tape) m_tape->controlTape("rewind");
}

void TapePane::OnRecord(wxCommandEvent&) {
    if (!m_tape) return;
    if (!m_tape->startTapeRecord()) {
        wxMessageBox("Could not start recording.\n(Write line not connected?)", "Error", wxICON_ERROR);
    } else {
        m_statusLabel->SetLabel("Recording...");
    }
}

void TapePane::OnSaveRecording(wxCommandEvent&) {
    if (!m_tape) return;
    m_tape->stopTapeRecord();
    wxFileDialog dlg(this, "Save Tape Recording", "", "",
                     "Commodore tapes (*.tap)|*.tap|BBC Micro tapes (*.uef)|*.uef|ZX Spectrum tapes (*.tap)|*.tap",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() == wxID_OK) {
        std::string path = dlg.GetPath().ToStdString();
        if (m_tape->saveTapeRecording(path)) {
            m_statusLabel->SetLabel("Saved: " + dlg.GetFilename().ToStdString());
        } else {
            wxMessageBox("Failed to save recording.", "Error", wxICON_ERROR);
        }
    }
}
