#include <chrono>
#include <wx/wx.h>
#include <wx/splitter.h>
#include <wx/notebook.h>
#include <wx/artprov.h>
#include <wx/dnd.h>
#include "machine_selector.h"
#include "register_pane.h"
#include "memory_pane.h"
#include "disasm_pane.h"
#include "console_pane.h"
#include "cartridge_pane.h"
#include "breakpoint_pane.h"
#include "stack_pane.h"
#include "machine_inspector_pane.h"
#include "device_info_pane.h"
#include "register_watch_pane.h"
#include "trace_pane.h"
#include "libdebug/main/debug_context.h"
#include "dialogs/assemble_dialog.h"
#include "dialogs/calculator_dialog.h"
#include "dialogs/tool_runner_dialog.h"
#include "libdevices/main/device_info.h"
#include "libdebug/main/expression_evaluator.h"
#include <unordered_map>
#include "plugin_loader/main/plugin_loader.h"
#include "plugin_pane_manager.h"
#include "ikeyboard_capture_pane.h"
#include "audio_output.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/machines/machine_registry.h"
#include "libcore/main/json_machine_loader.h"
#include "libcore/main/image_loader.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libtoolchain/main/toolchain_registry.h"
#include "gui_ids.h"
#include "screen_pane.h"
#include "tape_pane.h"
#include "symbol_pane.h"
#include "drive_status_pane.h"
#include "mega65_status_pane.h"
#include "include/util/logging.h"
#include "gui_utils.h"
#include "dock_layout.h"
#include "source_pane.h"
#include "libproject/main/project.h"
#include "libproject/main/make_runner.h"
#include "libproject/main/diagnostics.h"
#include "libproject/main/launcher.h"
#include <wx/listctrl.h>
#include <wx/dirdlg.h>
#include <atomic>
#include <thread>
#include "libdebug/main/source_stepper.h"
#include "libtoolchain/main/source_map.h"
#include <filesystem>
#include <wx/config.h>
#include <memory>
#include <map>
#include "libcore/main/machine_speed.h"
#include <fstream>
#include <vector>
#include <cstdio>

// mmemu - Multi Machine Emulator

// GUI run loop timing: the timer period and how much of it the emulation runs
static constexpr int kTimerMs    = 33;
static constexpr int kRunSliceMs = 28;

class MmemuFrame : public wxFrame {
public:
    MmemuFrame();
    ~MmemuFrame() override {
        m_timer.Stop(); stopBuildAndWait(); delete m_audio; delete m_dbg;
        if (!m_speedMenuShown) delete m_speedMenuItem;   // detached items are not deleted by wx
    }

    /** Open a Makefile project (#148); also used by --project. */
    bool openProject(const std::string& dir);

    /** Toggle keyboard-capture mode and sync all UI + app key handler. */
    void setKeyCapture(bool active);

private:
    void OnLoadMachine(wxCommandEvent& event);
    void OnStep(wxCommandEvent& event);
    void OnStepOver(wxCommandEvent& event);
    void OnStepOut(wxCommandEvent& event);
    void doStep(source_stepper::Mode mode);
    void refreshAfterStop();
    void updateSourceView();
    void OnOpenSource(wxCommandEvent& event);
    void startBuild(bool run);
    void onBuildLine(const std::string& line);
    void onBuildDone(project::MakeResult res, bool run, project::FileTimes before);
    void appendOutput(const std::string& text);
    void stopBuildAndWait();
    void OnLoadSourceMap(wxCommandEvent& event);
    void OnBackStep(wxCommandEvent& event);
    void OnRun(wxCommandEvent& event);
    void OnPause(wxCommandEvent& event);
    void OnReset(wxCommandEvent& event);
    void OnResetToMonitor(wxCommandEvent& event);
    void OnSpeed(wxCommandEvent& event);
    wxMenu*     m_controlMenu = nullptr;
    wxMenuItem* m_speedMenuItem = nullptr;   // owned by us while removed from the menu
    size_t      m_speedMenuPos = 0;
    bool        m_speedMenuShown = false;
    void OnSpeedUpdateUI(wxUpdateUIEvent& event);
    void OnFillMem(wxCommandEvent& event);
    void OnCopyMem(wxCommandEvent& event);
    void OnSwapMem(wxCommandEvent& event);
    void OnAssemble(wxCommandEvent& event);
    void OnGotoAddr(wxCommandEvent& event);
    void OnSearchMem(wxCommandEvent& event);
    void OnFindNext(wxCommandEvent& event);
    void OnFindPrior(wxCommandEvent& event);
    void OnLoadImage(wxCommandEvent& event);
    void OnAttachCart(wxCommandEvent& event);
    void OnEjectCart(wxCommandEvent& event);
    void OnKbdFocus(const wxCommandEvent& event);
    void OnViewPane(wxCommandEvent& event);
    void rebuildViewMenu();
    void syncViewMenu();
    void OnNewMemView(wxCommandEvent& event);
    void OnRenameMemView(wxCommandEvent& event);
    void OnLoadMachineDirect(wxCommandEvent& event);
    void loadMachineById(const std::string& id);
    void OnLoadImageDirect(wxCommandEvent& event);
    void OnTypeTextDirect(wxCommandEvent& event);
    void OnTimer(wxTimerEvent& event);
    void OnCtrlShiftK(wxKeyEvent& event);

    // Returns the currently-selected MemoryPane, or nullptr if none exist.
    [[nodiscard]] MemoryPane* activeMemPane() const {
        if (m_memPanes.empty()) return nullptr;
        int sel = m_memNotebook->GetSelection();
        if (sel == wxNOT_FOUND) return nullptr;
        return m_memPanes[sel];
    }
    // Adds a new named memory pane tab and wires the current bus.
    void addMemView(const wxString& label) {
        auto* pane = new MemoryPane(m_memNotebook);
        if (m_bus) pane->SetBus(m_bus);
        m_memPanes.push_back(pane);
        m_memNotebook->AddPage(pane, label, true);
    }

    MachineDescriptor* m_machine = nullptr;
    ICore* m_cpu = nullptr;
    IBus* m_bus = nullptr;
    IDisassembler* m_disasm = nullptr;
    DebugContext* m_dbg = nullptr;
    AudioOutput* m_audio = nullptr;

    RegisterPane*   m_regPane;
    wxAuiNotebook*  m_memNotebook = nullptr;
    std::vector<MemoryPane*> m_memPanes;
    DisasmPane*     m_disasmPane;
    ConsolePane*    m_consolePane;
    CartridgePane*  m_cartPane;
    BreakpointPane* m_bpPane    = nullptr;
    SymbolPane*     m_symPane   = nullptr;
    StackPane*      m_stackPane = nullptr;
    MachineInspectorPane* m_machineInspectorPane = nullptr;
    DeviceInfoPane* m_deviceInfoPane = nullptr;
    RegisterWatchPane* m_regWatchPane = nullptr;
    TracePane* m_tracePane = nullptr;
    Mega65StatusPane* m_mega65StatusPane = nullptr;
    SourcePane*     m_sourcePane = nullptr;

    // Makefile project (#148)
    std::unique_ptr<project::Config> m_project;
    wxTextCtrl*     m_outputPane = nullptr;
    wxListCtrl*     m_problemsPane = nullptr;
    std::vector<project::Diagnostic> m_problems;
    std::unique_ptr<project::DiagnosticParser> m_buildParser;
    std::thread     m_buildThread;
    std::atomic<bool> m_buildCancel{false};
    bool            m_building = false;

    // Dockable layout (#146) and the View menu that opens its panes
    std::unique_ptr<DockLayout> m_dock;
    wxMenu* m_viewMenu = nullptr;
    std::map<int, std::string> m_viewIds;     // View menu id -> pane id
    int m_nextPluginViewId = ID_VIEW_PANE_FIRST;

    wxMenuItem* m_resetToMonitorItem = nullptr;


    wxTimer m_timer;
    bool m_running = false;
    bool m_kbdFocus = false;
    IKeyboardCapturePane* m_capturePane = nullptr;

    // Search state
    std::vector<uint8_t> m_lastSearchPattern;
    uint32_t             m_lastSearchFoundAddr = 0xFFFFFFFF;
};

/**
 * Global event filter that intercepts keyboard events when capture is active.
 * wxEventFilter::FilterEvent is guaranteed to be called by the wxWidgets event
 * loop for every event before any window sees it — unlike wxApp::FilterEvent,
 * which is port-specific and unreliable on GTK.
 *
 * We intercept wxEVT_CHAR_HOOK (the first keyboard event fired, before
 * accelerators) for key press, and wxEVT_KEY_UP for key release.
 * Ctrl+Shift+K is explicitly passed through so the frame toggle still works.
 *
 * Shift suppression: the PET has dedicated unshifted keys for many symbols that
 * require Shift on a PC keyboard (e.g. PC Shift+1 = PET '!' key, no shift).
 * We buffer the shift key and only forward it to the emulator if the next key
 * press does NOT map to one of those dedicated PET keys.
 */
class KeyboardCaptureFilter : public wxEventFilter {
public:
    int FilterEvent(wxEvent& event) override {
        if (!m_handler) return Event_Skip;

        const wxEventType t = event.GetEventType();
        if (t != wxEVT_CHAR_HOOK && t != wxEVT_KEY_UP)
            return Event_Skip;

        auto& key = static_cast<wxKeyEvent&>(event);
        // Always pass Ctrl+Shift+K through so OnCtrlShiftK can handle it.
        if (key.GetKeyCode() == 'K' && key.ControlDown() && key.ShiftDown())
            return Event_Skip;

        bool down = (t == wxEVT_CHAR_HOOK);
        int  code = key.GetKeyCode();

        // --- Positional mode (MEGA65): Shift is a real modifier and keys keep
        //     their position, so Shift+2 gives '"' as on the MEGA65 keyboard ---
        if (m_positional) {
            if (code == WXK_SHIFT) {
                logKey("left_shift", down);
                (void)m_handler("left_shift", down);
                return Event_Processed;
            }
            std::string name = wxKeyToVic20Name(unshiftedUsKey(code));
            if (name.empty()) return Event_Skip;
            logKey(name, down);
            (void)m_handler(name, down);
            return Event_Processed;
        }

        // --- Shift key: buffer rather than forward immediately ---
        if (code == WXK_SHIFT) {
            if (down) {
                m_shiftPending = true;
            } else {
                // On release: always clear pending flag and send shift-up
                // (idempotent if shift was never forwarded).
                m_shiftPending = false;
                logKey("left_shift", false);
                (void) m_handler("left_shift", false);
            }
            return Event_Processed;
        }

        // --- Check if Shift+key maps to a PET dedicated (unshifted) key ---
        std::string dedicated;
        if (m_shiftPending || key.ShiftDown())
            dedicated = shiftedPetKey(code);

        if (!dedicated.empty()) {
            // Consume the buffered shift — the PET dedicated key needs no shift.
            m_shiftPending = false;
            logKey(dedicated, down);
            (void)m_handler(dedicated, down);
            return Event_Processed;
        }

        // --- Regular key: flush any pending shift first, then forward ---
        std::string name = wxKeyToVic20Name(code);
        if (!name.empty()) {
            if (m_shiftPending && down) {
                logKey("left_shift", true);
                (void)m_handler("left_shift", true);
                m_shiftPending = false;
            }
            logKey(name, down);
            (void)m_handler(name, down);
            return Event_Processed;
        }

        return Event_Skip;
    }

    std::function<bool(const std::string&, bool)> m_handler;
    std::shared_ptr<spdlog::logger> m_logger;
    bool m_positional = false;  // true: forward Shift + base key (no PET dedicated-key mapping)

private:
    bool m_shiftPending = false;

    void logKey(const std::string& name, bool down) const {
        if (m_logger) {
            char buf[80];
            snprintf(buf, sizeof(buf), "key%s: %s", down ? "Down" : "Up", name.c_str());
            m_logger->debug(buf);
        }
    }

    // Returns the PET dedicated-key name for a PC Shift+code combo,
    // or "" if the shift should be forwarded normally.
    //
    // GTK delivers the VIRTUAL key code (unshifted) for digit keys but may
    // deliver the COMPOSED Unicode character for punctuation keys.  Both the
    // base key codes (e.g. '\'' = 39) and the composed codes (e.g. '"' = 34)
    // are listed so the mapping works regardless of GTK behaviour.
    // GTK may deliver the shifted character; map it back to its US-layout base key.
    static int unshiftedUsKey(int code) {
        static const char shifted[] = "!@#$%^&*()_+\":?<>{}";
        static const char base[]    = "1234567890-=';/,.[]";
        if (const char* p = std::strchr(shifted, code); p && code)
            return base[p - shifted];
        return code;
    }

    static std::string shiftedPetKey(int code) {
        switch (code) {
            // Base key codes (GTK delivers unshifted virtual key)
            case '1':  return "exclaim";    // Shift+1  → !
            case '2':  return "at";         // Shift+2  → @ (PET dedicated key)
            case '3':  return "hash";       // Shift+3  → #
            case '4':  return "dollar";     // Shift+4  → $
            case '5':  return "percent";    // Shift+5  → %
            case '6':  return "uparrow";    // Shift+6  → ^ (PET ↑ char)
            case '7':  return "ampersand";  // Shift+7  → &
            case '8':  return "asterisk";   // Shift+8  → *
            case '9':  return "lparen";     // Shift+9  → (
            case '0':  return "rparen";     // Shift+0  → )
            case '=':  return "plus";       // Shift+=  → +
            case '-':  return "leftarrow";  // Shift+-  → _ (PET ← char)
            case '\'': return "dquote";     // Shift+'  → "
            case ';':  return "colon";      // Shift+;  → :
            case '/':  return "question";   // Shift+/  → ?
            case ',':  return "less";       // Shift+,  → <
            // Composed character codes (GTK may deliver the shifted character)
            case '!':  return "exclaim";
            case '@':  return "at";
            case '#':  return "hash";
            case '$':  return "dollar";
            case '%':  return "percent";
            case '^':  return "uparrow";
            case '&':  return "ampersand";
            case '*':  return "asterisk";
            case '(':  return "lparen";
            case ')':  return "rparen";
            case '+':  return "plus";
            case '_':  return "leftarrow";
            case '"':  return "dquote";     // Shift+' delivered as " (34)
            case ':':  return "colon";
            case '?':  return "question";
            case '<':  return "less";
            default:   return {};
        }
    }
};

class MmemuDropTarget : public wxFileDropTarget {
public:
    MmemuDropTarget(MmemuFrame* frame) : m_frame(frame) {}
    
    bool OnDropFiles(wxCoord x, wxCoord y, const wxArrayString& filenames) override {
        if (filenames.IsEmpty()) return false;
        std::string path = filenames[0].ToStdString();
        
        // Auto-detect type
        auto* loader = ImageLoaderRegistry::instance().findLoader(path);
        if (loader) {
            // It's a program/bin
            wxCommandEvent evt(wxEVT_MENU, ID_LOAD_IMAGE);
            evt.SetString(path);
            m_frame->GetEventHandler()->AddPendingEvent(evt);
            return true;
        }
        
        auto cart = ImageLoaderRegistry::instance().createCartridgeHandler(path);
        if (cart) {
            // It's a cartridge
            wxCommandEvent evt(wxEVT_MENU, ID_ATTACH_CART);
            evt.SetString(path);
            m_frame->GetEventHandler()->AddPendingEvent(evt);
            return true;
        }
        
        wxMessageBox("Unsupported file format.", "Error", wxICON_ERROR);
        return false;
    }

private:
    MmemuFrame* m_frame;
};

static void* createScreenPane(void* parentHandle, void* /*ctx*/) {
    auto* parent = static_cast<wxWindow*>(parentHandle);
    auto* pane = new ScreenPane(parent);
    return static_cast<void*>(pane);
}

static void onScreenMachineLoad(void* paneHandle, MachineDescriptor* desc, void* /*ctx*/) {
    auto* pane = static_cast<ScreenPane*>(paneHandle);
    IVideoOutput* vid = nullptr;
    if (desc && desc->ioRegistry) {
        std::vector<IOHandler*> handlers;
        desc->ioRegistry->enumerate(handlers);
        for (auto* h : handlers) {
            if ((vid = dynamic_cast<IVideoOutput*>(h))) break;
        }
    }
    pane->SetVideoOutput(vid);
}

static void refreshScreenPane(void* paneHandle, uint64_t /*cycles*/, void* /*ctx*/) {
    static_cast<ScreenPane*>(paneHandle)->RefreshFrame();
}

static void* createTapePane(void* parentHandle, void* /*ctx*/) {
    auto* parent = static_cast<wxWindow*>(parentHandle);
    auto* pane = new TapePane(parent);
    return static_cast<void*>(pane);
}

static void onTapeMachineLoad(void* paneHandle, MachineDescriptor* desc, void* /*ctx*/) {
    auto* pane = static_cast<TapePane*>(paneHandle);
    IOHandler* tape = desc && desc->ioRegistry ? desc->ioRegistry->findHandler("Tape") : nullptr;
    pane->SetTapeDevice(tape);
}

static void* createDrivePane(void* parentHandle, void* /*ctx*/) {
    auto* parent = static_cast<wxWindow*>(parentHandle);
    auto* pane = new DriveStatusPane(parent);
    return static_cast<void*>(pane);
}

static void onDriveMachineLoad(void* paneHandle, MachineDescriptor* desc, void* /*ctx*/) {
    auto* pane = static_cast<DriveStatusPane*>(paneHandle);
    pane->SetMachine(desc);
}

static void refreshDrivePane(void* paneHandle, uint64_t /*cycles*/, void* /*ctx*/) {
    static_cast<DriveStatusPane*>(paneHandle)->RefreshStatus();
}

static PluginPaneInfo s_builtInDrivePane = {
    "drive", "Drive Status", "Tools", nullptr,
    createDrivePane, nullptr, refreshDrivePane, onDriveMachineLoad, nullptr
};

static PluginPaneInfo s_builtInTapePane = {
    "tape", "Tape Control", "Tools", nullptr,
    createTapePane, nullptr, nullptr, onTapeMachineLoad, nullptr
};

static PluginPaneInfo s_builtInScreenPane = {
    "screen", "Screen", nullptr, nullptr,
    createScreenPane, nullptr, refreshScreenPane, onScreenMachineLoad, nullptr
};

#include <wx/cmdline.h>

static const wxCmdLineEntryDesc g_cmdLineDesc[] =
{
    { wxCMD_LINE_OPTION, "m", "machine", "Create a machine on startup", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_OPTION, "i", "mount",   "Mount a disk/tape/program image", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_OPTION, "t", "type",    "Type text into the machine", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_SWITCH, "v", "verbose", "Enable debug logging", wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_SWITCH, nullptr, "trace", "Enable trace logging (very verbose)", wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_SWITCH, nullptr, "run", "Auto-start the loaded program", wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_OPTION, "p", "project", "Open a Makefile project directory", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_SWITCH, "h", "help",    "Show this help", wxCMD_LINE_VAL_NONE, wxCMD_LINE_OPTION_HELP },
    { wxCMD_LINE_SWITCH, "?", "",        "Show this help", wxCMD_LINE_VAL_NONE, wxCMD_LINE_OPTION_HELP },
    { wxCMD_LINE_NONE, nullptr, nullptr, nullptr, wxCMD_LINE_VAL_NONE, 0 }
};

class MmemuApp : public wxApp {
public:
    void OnInitCmdLine(wxCmdLineParser& parser) override {
        parser.SetDesc(g_cmdLineDesc);
        parser.SetSwitchChars("-");
    }

    bool OnCmdLineParsed(wxCmdLineParser& parser) override {
        parser.Found("machine", &m_startMachine);
        parser.Found("mount", &m_startMount);
        parser.Found("project", &m_startProject);
        parser.Found("type", &m_startType);
        m_autoRun = parser.Found("run");
        m_trace = parser.Found("trace");
        m_verbose = parser.Found("verbose");
        return true;
    }

    bool OnInit() override {
        SetAppName("mmemu");   // wxConfig (saved window layout) lives in ~/.mmemu
        // wxApp::OnInit() drives OnInitCmdLine → parse → OnCmdLineParsed,
        // which populates m_startMachine etc.  Must be called first.
        if (!wxApp::OnInit())
            return false;

        LogRegistry::instance().init();
        // Also check for -vv in raw argv (wxWidgets can't parse combined short flags)
        for (int i = 1; i < argc; ++i) {
            wxString a(argv[i]);
            if (a == "-vv") m_trace = true;
        }
        if (m_trace)
            LogRegistry::instance().setGlobalLevel(spdlog::level::trace);
        else if (m_verbose)
            LogRegistry::instance().setGlobalLevel(spdlog::level::debug);
        m_keyFilter.m_logger = LogRegistry::instance().getLogger("gui.keyboard");
        wxEvtHandler::AddFilter(&m_keyFilter);
        PluginLoader::instance().setPaneRegisterFn([](const PluginPaneInfo* info) {
            PluginPaneManager::instance().registerPane(info);
        });
        PluginPaneManager::instance().registerPane(&s_builtInScreenPane);
        PluginPaneManager::instance().registerPane(&s_builtInTapePane);
        PluginPaneManager::instance().registerPane(&s_builtInDrivePane);
        PluginLoader::instance().loadFromStandardLocations();
        // JSON-defined machines, after the plugins they use (as the CLI and MCP do)
        {
            JsonMachineLoader jsonLoader;
            jsonLoader.loadFile("machines/rawMega65.json");
        }
        auto *frame = new MmemuFrame();
        frame->Show(true);

        if (!m_startMachine.empty()) {
            wxCommandEvent evt(wxEVT_MENU, ID_LOAD_MACHINE_DIRECT);
            evt.SetString(m_startMachine);
            frame->GetEventHandler()->AddPendingEvent(evt);
        }
        if (!m_startMount.empty()) {
            wxCommandEvent evt(wxEVT_MENU, ID_LOAD_IMAGE_DIRECT);
            evt.SetString(m_startMount);
            frame->GetEventHandler()->AddPendingEvent(evt);
        }
        if (!m_startType.empty()) {
            wxCommandEvent evt(wxEVT_MENU, ID_TYPE_TEXT_DIRECT);
            evt.SetString(m_startType);
            frame->GetEventHandler()->AddPendingEvent(evt);
        }
        if (m_autoRun) {
            wxCommandEvent evt(wxEVT_MENU, ID_RUN);
            frame->GetEventHandler()->AddPendingEvent(evt);
        }
        if (!m_startProject.empty()) {
            std::string dir = m_startProject.utf8_string();
            frame->CallAfter([frame, dir]() { frame->openProject(dir); });
        }

        return true;
    }

    int OnExit() override {
        wxEvtHandler::RemoveFilter(&m_keyFilter);
        return wxApp::OnExit();
    }

    KeyboardCaptureFilter m_keyFilter;
    wxString m_startMachine;
    wxString m_startProject;
    wxString m_startMount;
    wxString m_startType;
    bool     m_autoRun = false;
    bool     m_verbose = false;
    bool     m_trace   = false;
};

wxIMPLEMENT_APP(MmemuApp);

MmemuFrame::MmemuFrame()
    : wxFrame(nullptr, wxID_ANY, "mmemu - Multi Machine Emulator",
              wxDefaultPosition, wxSize(1280, 860)),
      m_timer(this, ID_GUI_TIMER)
{
    // Menu
    auto* menuFile = new wxMenu;
    menuFile->Append(ID_LOAD_MACHINE, "Load Machine...\tCtrl-L");
    menuFile->AppendSeparator();
    menuFile->Append(ID_OPEN_SOURCE, "Open Source...\tCtrl-O");
    menuFile->Append(ID_SAVE_SOURCE, "Save Source\tCtrl-S");
    menuFile->AppendSeparator();
    menuFile->Append(wxID_EXIT);
    
    auto* menuControl = new wxMenu;
    menuControl->Append(ID_STEP, "Step Into\tF11", "Next source line (into subroutines), or one instruction without a source map");
    menuControl->Append(ID_STEP_OVER, "Step Over\tF10", "Next source line at this level (runs subroutines), or one instruction");
    menuControl->Append(ID_STEP_OUT, "Step Out\tCtrl-F11", "Run until the current subroutine returns");
    menuControl->Append(ID_BACKSTEP, "Back Step\tShift-F11");
    menuControl->Append(ID_RUN, "Run\tF5");
    menuControl->Append(ID_PAUSE, "Pause\tF6");
    menuControl->AppendSeparator();
    menuControl->Append(ID_RESET, "Reset\tCtrl-R");
    m_resetToMonitorItem = menuControl->Append(ID_RESET_TO_MONITOR, "Reset to Monitor");
    m_resetToMonitorItem->Enable(false);  // Hidden until MEGA65 is loaded
    // Speed: CPU clock, or max = warp (devices advance one cycle per CPU cycle).
    // Enabled and checked by UPDATE_UI from the machine's current speed.
    auto* menuSpeed = new wxMenu;
    menuSpeed->AppendRadioItem(ID_SPEED_1,   "1 MHz");
    menuSpeed->AppendRadioItem(ID_SPEED_2,   "2 MHz");
    menuSpeed->AppendRadioItem(ID_SPEED_3_5, "3.5 MHz");
    menuSpeed->AppendRadioItem(ID_SPEED_40,  "40.5 MHz");
    menuSpeed->AppendRadioItem(ID_SPEED_MAX, "Max (unbounded)");
    // Only shown for machines with a speed model (MEGA65): inserted/removed on machine load
    m_controlMenu = menuControl;
    m_speedMenuPos = menuControl->GetMenuItemCount();
    m_speedMenuItem = menuControl->AppendSubMenu(menuSpeed, "Speed",
        "CPU clock with real-time devices, or Max: devices advance one cycle per CPU cycle");
    menuControl->Remove(m_speedMenuItem);
    menuControl->AppendSeparator();
    menuControl->Append(ID_LOAD_IMAGE, "Load Image...\tCtrl-I");
    menuControl->Append(ID_ATTACH_CART, "Attach Cartridge...");
    menuControl->Append(ID_EJECT_CART, "Eject Cartridge");

    auto* menuDebug = new wxMenu;
    menuDebug->Append(ID_ASSEMBLE, "Assemble...\tCtrl-A");
    menuDebug->Append(ID_LOAD_SOURCE_MAP, "Load Source Map...", "Load a ca45 --source-map file for source-level debugging");
    menuDebug->AppendSeparator();
    menuDebug->Append(ID_GOTO_ADDR, "Go to Address...\tCtrl-G");
    menuDebug->Append(ID_SEARCH_MEM, "Search Memory...\tCtrl-F");
    menuDebug->Append(ID_FIND_NEXT,  "Find Next\tF3");
    menuDebug->Append(ID_FIND_PRIOR, "Find Prior\tShift-F3");
    menuDebug->AppendSeparator();
    menuDebug->Append(ID_FILL_MEM, "Fill Memory...");
    menuDebug->Append(ID_COPY_MEM, "Copy Memory...");
    menuDebug->Append(ID_SWAP_MEM, "Swap Memory...");

    // View: every pane, checked when open (filled by rebuildViewMenu())
    m_viewMenu = new wxMenu;

    auto* menuTools = new wxMenu;
    menuTools->Append(ID_CALCULATOR, "Calculator\tCtrl-Shift-C");
    menuTools->AppendSeparator();
    menuTools->Append(ID_TOOL_PROFILE, "Profile CPU...");
    menuTools->Append(ID_TOOL_MEASURE, "Measure Region...");
    menuTools->Append(ID_TOOL_RUNTO,   "Run Until...");
    menuTools->Append(ID_TOOL_DEVINFO, "Device Info...");
    menuTools->AppendSeparator();
    menuTools->Append(ID_TOOL_HEATMAP, "Memory Heat Map...");
    menuTools->Append(ID_TOOL_RASTER,  "Raster Position...");

    auto* menuBar = new wxMenuBar;
    menuBar->Append(menuFile, "&File");
    menuBar->Append(m_viewMenu, "&View");
    auto* menuProject = new wxMenu;
    menuProject->Append(ID_PROJECT_OPEN,  "Open Project...\tCtrl-Shift-O", "Open a directory with a Makefile");
    menuProject->Append(ID_PROJECT_BUILD, "Build\tCtrl-B", "make <build target> (default: all)");
    menuProject->Append(ID_PROJECT_RUN,   "Run Project\tCtrl-F5", "make <run target>, then load and start the new .prg");
    menuProject->Append(ID_PROJECT_STOP,  "Stop Build");
    menuProject->AppendSeparator();
    menuProject->Append(ID_PROJECT_CLOSE, "Close Project");
    menuBar->Append(menuProject, "&Project");
    menuBar->Append(menuControl, "&Control");
    menuBar->Append(menuDebug, "&Debug");
    menuBar->Append(menuTools, "&Tools");
    SetMenuBar(menuBar);
    
    // ToolBar
    auto* toolBar = CreateToolBar();
    toolBar->AddTool(ID_LOAD_MACHINE, "Machine", wxArtProvider::GetBitmap(wxART_NEW));
    toolBar->AddSeparator();
    toolBar->AddTool(ID_BACKSTEP, "Back Step", wxArtProvider::GetBitmap(wxART_GO_BACK));
    toolBar->AddTool(ID_STEP, "Step", wxArtProvider::GetBitmap(wxART_GO_FORWARD));
    toolBar->AddTool(ID_RUN, "Run", wxArtProvider::GetBitmap(wxART_GO_FORWARD));
    toolBar->AddTool(ID_PAUSE, "Pause", wxArtProvider::GetBitmap(wxART_QUIT));
    toolBar->AddSeparator();
    toolBar->AddTool(ID_GOTO_ADDR, "Go to", wxArtProvider::GetBitmap(wxART_FIND));
    toolBar->AddSeparator();
    toolBar->AddCheckTool(ID_KBD_FOCUS, "Keyboard Focus (Ctrl-Shift-K)", wxArtProvider::GetBitmap(wxART_LIST_VIEW));
    toolBar->Realize();
    
    // Status Bar
    wxFrameBase::CreateStatusBar();
    
    // Dockable layout (#146): the machine screen in the centre, tool panes as
    // tabs in the bottom and side docks. Only Console, CPU and Breakpoints are
    // open by default; everything else is on the View menu.
    m_dock = std::make_unique<DockLayout>(this);
    wxWindow* host = m_dock->bottomBook();

    // CPU: registers beside the disassembly
    auto* cpuPane = new wxSplitterWindow(host, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE);
    m_regPane = new RegisterPane(cpuPane);
    m_disasmPane = new DisasmPane(cpuPane);
    cpuPane->SplitVertically(m_regPane, m_disasmPane, 220);
    cpuPane->SetMinimumPaneSize(120);

    m_consolePane = new ConsolePane(host);

    m_memNotebook = new wxAuiNotebook(host, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxAUI_NB_TOP | wxAUI_NB_SCROLL_BUTTONS | wxAUI_NB_CLOSE_ON_ALL_TABS | wxAUI_NB_TAB_MOVE);
    addMemView("Memory 1");
    m_memNotebook->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, [this](wxAuiNotebookEvent& event) {
        if (m_memPanes.size() <= 1) { event.Veto(); return; }
        int page = event.GetSelection();
        if (page != wxNOT_FOUND && page < (int)m_memPanes.size())
            m_memPanes.erase(m_memPanes.begin() + page);
    });

    m_bpPane = new BreakpointPane(host);
    m_symPane = new SymbolPane(host);
    m_stackPane = new StackPane(host);
    m_tracePane = new TracePane(host);
    m_machineInspectorPane = new MachineInspectorPane(host);
    m_deviceInfoPane = new DeviceInfoPane(host);
    m_regWatchPane = new RegisterWatchPane(host);
    m_cartPane = new CartridgePane(host);
    m_mega65StatusPane = new Mega65StatusPane(host);
    m_sourcePane = new SourcePane(host);
    m_outputPane = new wxTextCtrl(host, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP | wxTE_RICH2);
    m_outputPane->SetFont(wxFont(wxFontInfo(9).Family(wxFONTFAMILY_TELETYPE)));
    m_problemsPane = new wxListCtrl(host, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
    m_problemsPane->AppendColumn("", wxLIST_FORMAT_LEFT, 70);
    m_problemsPane->AppendColumn("File", wxLIST_FORMAT_LEFT, 160);
    m_problemsPane->AppendColumn("Line", wxLIST_FORMAT_RIGHT, 50);
    m_problemsPane->AppendColumn("Message", wxLIST_FORMAT_LEFT, 600);
    m_problemsPane->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& e) {
        long idx = (long)e.GetData();
        if (idx < 0 || idx >= (long)m_problems.size()) return;
        const auto& d = m_problems[idx];
        if (d.file.empty()) { SetStatusText("The tool did not say which file this is in."); return; }
        if (m_sourcePane->ShowLine(d.file, d.line)) m_dock->open("source");
    });
    m_sourcePane->SetStatusCallback([this](const wxString& msg) { SetStatusText(msg); });
    m_sourcePane->SetBreakpointsChangedCallback([this]() { if (m_bpPane) m_bpPane->RefreshValues(); });

    using Area = DockLayout::Area;
    m_dock->addPane("console",     "Console",          m_consolePane,          Area::Bottom, true);
    m_dock->addPane("cpu",         "CPU",              cpuPane,                Area::Bottom, true);
    m_dock->addPane("breakpoints", "Breakpoints",      m_bpPane,               Area::Bottom, true);
    m_dock->addPane("memory",      "Memory",           m_memNotebook,          Area::Bottom, false);
    m_dock->addPane("stack",       "Stack",            m_stackPane,            Area::Bottom, false);
    m_dock->addPane("trace",       "Trace",            m_tracePane,            Area::Bottom, false);
    m_dock->addPane("symbols",     "Symbols",          m_symPane,              Area::Side,   false);
    m_dock->addPane("machine",     "Machine Explorer", m_machineInspectorPane, Area::Side,   false);
    m_dock->addPane("devices",     "Devices",          m_deviceInfoPane,       Area::Side,   false);
    m_dock->addPane("iowatch",     "I/O Watch",        m_regWatchPane,         Area::Side,   false);
    m_dock->addPane("cartridge",   "Cartridge",        m_cartPane,             Area::Side,   false);
    m_dock->addPane("mega65",      "MEGA65 Status",    m_mega65StatusPane,     Area::Side,   false);
    m_dock->addPane("source",      "Source",           m_sourcePane,           Area::Side,   false);
    m_dock->addPane("output",      "Output",           m_outputPane,           Area::Bottom, false);
    m_dock->addPane("problems",    "Problems",         m_problemsPane,         Area::Bottom, false);
    m_dock->setAvailable("mega65", false);   // until a MEGA65 is loaded

    // Plugin panes: the screen goes in the centre, the rest onto the View menu
    PluginPaneManager::instance().setHost(
        [this](const PluginPaneInfo& info, wxWindow* win) {
            if (std::string(info.paneId) == "screen") { m_dock->setCenter(win); return; }
            m_dock->addPane(info.paneId, info.displayName ? info.displayName : info.paneId,
                            win, DockLayout::Area::Bottom, false);
            rebuildViewMenu();
        },
        [this](const PluginPaneInfo& info, wxWindow*) {
            if (std::string(info.paneId) == "screen") { m_dock->setCenter(nullptr); return; }
            m_dock->removePane(info.paneId);
            rebuildViewMenu();
        });

    m_dock->setOnChange([this]() { syncViewMenu(); });
    // Pick up source files edited in another program when the window is reactivated
    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& e) {
        if (e.GetActive() && m_sourcePane) m_sourcePane->ReloadChangedFiles();
        e.Skip();
    });
    m_dock->restore();
    rebuildViewMenu();
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
        if (m_sourcePane && m_sourcePane->HasUnsavedChanges() && e.CanVeto()) {
            int answer = wxMessageBox("Save changes to modified source files?", "Quit",
                                      wxYES_NO | wxCANCEL | wxICON_QUESTION, this);
            if (answer == wxCANCEL || (answer == wxYES && !m_sourcePane->SaveAll())) { e.Veto(); return; }
        }
        stopBuildAndWait();
        if (m_dock) m_dock->save();
        e.Skip();
    });
    
    SetDropTarget(new MmemuDropTarget(this));
    
    // Bind events
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(true); }, wxID_EXIT);
    Bind(wxEVT_MENU, &MmemuFrame::OnLoadMachine, this, ID_LOAD_MACHINE);
    Bind(wxEVT_TOOL, &MmemuFrame::OnLoadMachine, this, ID_LOAD_MACHINE);
    Bind(wxEVT_MENU, &MmemuFrame::OnStep, this, ID_STEP);
    Bind(wxEVT_MENU, &MmemuFrame::OnStepOver, this, ID_STEP_OVER);
    Bind(wxEVT_MENU, &MmemuFrame::OnStepOut, this, ID_STEP_OUT);
    Bind(wxEVT_MENU, &MmemuFrame::OnOpenSource, this, ID_OPEN_SOURCE);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (m_sourcePane) m_sourcePane->SaveCurrent(); }, ID_SAVE_SOURCE);
    Bind(wxEVT_MENU, &MmemuFrame::OnLoadSourceMap, this, ID_LOAD_SOURCE_MAP);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        wxDirDialog dlg(this, "Open Project (directory with a Makefile)", "", wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) openProject(dlg.GetPath().utf8_string());
    }, ID_PROJECT_OPEN);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { startBuild(false); }, ID_PROJECT_BUILD);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { startBuild(true); }, ID_PROJECT_RUN);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_buildCancel = true; }, ID_PROJECT_STOP);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        stopBuildAndWait();
        m_project.reset();
        SetStatusText("Project closed.");
    }, ID_PROJECT_CLOSE);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) {
        switch (e.GetId()) {
            case ID_PROJECT_BUILD: case ID_PROJECT_RUN: e.Enable(m_project && !m_building); break;
            case ID_PROJECT_STOP:  e.Enable(m_building); break;
            case ID_PROJECT_CLOSE: e.Enable(m_project != nullptr); break;
            default: break;
        }
    }, ID_PROJECT_OPEN, ID_PROJECT_STOP);
    Bind(wxEVT_TOOL, &MmemuFrame::OnStep, this, ID_STEP);
    Bind(wxEVT_MENU, &MmemuFrame::OnBackStep, this, ID_BACKSTEP);
    Bind(wxEVT_TOOL, &MmemuFrame::OnBackStep, this, ID_BACKSTEP);
    Bind(wxEVT_MENU, &MmemuFrame::OnRun, this, ID_RUN);
    Bind(wxEVT_TOOL, &MmemuFrame::OnRun, this, ID_RUN);
    Bind(wxEVT_MENU, &MmemuFrame::OnPause, this, ID_PAUSE);
    Bind(wxEVT_TOOL, &MmemuFrame::OnPause, this, ID_PAUSE);
    Bind(wxEVT_MENU, &MmemuFrame::OnReset, this, ID_RESET);
    Bind(wxEVT_MENU, &MmemuFrame::OnResetToMonitor, this, ID_RESET_TO_MONITOR);
    Bind(wxEVT_MENU, &MmemuFrame::OnSpeed, this, ID_SPEED_1, ID_SPEED_MAX);
    Bind(wxEVT_UPDATE_UI, &MmemuFrame::OnSpeedUpdateUI, this, ID_SPEED_1, ID_SPEED_MAX);
    Bind(wxEVT_MENU, &MmemuFrame::OnFillMem, this, ID_FILL_MEM);
    Bind(wxEVT_MENU, &MmemuFrame::OnCopyMem, this, ID_COPY_MEM);
    Bind(wxEVT_MENU, &MmemuFrame::OnSwapMem, this, ID_SWAP_MEM);
    Bind(wxEVT_MENU, &MmemuFrame::OnAssemble, this, ID_ASSEMBLE);
    Bind(wxEVT_MENU, &MmemuFrame::OnGotoAddr, this, ID_GOTO_ADDR);
    Bind(wxEVT_TOOL, &MmemuFrame::OnGotoAddr, this, ID_GOTO_ADDR);
    Bind(wxEVT_MENU, &MmemuFrame::OnSearchMem, this, ID_SEARCH_MEM);
    Bind(wxEVT_MENU, &MmemuFrame::OnFindNext,  this, ID_FIND_NEXT);
    Bind(wxEVT_MENU, &MmemuFrame::OnFindPrior, this, ID_FIND_PRIOR);
    Bind(wxEVT_MENU, &MmemuFrame::OnLoadImage, this, ID_LOAD_IMAGE);
    Bind(wxEVT_MENU, &MmemuFrame::OnAttachCart, this, ID_ATTACH_CART);
    Bind(wxEVT_MENU, &MmemuFrame::OnEjectCart, this, ID_EJECT_CART);
    Bind(wxEVT_MENU, &MmemuFrame::OnKbdFocus,   this, ID_KBD_FOCUS);
    Bind(wxEVT_MENU, &MmemuFrame::OnViewPane, this, ID_SHOW_BP_PANE, ID_SHOW_MEGA65_PANE);
    Bind(wxEVT_MENU, &MmemuFrame::OnViewPane, this, ID_VIEW_PANE_FIRST, ID_VIEW_PANE_LAST);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_dock->resetToDefaults(); }, ID_VIEW_RESET_LAYOUT);
    Bind(wxEVT_MENU, &MmemuFrame::OnNewMemView,    this, ID_NEW_MEM_VIEW);
    Bind(wxEVT_MENU, &MmemuFrame::OnRenameMemView, this, ID_RENAME_MEM_VIEW);
    Bind(wxEVT_MENU, [this](wxCommandEvent&){ CalculatorDialog(this, m_dbg).ShowModal(); }, ID_CALCULATOR);

    // Tool menu bindings (#82)
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (!m_cpu) return;
        std::vector<ToolField> fields = {
            {"steps", "Steps", "integer", "1000000", "Number of CPU steps to profile"},
            {"top",   "Top N", "integer", "20",      "Number of top hotspots to show"}
        };
        ToolRunnerDialog dlg(this, "Profile CPU", fields, [this](const auto& args) -> ToolResult {
            int steps = 1000000, top = 20;
            try { steps = std::stoi(args.at("steps")); } catch (...) {}
            try { top = std::stoi(args.at("top")); } catch (...) {}

            std::unordered_map<uint32_t, int> histogram;
            uint64_t startCycles = m_cpu->cycles();
            for (int i = 0; i < steps; ++i) {
                histogram[m_cpu->pc()]++;
                if (m_machine && m_machine->schedulerStep)
                    m_machine->schedulerStep(*m_machine);
                else m_cpu->step();
                if (m_dbg && m_dbg->isPaused()) break;
            }
            uint64_t totalCycles = m_cpu->cycles() - startCycles;
            int totalSamples = 0;
            for (const auto& [a,c] : histogram) totalSamples += c;

            std::vector<std::pair<uint32_t,int>> sorted(histogram.begin(), histogram.end());
            std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b){ return a.second > b.second; });
            if ((int)sorted.size() > top) sorted.resize(top);

            ToolResult result;
            std::ostringstream os;
            os << totalSamples << " instructions, " << totalCycles << " cycles, "
               << histogram.size() << " unique addresses";
            result.summary = os.str();
            result.columns = {
                {"addr", "Address", 90, false}, {"count", "Count", 70, true},
                {"pct", "Pct", 60, true}, {"sym", "Symbol", 120, false},
                {"instr", "Instruction", 200, false}
            };
            for (const auto& [addr, cnt] : sorted) {
                ToolResultRow row;
                char buf[16]; std::snprintf(buf, sizeof(buf), "$%04X", addr);
                row.values["addr"] = buf;
                row.values["count"] = std::to_string(cnt);
                std::snprintf(buf, sizeof(buf), "%.1f%%", totalSamples > 0 ? 100.0*cnt/totalSamples : 0);
                row.values["pct"] = buf;
                if (m_dbg) {
                    std::string sym = m_dbg->symbols().getLabel(addr);
                    if (!sym.empty()) row.values["sym"] = sym;
                }
                if (m_disasm) {
                    char dbuf[64];
                    m_disasm->disasmOne(m_bus, addr, dbuf, sizeof(dbuf));
                    row.values["instr"] = dbuf;
                }
                result.rows.push_back(row);
            }
            return result;
        });
        dlg.ShowModal();
    }, ID_TOOL_PROFILE);

    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (!m_cpu) return;
        std::vector<ToolField> fields = {
            {"start", "Start Address", "address", "", "Start of region to measure"},
            {"end",   "End Address",   "address", "", "End of region (exclusive)"}
        };
        ToolRunnerDialog dlg(this, "Measure Region", fields, [this](const auto& args) -> ToolResult {
            uint32_t startAddr = 0, endAddr = 0;
            if (m_dbg) {
                ExpressionEvaluator::evaluate(args.at("start"), m_dbg, startAddr, 16);
                ExpressionEvaluator::evaluate(args.at("end"), m_dbg, endAddr, 16);
            }
            m_cpu->setPc(startAddr);
            uint64_t sc = m_cpu->cycles();
            int ic = 0;
            for (int i = 0; i < 10000000; ++i) {
                uint32_t pc = m_cpu->pc();
                if (pc < startAddr || pc >= endAddr) break;
                if (m_machine && m_machine->schedulerStep)
                    m_machine->schedulerStep(*m_machine);
                else m_cpu->step();
                ic++;
                if (m_dbg && m_dbg->isPaused()) break;
            }
            uint64_t tc = m_cpu->cycles() - sc;
            ToolResult result;
            std::ostringstream os;
            os << "Region $" << std::hex << std::uppercase << startAddr
               << " - $" << endAddr << std::dec
               << "\nInstructions: " << ic << "\nTotal cycles: " << tc
               << "\nAvg cycles/instr: " << std::fixed << std::setprecision(1)
               << (ic > 0 ? (double)tc/ic : 0)
               << "\nFinal PC: $" << std::hex << m_cpu->pc();
            result.summary = os.str();
            return result;
        });
        dlg.ShowModal();
    }, ID_TOOL_MEASURE);

    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (!m_cpu || !m_dbg) return;
        std::vector<ToolField> fields = {
            {"condition", "Condition", "expression", "", "Expression that must become true (e.g., A == $42)"}
        };
        ToolRunnerDialog dlg(this, "Run Until Condition", fields, [this](const auto& args) -> ToolResult {
            std::string cond = args.at("condition");
            m_dbg->resume();
            int steps = 0;
            bool condMet = false;
            for (int i = 0; i < 10000000; ++i) {
                if (m_machine && m_machine->schedulerStep)
                    m_machine->schedulerStep(*m_machine);
                else m_cpu->step();
                steps++;
                if (m_dbg->isPaused()) break;
                if (ExpressionEvaluator::evaluateCondition(cond, m_dbg)) {
                    condMet = true; break;
                }
            }
            ToolResult result;
            if (condMet)
                result.summary = "Condition met after " + std::to_string(steps) + " steps.";
            else if (m_dbg->isPaused())
                result.summary = "Breakpoint hit after " + std::to_string(steps) + " steps.";
            else
                result.summary = "Max steps reached (" + std::to_string(steps) + ").";
            return result;
        });
        dlg.ShowModal();
    }, ID_TOOL_RUNTO);

    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (!m_machine || !m_machine->ioRegistry) return;
        std::vector<IOHandler*> handlers;
        m_machine->ioRegistry->enumerate(handlers);
        wxArrayString choices;
        for (auto* h : handlers) choices.Add(h->name());
        wxString sel = wxGetSingleChoice("Select device:", "Device Info", choices, this);
        if (sel.empty()) return;
        IOHandler* handler = m_machine->ioRegistry->findHandler(sel.ToStdString());
        if (!handler) return;

        DeviceInfo info;
        handler->getDeviceInfo(info);

        ToolResult result;
        std::ostringstream os;
        os << info.name << "  base=$" << std::hex << std::uppercase << info.baseAddr
           << "  mask=$" << info.addrMask << std::dec;
        for (const auto& [k,v] : info.state) os << "\n" << k << " = " << v;
        result.summary = os.str();

        if (!info.registers.empty()) {
            result.columns = {
                {"addr", "Address", 80, false}, {"name", "Register", 140, false},
                {"value", "Value", 60, true}, {"desc", "Description", 250, false}
            };
            for (const auto& r : info.registers) {
                ToolResultRow row;
                char buf[16]; std::snprintf(buf, sizeof(buf), "$%04X", info.baseAddr + r.offset);
                row.values["addr"] = buf;
                row.values["name"] = r.name;
                std::snprintf(buf, sizeof(buf), "$%02X", r.value);
                row.values["value"] = buf;
                row.values["desc"] = r.description;
                result.rows.push_back(row);
            }
        }

        ToolRunnerDialog dlg(this, "Device Info: " + info.name, {}, [result](const auto&) { return result; });
        // Auto-run to populate immediately
        wxCommandEvent dummy;
        dlg.GetEventHandler()->ProcessEvent(
            (dummy.SetEventType(wxEVT_BUTTON), dummy.SetId(wxID_OK), dummy));
        dlg.ShowModal();
    }, ID_TOOL_DEVINFO);

    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (!m_dbg) return;
        auto& hm = m_dbg->heatmap();
        std::vector<ToolField> fields = {
            {"action", "Action", "choice", "top", "What to do", {"top", "page", "enable", "disable", "reset"}},
            {"top",    "Top N",  "integer", "20", "Number of hotspots (for 'top' action)"}
        };
        ToolRunnerDialog dlg(this, "Memory Heat Map", fields, [this, &hm](const auto& args) -> ToolResult {
            std::string action = args.at("action");
            ToolResult r;
            if (action == "enable")  { hm.setEnabled(true);  r.summary = "Recording enabled."; return r; }
            if (action == "disable") { hm.setEnabled(false); r.summary = "Recording disabled."; return r; }
            if (action == "reset")   { hm.reset();           r.summary = "Data cleared."; return r; }

            r.summary = std::string("Heat map: ") + (hm.isEnabled() ? "recording" : "paused");

            if (action == "top") {
                int n = 20; try { n = std::stoi(args.at("top")); } catch (...) {}
                auto spots = hm.topAddresses(n);
                if (spots.empty()) { r.summary += "\nNo data. Use 'enable' first, then run the CPU."; return r; }
                r.columns = {
                    {"addr", "Address", 90, false}, {"reads", "Reads", 70, true},
                    {"writes", "Writes", 70, true}, {"total", "Total", 70, true},
                    {"sym", "Symbol", 150, false}
                };
                for (const auto& s : spots) {
                    ToolResultRow row;
                    char buf[16]; std::snprintf(buf, sizeof(buf), "$%04X", s.addr);
                    row.values["addr"] = buf;
                    row.values["reads"] = std::to_string(s.reads);
                    row.values["writes"] = std::to_string(s.writes);
                    row.values["total"] = std::to_string(s.total);
                    if (m_dbg) {
                        std::string sym = m_dbg->symbols().getLabel(s.addr);
                        if (!sym.empty()) row.values["sym"] = sym;
                    }
                    r.rows.push_back(row);
                }
            } else if (action == "page") {
                r.columns = {{"page", "Page", 80, false}, {"heat", "Heat", 400, false}};
                uint32_t pages = hm.size() / 256;
                for (uint32_t p = 0; p < pages; p += 16) {
                    ToolResultRow row;
                    char buf[16]; std::snprintf(buf, sizeof(buf), "$%04X", p * 256);
                    row.values["page"] = buf;
                    std::string bar;
                    for (uint32_t i = 0; i < 16 && (p+i) < pages; ++i) {
                        double h = hm.pageHeat(p+i);
                        if (h == 0) bar += '.';
                        else if (h < 0.1) bar += '-';
                        else if (h < 0.3) bar += '+';
                        else if (h < 0.6) bar += '#';
                        else bar += '@';
                    }
                    row.values["heat"] = bar;
                    r.rows.push_back(row);
                }
            }
            return r;
        });
        dlg.ShowModal();
    }, ID_TOOL_HEATMAP);

    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (!m_machine || !m_machine->ioRegistry) return;
        std::vector<IOHandler*> handlers;
        m_machine->ioRegistry->enumerate(handlers);
        IOHandler* vic = nullptr;
        for (auto* h : handlers) {
            std::string n = h->name();
            if (n.find("VIC") != std::string::npos) { vic = h; break; }
        }
        if (!vic) { wxMessageBox("No VIC device found.", "Error", wxICON_ERROR); return; }

        ToolRunnerDialog dlg(this, "Raster Position", {}, [vic](const auto&) -> ToolResult {
            ToolResult r;
            DeviceInfo info;
            vic->getDeviceInfo(info);
            std::ostringstream os;
            for (const auto& [k, v] : info.state) {
                if (k == "Raster Line" || k == "Raster Cycle" ||
                    k == "Lines/Frame" || k == "Cycles/Line")
                    os << k << ": " << v << "\n";
            }
            r.summary = os.str();
            return r;
        });
        // Auto-run
        wxCommandEvent dummy; dummy.SetEventType(wxEVT_BUTTON); dummy.SetId(wxID_OK);
        dlg.GetEventHandler()->ProcessEvent(dummy);
        dlg.ShowModal();
    }, ID_TOOL_RASTER);

    Bind(wxEVT_TOOL, &MmemuFrame::OnKbdFocus, this, ID_KBD_FOCUS);
    Bind(wxEVT_TIMER, &MmemuFrame::OnTimer, this, ID_GUI_TIMER);
    
    Bind(wxEVT_MENU, &MmemuFrame::OnLoadMachineDirect, this, ID_LOAD_MACHINE_DIRECT);
    Bind(wxEVT_MENU, &MmemuFrame::OnLoadImageDirect,   this, ID_LOAD_IMAGE_DIRECT);
    Bind(wxEVT_MENU, &MmemuFrame::OnTypeTextDirect,    this, ID_TYPE_TEXT_DIRECT);

    // Ctrl+Shift+K toggles capture; FilterEvent routes all other keys.
    Bind(wxEVT_CHAR_HOOK, &MmemuFrame::OnCtrlShiftK, this);

    m_timer.Start(kTimerMs); // ~30 FPS pane refresh; emulation runs kRunSliceMs of each tick
}

void MmemuFrame::OnLoadMachineDirect(wxCommandEvent& event) {
    std::string id = event.GetString().ToStdString();
    loadMachineById(id);
}

void MmemuFrame::OnLoadImageDirect(wxCommandEvent& event) {
    if (!m_bus || !m_machine) return;
    std::string path = event.GetString().ToStdString();
    IBus* loadBus = m_machine->buses[0].bus; // physical bus (#79)
    auto* loader = ImageLoaderRegistry::instance().findLoader(path);
    if (loader) {
        if (loader->load(path, loadBus, m_machine, 0)) {
            SetStatusText("Loaded " + path);
            for (auto* p : m_memPanes) p->RefreshValues();
        }
    }
}

void MmemuFrame::OnTypeTextDirect(wxCommandEvent& event) {
    if (!m_machine) return;
    std::string text = event.GetString().ToStdString();
    IKeyboardMatrix* kbd = nullptr;
    if (m_machine->ioRegistry) {
        std::vector<IOHandler*> handlers;
        m_machine->ioRegistry->enumerate(handlers);
        for (auto* h : handlers) {
            if ((kbd = dynamic_cast<IKeyboardMatrix*>(h))) break;
        }
    }
    if (kbd) kbd->enqueueText(text);
}

void MmemuFrame::OnLoadMachine(wxCommandEvent& event) {
    (void)event;
    MachineSelectorDialog dialog(this);
    if (dialog.ShowModal() == wxID_OK) {
        loadMachineById(dialog.GetSelectedMachineId());
    }
}

void MmemuFrame::loadMachineById(const std::string& id) {
    MachineDescriptor* md = MachineRegistry::instance().createMachine(id);
    if (!md) {
        SetStatusText("Unknown machine: " + id);
        return;
    }

    if (m_machine != md) delete m_machine;
    m_machine = md;
    m_cpu = m_machine->cpus[0].cpu;
    m_bus = m_machine->cpus[0].dataBus ? m_machine->cpus[0].dataBus : m_machine->buses[0].bus;
    if (m_disasm) {
        delete m_disasm;
        m_disasm = nullptr;
    }
    m_disasm = ToolchainRegistry::instance().createDisassembler(m_cpu->isaName());

    delete m_dbg;
    m_dbg = new DebugContext(m_cpu, m_machine->buses[0].bus);
    m_cpu->setObserver(m_dbg);
    m_machine->buses[0].bus->setObserver(m_dbg);
    m_dbg->onMachineLoad(m_machine);

    for (const auto& path : m_machine->symbolFiles)
        m_dbg->symbols().loadSym(path);
    if (m_disasm)
        m_disasm->setSymbolTable(&m_dbg->symbols());

    m_regPane->SetCPU(m_cpu);
    for (auto* p : m_memPanes) p->SetBus(m_bus);
    m_disasmPane->SetBus(m_bus);
    m_disasmPane->SetDisassembler(m_disasm);
    m_disasmPane->SetDebugContext(m_dbg);
    m_disasmPane->SetMemoryPaneCallback([this](uint32_t addr) {
        if (!m_memPanes.empty()) m_memPanes[0]->SetAddress(addr);
    });
    m_consolePane->SetContext(m_cpu, m_bus);
    m_consolePane->SetMachine(m_machine);
    m_consolePane->SetDebugContext(m_dbg);
    m_bpPane->SetDebugContext(m_dbg);
    m_symPane->SetDebugContext(m_dbg);
    m_symPane->SetGotoCallback([this](uint32_t addr){ m_disasmPane->GoTo(addr); });
    m_stackPane->SetDebugContext(m_dbg);
    m_stackPane->SetGotoCallback([this](uint32_t addr){ m_disasmPane->GoTo(addr); });
    m_cartPane->SetBus(m_bus);
    m_machineInspectorPane->setMachine(m_machine);
    m_deviceInfoPane->setMachine(m_machine);
    m_regWatchPane->setMachine(m_machine);
    m_tracePane->SetDebugContext(m_dbg);
    m_tracePane->SetCPU(m_cpu);
    m_sourcePane->SetDebugContext(m_dbg);

    // Show/hide MEGA65 status pane and Reset to Monitor menu based on machine type
    {
        bool isMega65 = (id.find("mega65") != std::string::npos);
        if (isMega65) m_mega65StatusPane->SetMachine(m_machine, m_cpu);
        m_dock->setAvailable("mega65", isMega65);
        // Enable Reset to Monitor menu only for MEGA65
        if (m_resetToMonitorItem) {
            m_resetToMonitorItem->Enable(isMega65);
        }
    }


    // Control > Speed: only for machines with a CPU speed model (MEGA65)
    if (m_controlMenu && m_speedMenuItem) {
        bool want = machine_speed::supported(*m_machine);
        if (want && !m_speedMenuShown) m_controlMenu->Insert(m_speedMenuPos, m_speedMenuItem);
        if (!want && m_speedMenuShown) m_controlMenu->Remove(m_speedMenuItem);
        m_speedMenuShown = want;
    }

    if (m_machine->onReset) m_machine->onReset(*m_machine);

    // Discover and start audio output
    delete m_audio;
    m_audio = nullptr;
    if (m_machine->ioRegistry) {
        std::vector<IOHandler*> handlers;
        m_machine->ioRegistry->enumerate(handlers);
        for (auto* h : handlers) {
            if (auto* ao = dynamic_cast<IAudioOutput*>(h)) {
                m_audio = new AudioOutput();
                if (!m_audio->start(ao)) {
                    delete m_audio;
                    m_audio = nullptr;
                }
                break;
            }
        }
    }

    // Release capture before switching machine.
    m_capturePane = nullptr;
    setKeyCapture(false);

    PluginPaneManager::instance().onMachineSwitch(id, m_dock->bottomBook(), nullptr, m_machine);

    // Wire the screen pane's Capture button
    wxWindow* screenWin = PluginPaneManager::instance().getPaneWindow("screen");
    if (screenWin) {
        m_capturePane = dynamic_cast<IKeyboardCapturePane*>(screenWin);
        if (m_capturePane)
            m_capturePane->SetCaptureCallback([this](bool active) { setKeyCapture(active); });
    }

    std::string statusMsg = "Loaded machine: " + id;
    if (!m_machine->onKey) statusMsg += " (no keyboard)";
    SetTitle("mmemu - " + m_machine->displayName);
    SetStatusText(statusMsg);
}

void MmemuFrame::OnStep(wxCommandEvent&)     { doStep(source_stepper::Mode::Into); }
void MmemuFrame::OnStepOver(wxCommandEvent&) { doStep(source_stepper::Mode::Over); }
void MmemuFrame::OnStepOut(wxCommandEvent&)  { doStep(source_stepper::Mode::Out); }

// Step by source line where the PC has a source mapping, otherwise by
// instruction (Over runs a called subroutine to its return). Goes through
// the machine scheduler, so devices advance as when running.
void MmemuFrame::doStep(source_stepper::Mode mode) {
    if (!m_cpu || m_running) return;
    if (!m_dbg) {
        m_cpu->step();
    } else {
        // Bounded so a runaway loop cannot freeze the window for long
        auto r = source_stepper::step(*m_dbg, m_cpu, m_bus, m_machine, mode, 5000000);
        wxString msg = source_stepper::stopName(r.stop);
        if (!r.location.file.empty())
            msg += wxString::Format(" - %s:%d",
                                    wxString::FromUTF8(std::filesystem::path(r.location.file).filename().string()),
                                    r.location.line);
        if (r.stop == source_stepper::Stop::Breakpoint) msg = m_dbg->lastHitMessage();
        SetStatusText(msg);
    }
    refreshAfterStop();
}

void MmemuFrame::refreshAfterStop() {
    if (!m_cpu) return;
    m_regPane->RefreshValues();
    m_disasmPane->RefreshValues(m_cpu->pc());
    m_stackPane->RefreshValues();
    if (m_bpPane) m_bpPane->RefreshValues();
    for (auto* p : m_memPanes) p->UpdatePc(m_cpu->pc());
    updateSourceView();
}

// Show the source line for the PC (opening the Source pane) when mapped
void MmemuFrame::updateSourceView() {
    if (!m_sourcePane || !m_cpu || !m_dbg) return;
    if (m_sourcePane->ShowExecutionLine(m_cpu->pc()))
        m_dock->open("source");
    m_sourcePane->RefreshBreakpointMarkers();
}

// ---------------------------------------------------------------------------
// Makefile projects (#148)
// ---------------------------------------------------------------------------

bool MmemuFrame::openProject(const std::string& dir) {
    auto cfg = std::make_unique<project::Config>();
    std::string err;
    if (!project::open(dir, *cfg, err)) {
        SetStatusText(wxString::FromUTF8(err));
        return false;
    }
    stopBuildAndWait();
    m_project = std::move(cfg);
    if (!m_project->machine.empty() && (!m_machine || m_machine->machineId != m_project->machine))
        loadMachineById(m_project->machine);
    SetStatusText(wxString::FromUTF8("Opened project " + m_project->name + " - Build: Ctrl-B, Run: Ctrl-F5"));
    return true;
}

void MmemuFrame::appendOutput(const std::string& text) {
    if (m_outputPane) m_outputPane->AppendText(wxString::FromUTF8(text));
}

void MmemuFrame::stopBuildAndWait() {
    m_buildCancel = true;
    if (m_buildThread.joinable()) m_buildThread.join();
}

void MmemuFrame::startBuild(bool run) {
    if (!m_project || m_building) return;
    if (m_sourcePane) m_sourcePane->SaveAll();
    if (m_buildThread.joinable()) m_buildThread.join();

    m_outputPane->Clear();
    m_problemsPane->DeleteAllItems();
    m_problems.clear();
    m_sourcePane->SetDiagnostics({});
    m_dock->open("output");

    const project::Config cfg = *m_project;   // the worker gets its own copy
    const std::string target = run ? cfg.runTarget : cfg.buildTarget;
    m_buildParser = std::make_unique<project::DiagnosticParser>(cfg.dir);
    project::FileTimes before = project::snapshotPrograms(cfg.dir);
    m_building = true;
    m_buildCancel = false;
    appendOutput("make " + target + "   (" + cfg.dir + ")\n");
    SetStatusText("Building...");

    m_buildThread = std::thread([this, cfg, target, run, before]() {
        auto res = project::runMake(cfg, target, [this](const std::string& line) {
            CallAfter([this, line]() { onBuildLine(line); });
        }, &m_buildCancel);
        CallAfter([this, res, run, before]() { onBuildDone(res, run, before); });
    });
}

void MmemuFrame::onBuildLine(const std::string& line) {
    appendOutput(line + "\n");
    if (!m_buildParser) return;
    auto d = m_buildParser->feed(line);
    if (!d) return;
    long idx = (long)m_problems.size();
    m_problems.push_back(*d);
    const char* sev = d->severity == project::Diagnostic::Severity::Error ? "error"
                    : d->severity == project::Diagnostic::Severity::Warning ? "warning" : "note";
    long row = m_problemsPane->InsertItem(m_problemsPane->GetItemCount(), sev);
    m_problemsPane->SetItem(row, 1, d->file.empty() ? wxString("?")
                                    : wxString::FromUTF8(std::filesystem::path(d->file).filename().string()));
    m_problemsPane->SetItem(row, 2, wxString::Format("%d", d->line));
    m_problemsPane->SetItem(row, 3, wxString::FromUTF8(d->message));
    m_problemsPane->SetItemData(row, idx);
}

void MmemuFrame::onBuildDone(project::MakeResult res, bool run, project::FileTimes before) {
    if (m_buildThread.joinable()) m_buildThread.join();
    m_building = false;
    m_sourcePane->ReloadChangedFiles();   // the build may have regenerated sources

    int errors = 0, warnings = 0;
    std::vector<SourcePane::LineMark> marks;
    for (const auto& d : m_problems) {
        bool err = d.severity == project::Diagnostic::Severity::Error;
        if (err) ++errors; else if (d.severity == project::Diagnostic::Severity::Warning) ++warnings;
        if (!d.file.empty() && d.severity != project::Diagnostic::Severity::Note)
            marks.push_back({d.file, d.line, err});
    }
    m_sourcePane->SetDiagnostics(marks);
    const std::string summary = std::to_string(errors) + " error(s), " + std::to_string(warnings) + " warning(s)";

    if (!res.started || !res.error.empty()) {
        appendOutput("Error: " + res.error + "\n");
        SetStatusText(wxString::FromUTF8("Build could not run: " + res.error));
        return;
    }
    if (res.cancelled) { appendOutput("Build stopped.\n"); SetStatusText("Build stopped."); return; }
    if (!res.ok()) {
        appendOutput("Build failed (make exit " + std::to_string(res.exitCode) + "): " + summary + "\n");
        SetStatusText(wxString::FromUTF8("Build failed: " + summary));
        if (!m_problems.empty()) m_dock->open("problems");
        return;
    }
    appendOutput("Build succeeded: " + summary + "\n");
    if (!run) { SetStatusText(wxString::FromUTF8("Build succeeded: " + summary)); return; }

    const project::Config& cfg = *m_project;
    std::string file = cfg.load == "auto" ? project::newestChangedProgram(cfg.dir, before) : cfg.resolve(cfg.load);
    if (file.empty()) {
        appendOutput("No new .prg was written by make " + cfg.runTarget + "; set run.load in .mmsim/project.json.\n");
        SetStatusText("No new .prg to run");
        return;
    }
    if (!cfg.machine.empty() && (!m_machine || m_machine->machineId != cfg.machine))
        loadMachineById(cfg.machine);
    if (!m_machine || !m_dbg) { SetStatusText("Load a machine first."); return; }

    m_running = false;
    SetStatusText("Starting...");
    auto launched = project::launch(*m_machine, m_dbg, file, cfg.reset);
    appendOutput(launched.message + "\n");
    if (!launched.ok) { SetStatusText(wxString::FromUTF8(launched.message)); return; }
    std::string dbgInfo = project::loadDebugInfo(*m_dbg, cfg);
    if (!dbgInfo.empty()) appendOutput(dbgInfo + "\n");
    m_sourcePane->RefreshBreakpointMarkers(true);
    if (m_sourcePane) m_sourcePane->ClearExecutionLine();
    m_dbg->resume();
    m_running = true;
    SetStatusText(wxString::FromUTF8(launched.message));
}

void MmemuFrame::OnOpenSource(wxCommandEvent&) {
    wxFileDialog dlg(this, "Open Source", "", "",
                     "Source files (*.s;*.asm;*.s45;*.a65;*.inc;*.bas;*.c;*.h)|*.s;*.asm;*.s45;*.a65;*.inc;*.bas;*.c;*.h|"
                     "All files (*)|*", wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
    if (dlg.ShowModal() != wxID_OK) return;
    wxArrayString paths;
    dlg.GetPaths(paths);
    for (const auto& p : paths) m_sourcePane->OpenFile(p.utf8_string());
    m_dock->open("source");
}

void MmemuFrame::OnLoadSourceMap(wxCommandEvent&) {
    if (!m_dbg) { SetStatusText("Load a machine first."); return; }
    wxFileDialog mapDlg(this, "Load ca45 Source Map", "", "",
                        "Source maps (*.map;*.txt)|*.map;*.txt|All files (*)|*", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (mapDlg.ShowModal() != wxID_OK) return;
    std::string mapPath = mapDlg.GetPath().utf8_string();

    // ca45 maps do not name the source file: use the open editor tab, or a
    // same-named source next to the map, or ask
    std::string srcPath = m_sourcePane->CurrentFile();
    if (srcPath.empty()) {
        std::filesystem::path base(mapPath);
        for (const char* ext : {".s", ".asm", ".s45", ".a65"}) {
            auto cand = base; cand.replace_extension(ext);
            if (std::filesystem::exists(cand)) { srcPath = cand.string(); break; }
        }
    }
    if (srcPath.empty()) {
        wxFileDialog srcDlg(this, "Source File for This Map", "", "",
                            "Source files (*.s;*.asm;*.s45;*.a65)|*.s;*.asm;*.s45;*.a65|All files (*)|*",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (srcDlg.ShowModal() != wxID_OK) return;
        srcPath = srcDlg.GetPath().utf8_string();
    }

    std::vector<std::pair<std::string, uint32_t>> symbols;
    int n = m_dbg->sourceMap().loadCa45SourceMap(mapPath, srcPath, &symbols);
    if (n == 0) { SetStatusText("No address mappings found in " + wxString::FromUTF8(mapPath)); return; }
    for (const auto& [label, addr] : symbols) m_dbg->symbols().addSymbol(addr, label);
    m_sourcePane->OpenFile(srcPath);
    m_dock->open("source");
    m_sourcePane->RefreshBreakpointMarkers(true);
    updateSourceView();
    SetStatusText(wxString::Format("Loaded %d source lines and %zu symbols for %s", n, symbols.size(),
                                   wxString::FromUTF8(std::filesystem::path(srcPath).filename().string())));
}

void MmemuFrame::OnBackStep(wxCommandEvent& event) {
    (void)event;
    if (m_dbg && m_dbg->reverseStep()) {
        m_regPane->RefreshValues();
        m_disasmPane->RefreshValues(m_cpu->pc());
        m_stackPane->RefreshValues();
        for (auto* p : m_memPanes) p->UpdatePc(m_cpu->pc());
        SetStatusText("Reversed 1 instruction.");
    } else {
        SetStatusText("No undo history available.");
    }
}

void MmemuFrame::OnRun(wxCommandEvent& event) {
    (void)event;
    if (m_dbg) m_dbg->resume();
    if (m_sourcePane) m_sourcePane->ClearExecutionLine();
    m_running = true;
    SetStatusText("Running...");
}

void MmemuFrame::OnPause(wxCommandEvent& event) {
    (void)event;
    m_running = false;
    SetStatusText("Paused.");
    refreshAfterStop();
}

void MmemuFrame::OnReset(wxCommandEvent& event) {
    (void)event;
    if (m_machine && m_machine->onReset) {
        m_running = false;
        m_machine->onReset(*m_machine);
        if (m_dbg) m_dbg->resume();
        m_regPane->RefreshValues();
        m_disasmPane->RefreshValues(m_cpu->pc());
        SetStatusText("Reset.");
    }
}

static const char* speedValueFor(int id) {
    switch (id) {
        case ID_SPEED_1:   return "1";
        case ID_SPEED_2:   return "2";
        case ID_SPEED_3_5: return "3.5";
        case ID_SPEED_40:  return "40.5";
        default:           return "max";
    }
}

void MmemuFrame::OnSpeed(wxCommandEvent& event) {
    if (!m_machine) return;
    std::string error;
    double requested = 0;
    if (!machine_speed::apply(*m_machine, speedValueFor(event.GetId()), error, &requested)) {
        SetStatusText(error);
        return;
    }
    std::string msg = machine_speed::describe(*m_machine);
    std::string note = machine_speed::pendingNote(*m_machine, requested);
    if (!note.empty()) msg += " " + note;
    SetStatusText(msg);
}

void MmemuFrame::OnSpeedUpdateUI(wxUpdateUIEvent& event) {
    if (!m_machine || !machine_speed::supported(*m_machine)) {
        event.Enable(false);
        return;
    }
    const std::string value = speedValueFor(event.GetId());
    bool offered = value == "max";
    for (double v : m_machine->cpuSpeeds)
        if (machine_speed::formatMHz(v) == value) offered = true;
    event.Enable(offered);
    // Checked item follows the machine, including speed changes made by software
    double mhz = m_machine->cpuSpeedMHz();
    bool current = m_machine->getWarp() ? value == "max"
                                        : (value != "max" && machine_speed::formatMHz(mhz) == value);
    if (current) event.Check(true);
}

void MmemuFrame::OnResetToMonitor(wxCommandEvent& event) {
    (void)event;
    if (!m_machine || !m_machine->onKey || !m_machine->onReset) return;

    // Press Run/Stop
    m_machine->onKey("RUNSTOP", true);

    // Reset the machine
    m_running = false;
    m_machine->onReset(*m_machine);
    if (m_dbg) m_dbg->resume();

    // Step through a few boot cycles while Run/Stop is held
    for (int i = 0; i < 100 && m_cpu; ++i) {
        if (m_machine->schedulerStep)
            m_machine->schedulerStep(*m_machine);
        else
            m_cpu->step();
    }

    // Release Run/Stop
    m_machine->onKey("RUNSTOP", false);

    m_regPane->RefreshValues();
    m_disasmPane->RefreshValues(m_cpu->pc());
    SetStatusText("Reset to Monitor - Run/Stop held during boot.");
}

void MmemuFrame::OnFillMem(wxCommandEvent& event) {
    (void)event;
    if (!m_bus) return;
    std::vector<ToolField> fields = {
        {"addr",  "Address",  "address", "0000", "Start address"},
        {"len",   "Length",   "address", "100",  "Number of bytes to fill"},
        {"value", "Value",    "address", "00",   "Fill byte value"}
    };
    ToolRunnerDialog dlg(this, "Fill Memory", fields, [this](const auto& args) -> ToolResult {
        uint32_t addr = 0, len = 0, val = 0;
        ExpressionEvaluator::evaluate(args.at("addr"),  m_dbg, addr, 16);
        ExpressionEvaluator::evaluate(args.at("len"),   m_dbg, len, 16);
        ExpressionEvaluator::evaluate(args.at("value"), m_dbg, val, 16);
        for (uint32_t i = 0; i < len; ++i) m_bus->write8(addr + i, (uint8_t)val);
        for (auto* p : m_memPanes) p->RefreshValues();
        ToolResult r;
        r.summary = "Filled " + std::to_string(len) + " bytes at $" +
                    (std::ostringstream() << std::hex << std::uppercase << addr).str();
        return r;
    });
    dlg.ShowModal();
}

void MmemuFrame::OnCopyMem(wxCommandEvent& event) {
    (void)event;
    if (!m_bus) return;
    std::vector<ToolField> fields = {
        {"src", "Source Address",      "address", "0000", "Source start address"},
        {"len", "Length",              "address", "100",  "Number of bytes"},
        {"dst", "Destination Address", "address", "0000", "Destination start address"}
    };
    ToolRunnerDialog dlg(this, "Copy Memory", fields, [this](const auto& args) -> ToolResult {
        uint32_t src = 0, len = 0, dst = 0;
        ExpressionEvaluator::evaluate(args.at("src"), m_dbg, src, 16);
        ExpressionEvaluator::evaluate(args.at("len"), m_dbg, len, 16);
        ExpressionEvaluator::evaluate(args.at("dst"), m_dbg, dst, 16);
        std::vector<uint8_t> tmp(len);
        for (uint32_t i = 0; i < len; ++i) tmp[i] = m_bus->read8(src + i);
        for (uint32_t i = 0; i < len; ++i) m_bus->write8(dst + i, tmp[i]);
        for (auto* p : m_memPanes) p->RefreshValues();
        ToolResult r;
        r.summary = "Copied " + std::to_string(len) + " bytes";
        return r;
    });
    dlg.ShowModal();
}

void MmemuFrame::OnSwapMem(wxCommandEvent& event) {
    (void)event;
    if (!m_bus) return;
    std::vector<ToolField> fields = {
        {"addr1", "Address 1", "address", "0000", "First region start"},
        {"len",   "Length",    "address", "100",  "Number of bytes"},
        {"addr2", "Address 2", "address", "0000", "Second region start"}
    };
    ToolRunnerDialog dlg(this, "Swap Memory", fields, [this](const auto& args) -> ToolResult {
        uint32_t addr1 = 0, len = 0, addr2 = 0;
        ExpressionEvaluator::evaluate(args.at("addr1"), m_dbg, addr1, 16);
        ExpressionEvaluator::evaluate(args.at("len"),   m_dbg, len, 16);
        ExpressionEvaluator::evaluate(args.at("addr2"), m_dbg, addr2, 16);
        for (uint32_t i = 0; i < len; ++i) {
            uint8_t v1 = m_bus->read8(addr1 + i);
            uint8_t v2 = m_bus->read8(addr2 + i);
            m_bus->write8(addr1 + i, v2);
            m_bus->write8(addr2 + i, v1);
        }
        for (auto* p : m_memPanes) p->RefreshValues();
        ToolResult r;
        r.summary = "Swapped " + std::to_string(len) + " bytes";
        return r;
    });
    dlg.ShowModal();
}

void MmemuFrame::OnAssemble(wxCommandEvent& event) {
    (void)event;
    if (!m_cpu || !m_bus) return;

    std::string currentAsm = m_machine ? m_machine->preferredAssembler : "";
    AssembleDialog dialog(this, m_cpu->pc(), currentAsm);
    if (dialog.ShowModal() == wxID_OK) {
        std::string instr = dialog.GetInstruction();
        uint32_t addr = dialog.GetAddress();
        std::string selectedAsm = dialog.GetSelectedAssembler();

        std::string machPref = m_machine ? m_machine->preferredAssembler : "";
        IAssembler* assem = resolveAssembler(m_cpu->isaName(), machPref, selectedAsm);
        if (assem) {
            uint8_t opcodes[16];
            int sz = assem->assembleLine(instr, opcodes, sizeof(opcodes), addr);
            if (sz > 0) {
                for (int i = 0; i < sz; ++i) m_bus->write8(addr + i, opcodes[i]);
                for (auto* p : m_memPanes) p->RefreshValues();
                m_disasmPane->RefreshValues(m_cpu->pc());
            } else {
                wxMessageBox("Assembly failed!\nThe selected assembler may not support line mode.",
                             "Error", wxOK | wxICON_ERROR);
            }
            delete assem;
        } else {
            wxMessageBox("No assembler available for this ISA.", "Error", wxOK | wxICON_ERROR);
        }
    }
}

void MmemuFrame::OnGotoAddr(wxCommandEvent& event) {
    (void)event;
    if (!m_cpu) return;
    char pcBuf[16]; std::snprintf(pcBuf, sizeof(pcBuf), "%04X", m_cpu->pc());
    std::vector<ToolField> fields = {
        {"addr",  "Address",     "address", pcBuf, "Target address ($hex or expression)"},
        {"setpc", "Set PC",      "boolean", "",    "Also set the program counter"}
    };
    ToolRunnerDialog dlg(this, "Go to Address", fields, [this](const auto& args) -> ToolResult {
        uint32_t addr = 0;
        ExpressionEvaluator::evaluate(args.at("addr"), m_dbg, addr, 16);
        if (args.at("setpc") == "1") {
            m_cpu->setPc(addr);
            m_disasmPane->RefreshValues(m_cpu->pc());
        }
        if (auto* _mp = activeMemPane()) _mp->SetAddress(addr);
        for (auto* p : m_memPanes) p->RefreshValues();
        ToolResult r;
        r.summary = "Navigated to $" + (std::ostringstream() << std::hex << std::uppercase << addr).str();
        return r;
    });
    dlg.ShowModal();
}

void MmemuFrame::OnSearchMem(wxCommandEvent& event) {
    (void)event;
    if (!m_cpu) return;
    uint32_t mask = m_bus->config().addrMask;
    char maskBuf[16]; std::snprintf(maskBuf, sizeof(maskBuf), "%X", mask + 1);
    std::vector<ToolField> fields = {
        {"pattern", "Pattern",       "string",  "",      "Hex bytes (e.g. A9 00) or ASCII text"},
        {"mode",    "Mode",          "choice",  "Hex",   "Pattern format", {"Hex", "ASCII"}},
        {"start",   "Start Address", "address", "0000",  "Search start address"},
        {"len",     "Length",        "address", maskBuf,  "Search range length"}
    };
    ToolRunnerDialog dlg(this, "Search Memory", fields, [this, mask](const auto& args) -> ToolResult {
        std::string pattern = args.at("pattern");
        bool isHex = (args.at("mode") == "Hex");
        uint32_t startAddr = 0, length = 0;
        ExpressionEvaluator::evaluate(args.at("start"), m_dbg, startAddr, 16);
        ExpressionEvaluator::evaluate(args.at("len"),   m_dbg, length, 16);
        startAddr &= mask;

        m_lastSearchPattern.clear();
        if (isHex) {
            std::string cleanHex;
            for (char c : pattern) { if (std::isxdigit(c)) cleanHex += c; }
            for (size_t i = 0; i + 1 < cleanHex.length(); i += 2) {
                try { m_lastSearchPattern.push_back((uint8_t)std::stoul(cleanHex.substr(i, 2), nullptr, 16)); }
                catch (...) {}
            }
        } else {
            for (char c : pattern) m_lastSearchPattern.push_back((uint8_t)c);
        }

        ToolResult r;
        if (m_lastSearchPattern.empty()) { r.summary = "Empty pattern."; return r; }

        uint32_t endAddr = (startAddr + length > mask + 1) ? (mask + 1) : (startAddr + length);
        uint32_t foundAddr = 0xFFFFFFFF;
        if (endAddr >= m_lastSearchPattern.size()) {
            for (uint32_t i = startAddr; i <= endAddr - m_lastSearchPattern.size(); ++i) {
                bool match = true;
                for (size_t j = 0; j < m_lastSearchPattern.size(); ++j) {
                    if (m_bus->peek8((i + j) & mask) != m_lastSearchPattern[j]) { match = false; break; }
                }
                if (match) { foundAddr = i; break; }
            }
        }
        m_lastSearchFoundAddr = foundAddr;
        if (foundAddr != 0xFFFFFFFF) {
            if (auto* _mp = activeMemPane()) _mp->SetAddress(foundAddr);
            for (auto* p : m_memPanes) p->RefreshValues();
            char buf[32]; std::snprintf(buf, sizeof(buf), "Pattern found at $%X", foundAddr);
            r.summary = buf;
            SetStatusText(buf);
        } else {
            r.summary = "Pattern not found.";
        }
        return r;
    });
    dlg.ShowModal();
}

void MmemuFrame::OnFindNext(wxCommandEvent& event) {
    if (!m_bus || m_lastSearchPattern.empty()) return;
    uint32_t mask = m_bus->config().addrMask;
    uint32_t startAddr = (m_lastSearchFoundAddr == 0xFFFFFFFF) ? 0 : (m_lastSearchFoundAddr + 1) & mask;
    
    uint32_t foundAddr = 0xFFFFFFFF;
    // Search forward from startAddr to end of memory, then wrap around once
    for (uint32_t i = 0; i < mask + 1; ++i) {
        uint32_t curr = (startAddr + i) & mask;
        if (curr + m_lastSearchPattern.size() > mask + 1) continue;
        
        bool match = true;
        for (size_t j = 0; j < m_lastSearchPattern.size(); ++j) {
            if (m_bus->peek8((curr + j) & mask) != m_lastSearchPattern[j]) {
                match = false; break;
            }
        }
        if (match) { foundAddr = curr; break; }
    }

    if (foundAddr != 0xFFFFFFFF) {
        m_lastSearchFoundAddr = foundAddr;
        if (auto* _mp = activeMemPane()) _mp->SetAddress(foundAddr);
        for (auto* p : m_memPanes) p->RefreshValues();
        SetStatusText(wxString::Format("Next pattern found at $%X", foundAddr));
    } else {
        SetStatusText("No further occurrences found.");
    }
}

void MmemuFrame::OnFindPrior(wxCommandEvent& event) {
    if (!m_bus || m_lastSearchPattern.empty()) return;
    uint32_t mask = m_bus->config().addrMask;
    uint32_t startAddr = (m_lastSearchFoundAddr == 0xFFFFFFFF) ? mask : (m_lastSearchFoundAddr - 1) & mask;
    
    uint32_t foundAddr = 0xFFFFFFFF;
    // Search backward from startAddr
    for (uint32_t i = 0; i < mask + 1; ++i) {
        uint32_t curr = (startAddr - i) & mask;
        if (curr + m_lastSearchPattern.size() > mask + 1) continue;
        
        bool match = true;
        for (size_t j = 0; j < m_lastSearchPattern.size(); ++j) {
            if (m_bus->peek8((curr + j) & mask) != m_lastSearchPattern[j]) {
                match = false; break;
            }
        }
        if (match) { foundAddr = curr; break; }
    }

    if (foundAddr != 0xFFFFFFFF) {
        m_lastSearchFoundAddr = foundAddr;
        if (auto* _mp = activeMemPane()) _mp->SetAddress(foundAddr);
        for (auto* p : m_memPanes) p->RefreshValues();
        SetStatusText(wxString::Format("Prior pattern found at $%X", foundAddr));
    } else {
        SetStatusText("No prior occurrences found.");
    }
}

void MmemuFrame::OnLoadImage(wxCommandEvent& event) {
    if (!m_bus || !m_machine) return;
    std::string initPath = event.GetString().ToStdString();
    std::vector<ToolField> fields = {
        {"path",      "File",          "file",    initPath, "Program or binary image file",
         {}, "PRG files (*.prg)|*.prg|Binary files (*.bin)|*.bin|All files (*.*)|*.*"},
        {"addr",      "Address",       "address", "",       "Override load address (blank = use PRG header)"},
        {"autostart", "Auto Start",    "boolean", "",       "Set PC to load address after loading"}
    };
    ToolRunnerDialog dlg(this, "Load Image", fields, [this](const auto& args) -> ToolResult {
        std::string path = args.at("path");
        uint32_t addr = 0;
        if (!args.at("addr").empty())
            ExpressionEvaluator::evaluate(args.at("addr"), m_dbg, addr, 16);
        bool autoStart = (args.at("autostart") == "1");
        IBus* loadBus = m_machine->buses[0].bus; // physical bus (#79)
        auto* loader = ImageLoaderRegistry::instance().findLoader(path);
        ToolResult r;
        if (!loader) { r.summary = "Error: No loader for file type"; return r; }
        if (!loader->load(path, loadBus, m_machine, addr)) {
            r.summary = "Error: Failed to load image"; return r;
        }
        r.summary = "Loaded: " + path;
        if (autoStart) {
            uint32_t startAddr = addr;
            if (startAddr == 0 && std::string(loader->name()).find("PRG") != std::string::npos) {
                std::ifstream f(path, std::ios::binary);
                uint8_t h[2]; f.read((char*)h, 2);
                startAddr = h[0] | (h[1] << 8);
            }
            m_cpu->setPc(startAddr);
            m_running = true;
            m_regPane->RefreshValues();
            m_disasmPane->RefreshValues(m_cpu->pc());
        }
        for (auto* p : m_memPanes) p->RefreshValues();
        SetStatusText("Loaded " + path);
        return r;
    });
    dlg.ShowModal();
}

void MmemuFrame::OnAttachCart(wxCommandEvent& event) {
    if (!m_bus) return;
    std::string path = event.GetString().ToStdString();
    if (path.empty()) {
        wxFileDialog openFileDialog(this, "Attach Cartridge", "", "",
                           "Cartridge files (*.crt;*.car)|*.crt;*.car", 
                           wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (openFileDialog.ShowModal() == wxID_OK) {
            path = openFileDialog.GetPath().ToStdString();
        } else {
            return;
        }
    }
    
    auto handler = ImageLoaderRegistry::instance().createCartridgeHandler(path);
    if (handler) {
        if (handler->attach(m_bus, m_machine)) {
            ImageLoaderRegistry::instance().setActiveCartridge(m_bus, std::move(handler));
            m_cartPane->RefreshMetadata();
            // Reset machine
            if (m_machine->onReset) m_machine->onReset(*m_machine);
            m_regPane->RefreshValues();
            m_disasmPane->RefreshValues(m_cpu->pc());
            for (auto* p : m_memPanes) p->RefreshValues();
            SetStatusText("Attached cartridge " + path);
        } else {
            wxMessageBox("Failed to attach cartridge.", "Error", wxICON_ERROR);
        }
    } else {
        wxMessageBox("Unsupported cartridge format.", "Error", wxICON_ERROR);
    }
}

void MmemuFrame::OnEjectCart(wxCommandEvent&) {
    if (!m_bus) return;
    auto* cart = ImageLoaderRegistry::instance().getActiveCartridge(m_bus);
    if (cart) {
        cart->eject(m_bus);
        ImageLoaderRegistry::instance().setActiveCartridge(m_bus, nullptr);
        m_cartPane->RefreshMetadata();
        if (m_machine->onReset) m_machine->onReset(*m_machine);
        m_regPane->RefreshValues();
        m_disasmPane->RefreshValues(m_cpu->pc());
        for (auto* p : m_memPanes) p->RefreshValues();
        SetStatusText("Cartridge ejected.");
    }
}

void MmemuFrame::setKeyCapture(bool active) {
    m_kbdFocus = active;
    GetToolBar()->ToggleTool(ID_KBD_FOCUS, active);
    if (m_capturePane) m_capturePane->SetCaptureActive(active);
    auto* app = static_cast<MmemuApp*>(wxTheApp);
    if (active && m_machine && m_machine->onKey) {
        app->m_keyFilter.m_handler = m_machine->onKey;
        app->m_keyFilter.m_positional = (m_machine->machineId.find("mega65") != std::string::npos);
    } else {
        app->m_keyFilter.m_handler = nullptr;
        // Release any modifier keys that may have been left pressed (e.g., the
        // Ctrl and Shift that were pressed as part of the Ctrl+Shift+K shortcut).
        if (m_machine && m_machine->onKey) {
            for (const char* mod : {"control", "left_shift", "right_shift"})
                (void)m_machine->onKey(mod, false);
        }
    }
    SetStatusText(active
        ? "Keyboard captured \u2014 Ctrl+Shift+K or button to release."
        : "Keyboard released.");
}









void MmemuFrame::OnKbdFocus(const wxCommandEvent& event) {
    setKeyCapture(event.IsChecked());
}



void MmemuFrame::OnCtrlShiftK(wxKeyEvent& event) {
    if (event.GetKeyCode() == 'K' && event.ControlDown() && event.ShiftDown()) {
        setKeyCapture(!m_kbdFocus);
        return; // consumed
    }
    event.Skip();
}

// Built-in panes keep their View menu ids (and shortcuts); plugin panes get
// ids from ID_VIEW_PANE_FIRST on.
static const struct { const char* pane; int id; const char* shortcut; } kBuiltInViews[] = {
    {"breakpoints", ID_SHOW_BP_PANE,      "Ctrl-Shift-B"},
    {"symbols",     ID_SHOW_SYM_PANE,     "Ctrl-Y"},
    {"stack",       ID_SHOW_STACK_PANE,   "Ctrl-T"},
    {"trace",       ID_SHOW_TRACE_PANE,   "Ctrl-H"},
    {"machine",     ID_SHOW_MACHINE_PANE, "Ctrl-M"},
    {"devices",     ID_SHOW_DEVICE_PANE,  "Ctrl-D"},
    {"iowatch",     ID_SHOW_IOWATCH_PANE, "Ctrl-W"},
    {"cartridge",   ID_SHOW_CART_PANE,    nullptr},
    {"mega65",      ID_SHOW_MEGA65_PANE,  nullptr},
};

void MmemuFrame::rebuildViewMenu() {
    if (!m_viewMenu || !m_dock) return;
    while (m_viewMenu->GetMenuItemCount() > 0)
        m_viewMenu->Destroy(m_viewMenu->FindItemByPosition(0));
    m_viewIds.clear();

    for (const auto& p : m_dock->panes()) {
        if (!p.available) continue;
        int id = 0;
        const char* shortcut = nullptr;
        for (const auto& b : kBuiltInViews)
            if (p.id == b.pane) { id = b.id; shortcut = b.shortcut; }
        if (!id) {
            // Panes without a fixed id (console, cpu, memory, plugin panes)
            static std::map<std::string, int> s_assigned;
            auto it = s_assigned.find(p.id);
            if (it == s_assigned.end() && m_nextPluginViewId <= ID_VIEW_PANE_LAST)
                it = s_assigned.emplace(p.id, m_nextPluginViewId++).first;
            if (it == s_assigned.end()) continue;
            id = it->second;
        }
        wxString label = p.title;
        if (shortcut) label += wxString("\t") + shortcut;
        m_viewMenu->AppendCheckItem(id, label);
        m_viewIds[id] = p.id;
    }
    m_viewMenu->AppendSeparator();
    m_viewMenu->Append(ID_NEW_MEM_VIEW,    "New Memory View\tCtrl-Shift-M");
    m_viewMenu->Append(ID_RENAME_MEM_VIEW, "Rename Memory View...");
    m_viewMenu->AppendSeparator();
    m_viewMenu->Append(ID_VIEW_RESET_LAYOUT, "Reset Layout");
    syncViewMenu();
}

void MmemuFrame::syncViewMenu() {
    if (!m_viewMenu || !m_dock) return;
    for (const auto& [id, pane] : m_viewIds)
        if (wxMenuItem* item = m_viewMenu->FindItem(id)) item->Check(m_dock->isOpen(pane));
}

void MmemuFrame::OnViewPane(wxCommandEvent& event) {
    auto it = m_viewIds.find(event.GetId());
    if (it != m_viewIds.end()) m_dock->toggle(it->second);
}

void MmemuFrame::OnNewMemView(wxCommandEvent&) {
    wxString label = wxString::Format("Memory %zu", m_memPanes.size() + 1);
    addMemView(label);
    m_dock->open("memory");
}

void MmemuFrame::OnRenameMemView(wxCommandEvent&) {
    int sel = m_memNotebook->GetSelection();
    if (sel == wxNOT_FOUND) return;
    wxString current = m_memNotebook->GetPageText(sel);
    wxString name = wxGetTextFromUser("Enter a name for this memory view:", "Rename Memory View", current, this);
    if (!name.IsEmpty())
        m_memNotebook->SetPageText(sel, name);
}

void MmemuFrame::OnTimer(wxTimerEvent& event) {
    (void)event;
    if (m_running && m_machine && m_machine->schedulerStep) {
        // Run flat out for most of the timer period, then return so wx can
        // process input and the panes refresh (~30 Hz). No wxYield() inside the
        // slice: input waits at most one slice, and yielding every few steps
        // costs far more than the emulation. The clock is checked every 1024
        // steps (about 1 ms of emulation).
        int iters = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRunSliceMs);
        while (true) {
            m_machine->schedulerStep(*m_machine);
            ++iters;
            if (m_dbg && m_dbg->isPaused()) {
                m_running = false;
                SetStatusText(m_dbg->lastHitMessage());
                if (m_bpPane) m_bpPane->RefreshValues();
                updateSourceView();
                break;
            }
            // Check for program end (e.g., RTS on empty stack, JMP *, invalid BRK)
            if (m_cpu && m_cpu->isProgramEnd(m_bus)) {
                m_running = false;
                SetStatusText("Program ended.");
                updateSourceView();
                break;
            }
            if ((iters & 1023) == 0 && std::chrono::steady_clock::now() >= deadline)
                break;
        }
    }

    if (m_cpu) {
        bool isPaused = m_dbg && m_dbg->isPaused();

        m_regPane->RefreshValues();
        m_disasmPane->RefreshValues(m_cpu->pc());
        for (auto* p : m_memPanes) p->RefreshValues();
        for (auto* p : m_memPanes) p->UpdatePc(m_cpu->pc());

        if (!isPaused) {
            // CPU is running: tick plugins
            PluginPaneManager::instance().tickAll(m_cpu->cycles());
        }

        // History panes: refresh only when paused AND visible (not on every tick while running)
        if (m_sourcePane && m_sourcePane->IsShownOnScreen())
            m_sourcePane->RefreshBreakpointMarkers();   // only redraws if breakpoints changed
        if (isPaused) {
            if (m_stackPane && m_stackPane->IsShownOnScreen())
                m_stackPane->RefreshValues();
            if (m_tracePane && m_tracePane->IsShownOnScreen())
                m_tracePane->RefreshValues();
        }

        // Other expensive panes: only refresh when running to avoid continuous flicker
        if (!isPaused) {
            if (m_machineInspectorPane && m_machineInspectorPane->IsShownOnScreen())
                m_machineInspectorPane->refreshValues();
            if (m_deviceInfoPane && m_deviceInfoPane->IsShownOnScreen())
                m_deviceInfoPane->refreshValues();
            if (m_regWatchPane && m_regWatchPane->IsShownOnScreen())
                m_regWatchPane->refreshValues();
            if (m_mega65StatusPane && m_mega65StatusPane->IsShownOnScreen())
                m_mega65StatusPane->RefreshValues();
        }
    }
}








