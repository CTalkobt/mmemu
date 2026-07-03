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
#include "dialogs/memory_dialogs.h"
#include "dialogs/assemble_dialog.h"
#include "dialogs/image_dialogs.h"
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
#include <fstream>
#include <vector>
#include <cstdio>

// mmemu - Multi Machine Emulator

class MmemuFrame : public wxFrame {
public:
    MmemuFrame();
    ~MmemuFrame() override { m_timer.Stop(); delete m_audio; delete m_dbg; }

    /** Toggle keyboard-capture mode and sync all UI + app key handler. */
    void setKeyCapture(bool active);

private:
    void OnLoadMachine(wxCommandEvent& event);
    void OnStep(wxCommandEvent& event);
    void OnBackStep(wxCommandEvent& event);
    void OnRun(wxCommandEvent& event);
    void OnPause(wxCommandEvent& event);
    void OnReset(wxCommandEvent& event);
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
    void OnShowBpPane(wxCommandEvent& event);
    void OnShowSymPane(wxCommandEvent& event);
    void OnShowStackPane(wxCommandEvent& event);
    void OnShowMachinePane(wxCommandEvent& event);
    void OnShowDevicePane(wxCommandEvent& event);
    void OnNewMemView(wxCommandEvent& event);
    void OnRenameMemView(wxCommandEvent& event);
    void OnLoadMachineDirect(wxCommandEvent& event);
    void loadMachineById(const std::string& id);
    void OnLoadImageDirect(wxCommandEvent& event);
    void OnTypeTextDirect(wxCommandEvent& event);
    void OnTimer(wxTimerEvent& event);
    void OnCtrlShiftK(wxKeyEvent& event);
    void ShowBreakpointPane() const;
    void ShowSymbolPane();
    void ShowStackPane() const;
    void ShowMachineInspectorPane() const;
    void ShowDeviceInfoPane();

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
    wxAuiNotebook*  m_notebook  = nullptr;

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
        parser.Found("type", &m_startType);
        m_autoRun = parser.Found("run");
        m_trace = parser.Found("trace");
        m_verbose = parser.Found("verbose");
        return true;
    }

    bool OnInit() override {
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

        return true;
    }

    int OnExit() override {
        wxEvtHandler::RemoveFilter(&m_keyFilter);
        return wxApp::OnExit();
    }

    KeyboardCaptureFilter m_keyFilter;
    wxString m_startMachine;
    wxString m_startMount;
    wxString m_startType;
    bool     m_autoRun = false;
    bool     m_verbose = false;
    bool     m_trace   = false;
};

wxIMPLEMENT_APP(MmemuApp);

MmemuFrame::MmemuFrame()
    : wxFrame(nullptr, wxID_ANY, "mmemu - Multi Machine Emulator",
              wxDefaultPosition, wxSize(1024, 768)),
      m_timer(this, ID_GUI_TIMER)
{
    // Menu
    auto* menuFile = new wxMenu;
    menuFile->Append(ID_LOAD_MACHINE, "Load Machine...\tCtrl-L");
    menuFile->AppendSeparator();
    menuFile->Append(wxID_EXIT);
    
    auto* menuControl = new wxMenu;
    menuControl->Append(ID_STEP, "Step\tF11");
    menuControl->Append(ID_BACKSTEP, "Back Step\tShift-F11");
    menuControl->Append(ID_RUN, "Run\tF5");
    menuControl->Append(ID_PAUSE, "Pause\tF6");
    menuControl->AppendSeparator();
    menuControl->Append(ID_RESET, "Reset\tCtrl-R");
    menuControl->AppendSeparator();
    menuControl->Append(ID_LOAD_IMAGE, "Load Image...\tCtrl-I");
    menuControl->Append(ID_ATTACH_CART, "Attach Cartridge...");
    menuControl->Append(ID_EJECT_CART, "Eject Cartridge");

    auto* menuDebug = new wxMenu;
    menuDebug->Append(ID_ASSEMBLE, "Assemble...\tCtrl-A");
    menuDebug->AppendSeparator();
    menuDebug->Append(ID_GOTO_ADDR, "Go to Address...\tCtrl-G");
    menuDebug->Append(ID_SEARCH_MEM, "Search Memory...\tCtrl-F");
    menuDebug->Append(ID_FIND_NEXT,  "Find Next\tF3");
    menuDebug->Append(ID_FIND_PRIOR, "Find Prior\tShift-F3");
    menuDebug->AppendSeparator();
    menuDebug->Append(ID_FILL_MEM, "Fill Memory...");
    menuDebug->Append(ID_COPY_MEM, "Copy Memory...");
    menuDebug->Append(ID_SWAP_MEM, "Swap Memory...");
    menuDebug->AppendSeparator();
    menuDebug->Append(ID_SHOW_BP_PANE,    "Breakpoints\tCtrl-B");
    menuDebug->Append(ID_SHOW_SYM_PANE,   "Symbols\tCtrl-Y");
    menuDebug->Append(ID_SHOW_STACK_PANE, "Stack Trace\tCtrl-T");
    menuDebug->Append(ID_SHOW_MACHINE_PANE, "Machine Explorer\tCtrl-M");
    menuDebug->Append(ID_SHOW_DEVICE_PANE, "Device Info\tCtrl-D");
    menuDebug->AppendSeparator();
    menuDebug->Append(ID_NEW_MEM_VIEW,    "New Memory View\tCtrl-Shift-M");
    menuDebug->Append(ID_RENAME_MEM_VIEW, "Rename Memory View...");
    
    auto* menuTools = new wxMenu;
    menuTools->Append(ID_CALCULATOR, "Calculator\tCtrl-Shift-C");
    menuTools->AppendSeparator();
    menuTools->Append(ID_TOOL_PROFILE, "Profile CPU...");
    menuTools->Append(ID_TOOL_MEASURE, "Measure Region...");
    menuTools->Append(ID_TOOL_RUNTO,   "Run Until...");
    menuTools->Append(ID_TOOL_DEVINFO, "Device Info...");

    auto* menuBar = new wxMenuBar;
    menuBar->Append(menuFile, "&File");
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
    
    // Main layout: Overall horizontal split
    auto* mainSplitter = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE);
    
    // Left Column: Disasm, Memory, and Bottom Row (Registers + CLI)
    // Split into Top (Disasm) and Bottom (Memory + Bottom Row)
    auto* leftTopBottomSplitter = new wxSplitterWindow(mainSplitter, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE);
    m_disasmPane = new DisasmPane(leftTopBottomSplitter);
    
    // Split Bottom into Memory and Bottom Row
    auto* leftMemoryBottomRowSplitter = new wxSplitterWindow(leftTopBottomSplitter, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE);
    m_memNotebook = new wxAuiNotebook(leftMemoryBottomRowSplitter, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxAUI_NB_TOP | wxAUI_NB_SCROLL_BUTTONS | wxAUI_NB_CLOSE_ON_ALL_TABS | wxAUI_NB_TAB_MOVE);
    addMemView("Memory 1");
    m_memNotebook->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, [this](wxAuiNotebookEvent& event) {
        if (m_memPanes.size() <= 1) { event.Veto(); return; }
        int page = event.GetSelection();
        if (page != wxNOT_FOUND && page < (int)m_memPanes.size())
            m_memPanes.erase(m_memPanes.begin() + page);
    });
    
    // Split Bottom Row into Registers and CLI
    auto* bottomRowSplitter = new wxSplitterWindow(leftMemoryBottomRowSplitter, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE);
    m_regPane = new RegisterPane(bottomRowSplitter);
    m_consolePane = new ConsolePane(bottomRowSplitter);
    
    bottomRowSplitter->SplitVertically(m_regPane, m_consolePane, 200);
    leftMemoryBottomRowSplitter->SplitHorizontally(m_memNotebook, bottomRowSplitter, 300);
    leftTopBottomSplitter->SplitHorizontally(m_disasmPane, leftMemoryBottomRowSplitter, 250);
    
    // Right side: Tool Notebook
    m_notebook = new wxAuiNotebook(mainSplitter, wxID_ANY);
    m_machineInspectorPane = new MachineInspectorPane(m_notebook);
    m_notebook->AddPage(m_machineInspectorPane, "Machine");
    m_deviceInfoPane = new DeviceInfoPane(m_notebook);
    m_notebook->AddPage(m_deviceInfoPane, "Devices");
    m_regWatchPane = new RegisterWatchPane(m_notebook);
    m_notebook->AddPage(m_regWatchPane, "I/O Watch");
    m_tracePane = new TracePane(m_notebook);
    m_notebook->AddPage(m_tracePane, "Trace");
    m_cartPane = new CartridgePane(m_notebook);
    m_notebook->AddPage(m_cartPane, "Cartridge");
    m_bpPane = new BreakpointPane(m_notebook);
    m_notebook->AddPage(m_bpPane, "Breakpoints");
    m_symPane = new SymbolPane(m_notebook);
    m_notebook->AddPage(m_symPane, "Symbols");
    m_stackPane = new StackPane(m_notebook);
    m_notebook->AddPage(m_stackPane, "Stack");
    m_mega65StatusPane = new Mega65StatusPane(m_notebook);

    mainSplitter->SplitVertically(leftTopBottomSplitter, m_notebook, 750);
    mainSplitter->SetMinimumPaneSize(200);
    
    SetDropTarget(new MmemuDropTarget(this));
    
    // Bind events
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(true); }, wxID_EXIT);
    Bind(wxEVT_MENU, &MmemuFrame::OnLoadMachine, this, ID_LOAD_MACHINE);
    Bind(wxEVT_TOOL, &MmemuFrame::OnLoadMachine, this, ID_LOAD_MACHINE);
    Bind(wxEVT_MENU, &MmemuFrame::OnStep, this, ID_STEP);
    Bind(wxEVT_TOOL, &MmemuFrame::OnStep, this, ID_STEP);
    Bind(wxEVT_MENU, &MmemuFrame::OnBackStep, this, ID_BACKSTEP);
    Bind(wxEVT_TOOL, &MmemuFrame::OnBackStep, this, ID_BACKSTEP);
    Bind(wxEVT_MENU, &MmemuFrame::OnRun, this, ID_RUN);
    Bind(wxEVT_TOOL, &MmemuFrame::OnRun, this, ID_RUN);
    Bind(wxEVT_MENU, &MmemuFrame::OnPause, this, ID_PAUSE);
    Bind(wxEVT_TOOL, &MmemuFrame::OnPause, this, ID_PAUSE);
    Bind(wxEVT_MENU, &MmemuFrame::OnReset, this, ID_RESET);
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
    Bind(wxEVT_MENU, &MmemuFrame::OnShowBpPane,    this, ID_SHOW_BP_PANE);
    Bind(wxEVT_MENU, &MmemuFrame::OnShowSymPane,   this, ID_SHOW_SYM_PANE);
    Bind(wxEVT_MENU, &MmemuFrame::OnShowStackPane, this, ID_SHOW_STACK_PANE);
    Bind(wxEVT_MENU, &MmemuFrame::OnShowMachinePane, this, ID_SHOW_MACHINE_PANE);
    Bind(wxEVT_MENU, &MmemuFrame::OnShowDevicePane, this, ID_SHOW_DEVICE_PANE);
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

    Bind(wxEVT_TOOL, &MmemuFrame::OnKbdFocus, this, ID_KBD_FOCUS);
    Bind(wxEVT_TIMER, &MmemuFrame::OnTimer, this, ID_GUI_TIMER);
    
    Bind(wxEVT_MENU, &MmemuFrame::OnLoadMachineDirect, this, ID_LOAD_MACHINE_DIRECT);
    Bind(wxEVT_MENU, &MmemuFrame::OnLoadImageDirect,   this, ID_LOAD_IMAGE_DIRECT);
    Bind(wxEVT_MENU, &MmemuFrame::OnTypeTextDirect,    this, ID_TYPE_TEXT_DIRECT);

    // Ctrl+Shift+K toggles capture; FilterEvent routes all other keys.
    Bind(wxEVT_CHAR_HOOK, &MmemuFrame::OnCtrlShiftK, this);

    m_timer.Start(33); // ~30 FPS
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
    if (!md) return;

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

    // Show/hide MEGA65 status pane based on machine type
    {
        bool isMega65 = (id.find("mega65") != std::string::npos);
        for (size_t i = 0; i < m_notebook->GetPageCount(); ++i) {
            if (m_notebook->GetPage(i) == m_mega65StatusPane) {
                m_notebook->RemovePage(i);
                break;
            }
        }
        if (isMega65) {
            m_mega65StatusPane->SetMachine(m_machine, m_cpu);
            m_notebook->AddPage(m_mega65StatusPane, "MEGA65");
        }
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

    PluginPaneManager::instance().onMachineSwitch(id, m_notebook, m_notebook, m_machine);

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

void MmemuFrame::OnStep(wxCommandEvent& event) {
    (void)event;
    if (m_cpu) {
        m_cpu->step();
        m_regPane->RefreshValues();
        m_disasmPane->RefreshValues(m_cpu->pc());
        m_stackPane->RefreshValues();
        for (auto* p : m_memPanes) p->UpdatePc(m_cpu->pc());
    }
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
    m_running = true;
    SetStatusText("Running...");
}

void MmemuFrame::OnPause(wxCommandEvent& event) {
    (void)event;
    m_running = false;
    SetStatusText("Paused.");
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

void MmemuFrame::OnFillMem(wxCommandEvent& event) {
    (void)event;
    if (!m_bus) return;
    FillMemoryDialog dialog(this, m_dbg);
    if (dialog.ShowModal() == wxID_OK) {
        uint32_t addr = dialog.GetAddress();
        uint32_t len = dialog.GetLength();
        uint8_t val = dialog.GetValue();
        for (uint32_t i = 0; i < len; ++i) m_bus->write8(addr + i, val);
        for (auto* p : m_memPanes) p->RefreshValues();
    }
}

void MmemuFrame::OnCopyMem(wxCommandEvent& event) {
    (void)event;
    if (!m_bus) return;
    CopyMemoryDialog dialog(this, m_dbg);
    if (dialog.ShowModal() == wxID_OK) {
        uint32_t src = dialog.GetSrcAddress();
        uint32_t len = dialog.GetLength();
        uint32_t dst = dialog.GetDstAddress();
        std::vector<uint8_t> tmp(len);
        for (uint32_t i = 0; i < len; ++i) tmp[i] = m_bus->read8(src + i);
        for (uint32_t i = 0; i < len; ++i) m_bus->write8(dst + i, tmp[i]);
        for (auto* p : m_memPanes) p->RefreshValues();
    }
}

void MmemuFrame::OnSwapMem(wxCommandEvent& event) {
    (void)event;
    if (!m_bus) return;
    SwapMemoryDialog dialog(this, m_dbg);
    if (dialog.ShowModal() == wxID_OK) {
        uint32_t addr1 = dialog.GetAddress1();
        uint32_t len = dialog.GetLength();
        uint32_t addr2 = dialog.GetAddress2();
        for (uint32_t i = 0; i < len; ++i) {
            uint8_t v1 = m_bus->read8(addr1 + i);
            uint8_t v2 = m_bus->read8(addr2 + i);
            m_bus->write8(addr1 + i, v2);
            m_bus->write8(addr2 + i, v1);
        }
        for (auto* p : m_memPanes) p->RefreshValues();
    }
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
    GotoAddressDialog dialog(this, m_cpu->pc(), m_dbg);
    if (dialog.ShowModal() == wxID_OK) {
        uint32_t addr = dialog.GetAddress();
        if (dialog.ShouldSetPC()) {
            m_cpu->setPc(addr);
            m_disasmPane->RefreshValues(m_cpu->pc());
        }
        if (auto* _mp = activeMemPane()) _mp->SetAddress(addr);
        for (auto* p : m_memPanes) p->RefreshValues();
    }
}

void MmemuFrame::OnSearchMem(wxCommandEvent& event) {
    (void)event;
    if (!m_cpu) return;
    uint32_t mask = m_bus->config().addrMask;
    SearchMemoryDialog dialog(this, mask, m_dbg);
    if (dialog.ShowModal() == wxID_OK) {
        std::string pattern = dialog.GetPattern();
        bool isHex = dialog.IsHex();
        uint32_t startAddr = dialog.GetStartAddress() & mask;
        uint32_t length = dialog.GetLength();
        
        m_lastSearchPattern.clear();
        if (isHex) {
            std::string cleanHex;
            for (char c : pattern) { if (isxdigit(c)) cleanHex += c; }
            for (size_t i = 0; i + 1 < cleanHex.length(); i += 2) {
                try {
                    m_lastSearchPattern.push_back((uint8_t)std::stoul(cleanHex.substr(i, 2), nullptr, 16));
                } catch (...) {}
            }
        } else {
            for (char c : pattern) m_lastSearchPattern.push_back((uint8_t)c);
        }
        
        if (m_lastSearchPattern.empty()) return;
        
        uint32_t endAddr = (startAddr + length > mask + 1) ? (mask + 1) : (startAddr + length);
        uint32_t foundAddr = 0xFFFFFFFF;
        
        if (endAddr >= m_lastSearchPattern.size()) {
            for (uint32_t i = startAddr; i <= endAddr - m_lastSearchPattern.size(); ++i) {
                bool match = true;
                for (size_t j = 0; j < m_lastSearchPattern.size(); ++j) {
                    if (m_bus->peek8((i + j) & mask) != m_lastSearchPattern[j]) {
                        match = false; break;
                    }
                }
                if (match) { foundAddr = i; break; }
            }
        }
        
        m_lastSearchFoundAddr = foundAddr;
        if (foundAddr != 0xFFFFFFFF) {
            if (auto* _mp = activeMemPane()) _mp->SetAddress(foundAddr);
            for (auto* p : m_memPanes) p->RefreshValues();
            SetStatusText(wxString::Format("Pattern found at $%X", foundAddr));
        } else {
            wxMessageBox("Pattern not found", "Search", wxOK | wxICON_INFORMATION);
        }
    }
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
    if (!m_bus) return;
    std::string path = event.GetString().ToStdString();
    LoadImageDialog dialog(this, path);
    if (dialog.ShowModal() == wxID_OK) {
        path = dialog.GetPath();
        uint32_t addr = dialog.GetAddress();
        IBus* loadBus = m_machine->buses[0].bus; // physical bus (#79)
        auto* loader = ImageLoaderRegistry::instance().findLoader(path);
        if (loader) {
            if (loader->load(path, loadBus, m_machine, addr)) {
                SetStatusText("Loaded " + path);
                if (dialog.GetAutoStart()) {
                    uint32_t startAddr = addr;
                    if (startAddr == 0 && std::string(loader->name()).find("PRG") != std::string::npos) {
                        std::ifstream f(path, std::ios::binary);
                        uint8_t h[2];
                        f.read((char*)h, 2);
                        startAddr = h[0] | (h[1] << 8);
                    }
                    m_cpu->setPc(startAddr);
                    m_running = true;
                    m_regPane->RefreshValues();
                    m_disasmPane->RefreshValues(m_cpu->pc());
                }
                for (auto* p : m_memPanes) p->RefreshValues();
            } else {
                wxMessageBox("Failed to load image.", "Error", wxICON_ERROR);
            }
        }
    }
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

void MmemuFrame::ShowBreakpointPane() const {
    // Find the pane in the notebook (it may have been closed by the user).
    for (size_t i = 0; i < m_notebook->GetPageCount(); ++i) {
        if (m_notebook->GetPage(i) == m_bpPane) {
            m_notebook->SetSelection(i);
            return;
        }
    }
    // Not present — re-add it.
    m_notebook->AddPage(m_bpPane, "Breakpoints", true);
}

void MmemuFrame::OnShowBpPane(wxCommandEvent&) {
    ShowBreakpointPane();
}

void MmemuFrame::ShowSymbolPane() {
    for (size_t i = 0; i < m_notebook->GetPageCount(); ++i) {
        if (m_notebook->GetPage(i) == m_symPane) {
            m_notebook->SetSelection(i);
            return;
        }
    }
    m_notebook->AddPage(m_symPane, "Symbols", true);
}

void MmemuFrame::OnShowSymPane(wxCommandEvent&) {
    ShowSymbolPane();
}

void MmemuFrame::ShowStackPane() const {
    for (size_t i = 0; i < m_notebook->GetPageCount(); ++i) {
        if (m_notebook->GetPage(i) == m_stackPane) {
            m_notebook->SetSelection(i);
            return;
        }
    }
    m_notebook->AddPage(m_stackPane, "Stack", true);
}

void MmemuFrame::OnShowStackPane(wxCommandEvent&) {
    ShowStackPane();
}

void MmemuFrame::OnKbdFocus(const wxCommandEvent& event) {
    setKeyCapture(event.IsChecked());
}

void MmemuFrame::ShowMachineInspectorPane() const {
    for (size_t i = 0; i < m_notebook->GetPageCount(); ++i) {
        if (m_notebook->GetPage(i) == m_machineInspectorPane) {
            m_notebook->SetSelection(i);
            return;
        }
    }
    m_notebook->AddPage(m_machineInspectorPane, "Machine", true);
}

void MmemuFrame::OnShowMachinePane(wxCommandEvent&) {
    ShowMachineInspectorPane();
}

void MmemuFrame::OnCtrlShiftK(wxKeyEvent& event) {
    if (event.GetKeyCode() == 'K' && event.ControlDown() && event.ShiftDown()) {
        setKeyCapture(!m_kbdFocus);
        return; // consumed
    }
    event.Skip();
}

void MmemuFrame::OnNewMemView(wxCommandEvent&) {
    wxString label = wxString::Format("Memory %zu", m_memPanes.size() + 1);
    addMemView(label);
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
        // Run as many cycles as possible within 25ms to keep the UI responsive.
        // This adapts automatically to any CPU clock speed (1 MHz or 40 MHz).
        int iters = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(25);
        while (true) {
            m_machine->schedulerStep(*m_machine);
            ++iters;
            if (m_dbg && m_dbg->isPaused()) {
                m_running = false;
                SetStatusText(m_dbg->lastHitMessage());
                if (m_bpPane) m_bpPane->RefreshValues();
                break;
            }
            // Check deadline every 256 iterations to avoid clock_now() overhead
            if ((iters & 0xFF) == 0 && std::chrono::steady_clock::now() >= deadline)
                break;
        }
    }

    if (m_cpu) {
        m_regPane->RefreshValues();
        m_disasmPane->RefreshValues(m_cpu->pc());
        for (auto* p : m_memPanes) p->RefreshValues();
        for (auto* p : m_memPanes) p->UpdatePc(m_cpu->pc());
        m_stackPane->RefreshValues();
        PluginPaneManager::instance().tickAll(m_cpu->cycles());
        
        if (m_machineInspectorPane && m_notebook->GetCurrentPage() == m_machineInspectorPane)
            m_machineInspectorPane->refreshValues();
        if (m_deviceInfoPane && m_notebook->GetCurrentPage() == m_deviceInfoPane)
            m_deviceInfoPane->refreshValues();
        if (m_regWatchPane && m_notebook->GetCurrentPage() == m_regWatchPane)
            m_regWatchPane->refreshValues();
        if (m_tracePane && m_notebook->GetCurrentPage() == m_tracePane)
            m_tracePane->RefreshValues();
        if (m_mega65StatusPane && m_notebook->GetCurrentPage() == m_mega65StatusPane)
            m_mega65StatusPane->RefreshValues();
    }
}

void MmemuFrame::ShowDeviceInfoPane() {
    for (size_t i = 0; i < m_notebook->GetPageCount(); ++i) {
        if (m_notebook->GetPage(i) == m_deviceInfoPane) {
            m_notebook->SetSelection(i);
            return;
        }
    }
    m_notebook->AddPage(m_deviceInfoPane, "Devices", true);
}

void MmemuFrame::OnShowDevicePane(wxCommandEvent&) {
    ShowDeviceInfoPane();
}
