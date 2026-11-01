#pragma once

#include "mmemu_plugin_api.h"
#include "libcore/main/machine_desc.h"
#include <wx/wx.h>
#include <wx/aui/auibook.h>
#include <string>
#include <vector>
#include <map>
#include <functional>

/**
 * Singleton manager for plugin-provided GUI panes.
 */
class PluginPaneManager {
public:
    static PluginPaneManager& instance();

    /**
     * Registers a new pane info.
     */
    void registerPane(const PluginPaneInfo* info);

    /**
     * Called when a machine is loaded or switched.
     * desc may be nullptr if no machine descriptor is available.
     */
    void onMachineSwitch(const std::string& machineId, wxWindow* parent, wxAuiNotebook* notebook, MachineDescriptor* desc = nullptr);

    /**
     * Periodic refresh for all live panes.
     */
    void tickAll(uint64_t cycles);

    /**
     * Return the live window for a registered pane, or nullptr if not yet created.
     */
    wxWindow* getPaneWindow(const std::string& paneId);

    /**
     * Integration with host menus.
     */
    void populateMenu(wxMenuBar* menuBar);

    /**
     * Let the host place plugin panes (e.g. in a dock layout) instead of
     * adding them to the notebook passed to onMachineSwitch(). onAdd runs after
     * a pane is created; onRemove runs before it is destroyed.
     */
    using PaneHostFn = std::function<void(const PluginPaneInfo&, wxWindow*)>;
    void setHost(PaneHostFn onAdd, PaneHostFn onRemove) {
        m_hostAdd = std::move(onAdd);
        m_hostRemove = std::move(onRemove);
    }

private:
    PluginPaneManager() = default;

    struct LivePane {
        PluginPaneInfo info;
        wxWindow* window = nullptr;
    };

    std::vector<PluginPaneInfo> m_registeredPanes;
    std::map<std::string, LivePane> m_livePanes; // paneId -> LivePane
    std::string m_currentMachineId;
    wxAuiNotebook* m_notebook = nullptr;
    PaneHostFn m_hostAdd, m_hostRemove;
};
