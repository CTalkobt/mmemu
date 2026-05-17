#pragma once
#include <wx/wx.h>
#include <wx/grid.h>
#include "libcore/main/icore.h"
#include "libcore/main/machine_desc.h"

class IORegistry;
class IOHandler;
class IMapController;

/**
 * MEGA65 Status Pane — shows I/O personality mode and MAP state.
 * Only meaningful when a MEGA65 machine is loaded.
 */
class Mega65StatusPane : public wxPanel {
public:
    Mega65StatusPane(wxWindow* parent);

    void SetMachine(MachineDescriptor* machine, ICore* cpu);
    void RefreshValues();

private:
    MachineDescriptor* m_machine = nullptr;
    ICore* m_cpu = nullptr;
    IOHandler* m_keyHandler = nullptr;
    IMapController* m_mapCtrl = nullptr;

    wxStaticText* m_personalityLabel;
    wxGrid* m_mapGrid;
};
