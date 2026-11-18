#include "machine_selector.h"
#include "libcore/main/machines/machine_registry.h"
#include <algorithm>
#include <cctype>
#include <map>

namespace {

const char* const NO_VENDOR = "Other";

/** A machine's id on its tree item. */
class MachineItem : public wxTreeItemData {
public:
    explicit MachineItem(std::string id) : id(std::move(id)) {}
    std::string id;
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

}  // namespace

MachineSelectorDialog::MachineSelectorDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, "Select Machine", wxDefaultPosition, wxSize(600, 560),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto& reg = MachineRegistry::instance();
    std::vector<std::pair<std::string, std::string>> detailed;
    reg.enumerateDetailed(detailed);
    for (const auto& [id, description] : detailed) {
        const std::string vendor = reg.vendorOf(id);
        m_machines.push_back({id, description, vendor.empty() ? NO_VENDOR : vendor});
    }

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    m_filter = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    m_filter->SetHint("Filter (id, name or vendor)");
    sizer->Add(m_filter, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    m_tree = new wxTreeCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                            wxTR_DEFAULT_STYLE | wxTR_HIDE_ROOT | wxTR_SINGLE);
    sizer->Add(m_tree, 1, wxEXPAND | wxALL, 10);

    auto* btnSizer = new wxStdDialogButtonSizer();
    auto* okBtn = new wxButton(this, wxID_OK);
    auto* cancelBtn = new wxButton(this, wxID_CANCEL);
    btnSizer->AddButton(okBtn);
    btnSizer->AddButton(cancelBtn);
    btnSizer->Realize();
    okBtn->SetDefault();
    sizer->Add(btnSizer, 0, wxALIGN_RIGHT | wxALL, 10);
    SetSizer(sizer);

    Populate("");

    Bind(wxEVT_BUTTON, &MachineSelectorDialog::OnOK, this, wxID_OK);
    Bind(wxEVT_BUTTON, &MachineSelectorDialog::OnCancel, this, wxID_CANCEL);
    m_filter->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { Populate(m_filter->GetValue()); });
    m_filter->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent& e) { OnOK(e); });
    // Double-click / Enter: a machine is chosen, a vendor opens or closes
    m_tree->Bind(wxEVT_TREE_ITEM_ACTIVATED, [this](wxTreeEvent& e) {
        if (!IdOf(e.GetItem()).empty()) {
            wxCommandEvent ok;
            OnOK(ok);
        } else {
            m_tree->Toggle(e.GetItem());
        }
    });
    okBtn->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) { e.Enable(!IdOf(m_tree->GetSelection()).empty()); });
}

void MachineSelectorDialog::Populate(const wxString& filterText) {
    const std::string filter = lower(filterText.ToStdString());
    const std::string keep = IdOf(m_tree->GetSelection());

    // Vendors alphabetically ("Other" last), machines by name within each
    std::map<std::string, std::vector<const Entry*>> byVendor;
    for (const Entry& e : m_machines) {
        if (!filter.empty() && lower(e.id + " " + e.description + " " + e.vendor).find(filter) == std::string::npos)
            continue;
        byVendor[e.vendor].push_back(&e);
    }
    std::vector<std::string> vendors;
    for (const auto& v : byVendor)
        if (v.first != NO_VENDOR) vendors.push_back(v.first);
    std::sort(vendors.begin(), vendors.end(),
              [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    if (byVendor.count(NO_VENDOR)) vendors.push_back(NO_VENDOR);

    m_tree->Freeze();
    m_tree->DeleteAllItems();
    const wxTreeItemId root = m_tree->AddRoot("Machines");
    wxTreeItemId select, firstMachine;
    for (const std::string& vendor : vendors) {
        auto& list = byVendor[vendor];
        std::sort(list.begin(), list.end(), [](const Entry* a, const Entry* b) {
            const std::string& na = a->description.empty() ? a->id : a->description;
            const std::string& nb = b->description.empty() ? b->id : b->description;
            return lower(na) < lower(nb);
        });
        const wxTreeItemId node = m_tree->AppendItem(root, wxString::Format("%s (%zu)", vendor, list.size()));
        m_tree->SetItemBold(node);
        for (const Entry* e : list) {
            const std::string label = e->description.empty() ? e->id : e->description + "  [" + e->id + "]";
            const wxTreeItemId item = m_tree->AppendItem(node, label, -1, -1, new MachineItem(e->id));
            if (e->id == keep) select = item;
            if (!firstMachine.IsOk()) firstMachine = item;
        }
        if (!filter.empty()) m_tree->Expand(node);      // closed until the filter narrows the list
    }
    // Keep the selection; else, filtering, the first match (Enter takes it)
    if (!select.IsOk() && !filter.empty()) select = firstMachine;
    if (select.IsOk()) {
        m_tree->EnsureVisible(select);
        m_tree->SelectItem(select);
    }
    m_tree->Thaw();
}

std::string MachineSelectorDialog::IdOf(const wxTreeItemId& item) const {
    if (!item.IsOk()) return "";
    const auto* data = dynamic_cast<const MachineItem*>(m_tree->GetItemData(item));
    return data ? data->id : "";
}

void MachineSelectorDialog::OnOK(wxCommandEvent&) {
    const std::string id = IdOf(m_tree->GetSelection());
    if (id.empty()) return;
    m_selectedId = id;
    EndModal(wxID_OK);
}

void MachineSelectorDialog::OnCancel(wxCommandEvent&) {
    EndModal(wxID_CANCEL);
}
