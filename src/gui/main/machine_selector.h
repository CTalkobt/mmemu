#pragma once
#include <wx/wx.h>
#include <wx/treectrl.h>
#include <string>
#include <vector>

/**
 * Picks a machine: a tree with a node per vendor (each machine's
 * registered MachineDescriptor::vendor; machines without one under
 * "Other") and the vendor's machines, its own and compatible ones, under
 * it. The filter keeps the machines whose id, description or vendor
 * contains its text, their vendors opened.
 */
class MachineSelectorDialog : public wxDialog {
public:
    MachineSelectorDialog(wxWindow* parent);
    std::string GetSelectedMachineId() const { return m_selectedId; }

private:
    struct Entry {
        std::string id, description, vendor;
    };

    void OnOK(wxCommandEvent& event);
    void OnCancel(wxCommandEvent& event);
    /** Rebuild the tree for the filter's text. */
    void Populate(const wxString& filter);
    /** The machine id of tree item `item` ("" for a vendor node). */
    std::string IdOf(const wxTreeItemId& item) const;

    wxTextCtrl* m_filter = nullptr;
    wxTreeCtrl* m_tree = nullptr;
    std::string m_selectedId;
    std::vector<Entry> m_machines;
};
