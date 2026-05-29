#pragma once

#include <wx/wx.h>
#include <wx/grid.h>
#include <wx/combobox.h>
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_handler.h"
#include <map>

/**
 * A pane that displays decoded I/O registers for a selected device,
 * with bit-field breakdowns and change highlighting.
 */
class RegisterWatchPane : public wxPanel {
public:
    RegisterWatchPane(wxWindow* parent);

    void setMachine(MachineDescriptor* desc);
    void refreshValues();

private:
    void onDeviceSelected(wxCommandEvent& evt);
    void rebuildGrid();
    std::string decodeBitFields(const DeviceRegister& reg) const;

    wxComboBox* m_deviceSelector;
    wxGrid* m_grid;
    MachineDescriptor* m_desc = nullptr;

    // Track previous values for change highlighting
    std::map<uint32_t, uint8_t> m_prevValues;
    int m_changeAge = 0; // Frame counter for fade-out
};
