#include "test_harness.h"
#include "plugin_pane_manager.h"
#include "gui_utils.h"
#include <vector>
#include <string>

// We mock the creation and destruction of panes.
// Since we are in a headless test, we won't actually create wxWindow objects,
// but we'll use pointers as opaque handles to verify the manager's logic.

static int g_universal_created = 0;
static int g_universal_destroyed = 0;
static int g_c64_created = 0;
static int g_c64_destroyed = 0;

static void* createUniversal(void* parent, void* ctx) {
    g_universal_created++;
    return (void*)0x1000; // Mock handle
}

static void destroyUniversal(void* handle, void* ctx) {
    g_universal_destroyed++;
}

static void* createC64(void* parent, void* ctx) {
    g_c64_created++;
    return (void*)0x2000; // Mock handle
}

static void destroyC64(void* handle, void* ctx) {
    g_c64_destroyed++;
}

TEST_CASE(plugin_pane_manager_logic) {
    PluginPaneManager& manager = PluginPaneManager::instance();

    // Setup machine IDs for machine-specific pane
    const char* c64_ids[] = {"c64", nullptr};

    PluginPaneInfo universal;
    std::memset(&universal, 0, sizeof(universal));
    universal.paneId = "universal_pane";
    universal.machineIds = nullptr; // All machines
    universal.createPane = createUniversal;
    universal.destroyPane = destroyUniversal;
    universal.refreshPane = nullptr;
    universal.onMachineLoad = nullptr;
    universal.ctx = nullptr;

    PluginPaneInfo c64_only;
    std::memset(&c64_only, 0, sizeof(c64_only));
    c64_only.paneId = "c64_pane";
    c64_only.machineIds = c64_ids;
    c64_only.createPane = createC64;
    c64_only.destroyPane = destroyC64;
    c64_only.refreshPane = nullptr;
    c64_only.onMachineLoad = nullptr;
    c64_only.ctx = nullptr;

    manager.registerPane(&universal);
    manager.registerPane(&c64_only);

    // Initial state
    ASSERT(g_universal_created == 0);
    ASSERT(g_c64_created == 0);

    // Switch to VIC20 (universal should be created, c64 should not)
    // We pass a dummy parent pointer (0xDEADBEEF)
    manager.onMachineSwitch("vic20", (wxWindow*)0xDEADBEEF, nullptr, nullptr);
    ASSERT(g_universal_created == 1);
    ASSERT(g_c64_created == 0);
    ASSERT(manager.getPaneWindow("universal_pane") == (wxWindow*)0x1000);
    ASSERT(manager.getPaneWindow("c64_pane") == nullptr);

    // Switch to C64 (universal stays, c64 is created)
    manager.onMachineSwitch("c64", (wxWindow*)0xDEADBEEF, nullptr, nullptr);
    ASSERT(g_universal_created == 1); // Should not have been re-created
    ASSERT(g_c64_created == 1);
    ASSERT(manager.getPaneWindow("universal_pane") == (wxWindow*)0x1000);
    ASSERT(manager.getPaneWindow("c64_pane") == (wxWindow*)0x2000);

    // Switch back to VIC20 (universal stays, c64 is destroyed)
    manager.onMachineSwitch("vic20", (wxWindow*)0xDEADBEEF, nullptr, nullptr);
    ASSERT(g_universal_created == 1);
    ASSERT(g_c64_created == 1);
    ASSERT(g_c64_destroyed == 1);
    ASSERT(manager.getPaneWindow("c64_pane") == nullptr);
}

TEST_CASE(gui_keyboard_mapping) {
    ASSERT(wxKeyToVic20Name('A') == "a");
    ASSERT(wxKeyToVic20Name('z') == "z");
    ASSERT(wxKeyToVic20Name('0') == "0");
    ASSERT(wxKeyToVic20Name(WXK_SPACE) == "space");
    ASSERT(wxKeyToVic20Name(WXK_RETURN) == "return");
    ASSERT(wxKeyToVic20Name(WXK_F1) == "f1");
    ASSERT(wxKeyToVic20Name(WXK_NUMPAD5) == "5");
    ASSERT(wxKeyToVic20Name(0xFFFF) == ""); // Unknown key
}

// #146: a host (the dock layout) places plugin panes instead of a notebook.
// onAdd runs after a pane is created, onRemove before it is destroyed.
static std::vector<std::string> g_hostEvents;
static void* createHostPane(void*, void*) { g_hostEvents.push_back("create"); return (void*)0x3000; }
static void destroyHostPane(void*, void*) { g_hostEvents.push_back("destroy"); }

TEST_CASE(plugin_pane_manager_host_callbacks) {
    PluginPaneManager& manager = PluginPaneManager::instance();
    static const char* ids[] = {"host_test_machine", nullptr};
    PluginPaneInfo info;
    std::memset(&info, 0, sizeof(info));
    info.paneId = "host_test_pane";
    info.displayName = "Host Test";
    info.machineIds = ids;
    info.createPane = createHostPane;
    info.destroyPane = destroyHostPane;
    manager.registerPane(&info);

    g_hostEvents.clear();
    manager.setHost(
        [](const PluginPaneInfo& p, wxWindow* w) {
            if (std::string(p.paneId) == "host_test_pane" && w == (wxWindow*)0x3000) g_hostEvents.push_back("add");
        },
        [](const PluginPaneInfo& p, wxWindow* w) {
            if (std::string(p.paneId) == "host_test_pane" && w == (wxWindow*)0x3000) g_hostEvents.push_back("remove");
        });

    manager.onMachineSwitch("host_test_machine", (wxWindow*)0xDEADBEEF, nullptr, nullptr);
    manager.onMachineSwitch("some_other_machine", (wxWindow*)0xDEADBEEF, nullptr, nullptr);
    manager.setHost(nullptr, nullptr);

    std::vector<std::string> expected = {"create", "add", "remove", "destroy"};
    ASSERT(g_hostEvents == expected);
}

#include "dock_layout.h"

TEST_CASE(dock_layout_id_lists_round_trip) {
    using dock_layout_detail::joinIds;
    using dock_layout_detail::splitIds;
    std::vector<std::string> ids = {"console", "cpu", "breakpoints"};
    ASSERT(joinIds(ids) == "console,cpu,breakpoints");
    ASSERT(splitIds("console,cpu,breakpoints") == ids);
    ASSERT(splitIds("").empty());
    ASSERT(splitIds(",cpu,,") == std::vector<std::string>{"cpu"});
}
