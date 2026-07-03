#pragma once

#include <wx/wx.h>
#include <wx/treectrl.h>
#include <wx/combobox.h>
#include <map>
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_handler.h"

/**
 * A pane that displays detailed information about individual devices.
 */
class DeviceInfoPane : public wxPanel {
public:
    DeviceInfoPane(wxWindow* parent);

    void setMachine(MachineDescriptor* desc);
    void refreshValues();

private:
    void onDeviceSelected(wxCommandEvent& evt);
    void onPaintBitmaps(wxPaintEvent& evt);
    void onTreeRightClick(wxTreeEvent& evt);
    void onTreeActivated(wxTreeEvent& evt);
    void editRegister(int regIndex);

    wxComboBox* m_deviceSelector;
    wxTreeCtrl* m_tree;
    wxScrolledWindow* m_bitmapWindow;
    MachineDescriptor* m_desc = nullptr;
    DeviceInfo m_currentInfo;

    // Map tree items to register indices for editing
    std::map<wxTreeItemId, int> m_regItemMap;
};
