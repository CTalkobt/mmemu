#pragma once

#include <wx/wx.h>
#include <wx/treectrl.h>
#include <wx/combobox.h>
#include <map>
#include <string>
#include <vector>
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_handler.h"

/**
 * A pane that displays detailed information about individual devices, and
 * a toggle button for each of the device's switches (DeviceInfo::switches:
 * pressing one calls IOHandler::setOption(key, "on" / "off")).
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
    /** Toggle buttons for the selected device's switches (rebuilt when they change). */
    void updateSwitches(IOHandler* handler);

    wxComboBox* m_deviceSelector;
    wxPanel* m_switchPanel;
    std::vector<std::string> m_switchKeys;   // the buttons' keys, in order
    std::string m_switchDevice;              // whose they are
    wxTreeCtrl* m_tree;
    wxScrolledWindow* m_bitmapWindow;
    MachineDescriptor* m_desc = nullptr;
    DeviceInfo m_currentInfo;

    // Map tree items to register indices for editing
    std::map<wxTreeItemId, int> m_regItemMap;
};
