#include "plugin_pane_manager.h"
#include <iostream>

static PluginPaneManager* s_instance = nullptr;

PluginPaneManager& PluginPaneManager::instance() {
    if (!s_instance) s_instance = new PluginPaneManager();
    return *s_instance;
}

void PluginPaneManager::registerPane(const PluginPaneInfo* info) {
    if (!info || !info->paneId) return;
    m_registeredPanes.push_back(*info);
}

static bool isRelevant(const PluginPaneInfo& info, const std::string& machineId) {
    if (!info.machineIds) return true;
    for (int i = 0; info.machineIds[i]; ++i) {
        if (machineId == info.machineIds[i]) return true;
    }
    return false;
}

void PluginPaneManager::onMachineSwitch(const std::string& machineId, wxWindow* parent, wxAuiNotebook* notebook, MachineDescriptor* desc) {
    m_currentMachineId = machineId;
    m_notebook = notebook;

    // Destroy panes that are no longer relevant
    auto it = m_livePanes.begin();
    while (it != m_livePanes.end()) {
        if (!isRelevant(it->second.info, machineId)) {
            if (it->second.info.destroyPane && it->second.window) {
                it->second.info.destroyPane(it->second.window, it->second.info.ctx);
            }
            if (m_notebook) {
                for (size_t i = 0; i < m_notebook->GetPageCount(); ++i) {
                    if (m_notebook->GetPage(i) == it->second.window) {
                        m_notebook->DeletePage(i);
                        break;
                    }
                }
            }
            it = m_livePanes.erase(it);
        } else {
            ++it;
        }
    }

    // Create panes that are now relevant
    for (const auto& info : m_registeredPanes) {
        if (isRelevant(info, machineId) && m_livePanes.find(info.paneId) == m_livePanes.end()) {
            if (info.createPane && parent) {
                void* handle = info.createPane(parent, info.ctx);
                if (handle) {
                    wxWindow* win = static_cast<wxWindow*>(handle);
                    m_livePanes[info.paneId] = {info, win};
                    if (m_notebook && win) {
                        m_notebook->AddPage(win, info.displayName ? info.displayName : info.paneId);
                    }
                }
            }
        }
    }

    // Notify all live panes
    for (auto& pair : m_livePanes) {
        if (pair.second.info.onMachineLoad && pair.second.window) {
            pair.second.info.onMachineLoad(pair.second.window, desc, pair.second.info.ctx);
        }
    }
}

void PluginPaneManager::tickAll(uint64_t cycles) {
    for (auto& pair : m_livePanes) {
        if (pair.second.info.refreshPane && pair.second.window) {
            pair.second.info.refreshPane(pair.second.window, cycles, pair.second.info.ctx);
        }
    }
}

wxWindow* PluginPaneManager::getPaneWindow(const std::string& paneId) {
    auto it = m_livePanes.find(paneId);
    return (it != m_livePanes.end()) ? it->second.window : nullptr;
}

void PluginPaneManager::populateMenu(wxMenuBar* menuBar) {
}
