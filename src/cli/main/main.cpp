#include <iostream>
#include <string>
#include <memory>
#include <csignal>
#include <cstdlib>
#include "cli_interpreter.h"
#include "gdb_server.h"
#include "serial_monitor_server.h"
#include "vice_monitor_server.h"
#include "plugin_loader/main/plugin_loader.h"
#include "plugin_command_registry.h"
#include "include/util/logging.h"
#include "include/version.h"
#include "libcore/main/json_machine_loader.h"

// Defined in cli_interpreter.cpp
extern volatile sig_atomic_t g_interrupted;
static void sigintHandler(int) { g_interrupted = 1; }

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    // Install SIGINT handler so Ctrl-C breaks out of run loops
    struct sigaction sa = {};
    sa.sa_handler = sigintHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    std::cout << "mmemu - Multi Machine Emulator (CLI)\n";
    std::cout << "Version " MMSIM_VERSION_FULL "\n";

    LogRegistry::instance().init();

    // Parse verbosity flags and experimental mode early so logging is active for plugin loading
    int verbosity = 0;
    bool experimentalMode = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-vv" || arg == "--trace")   { verbosity = 2; }
        else if (arg == "-v" || arg == "--verbose") { if (verbosity < 1) verbosity = 1; }
        else if (arg == "--experimental") { experimentalMode = true; }
    }
    if (verbosity >= 2)
        LogRegistry::instance().setGlobalLevel(spdlog::level::trace);
    else if (verbosity >= 1)
        LogRegistry::instance().setGlobalLevel(spdlog::level::debug);

    // Set experimental mode via environment variable for machine factory to pick up
    if (experimentalMode)
        setenv("MMSIM_EXPERIMENTAL_PREFIX", "1", 1);

    PluginLoader::instance().setCommandRegisterFn([](const PluginCommandInfo* info) {
        PluginCommandRegistry::instance().registerCommand(info);
    });
    
    // Register built-ins to prevent collisions
    const char* builtIns[] = {"help", "?", "list", "create", "reset", "step", "backstep", "bs", "setpc", "regs", "m", "f", "copy", "search", "searcha", "findnext", "findprior", "disasm", "asm", "type", "key", "load", "quit", "q", nullptr};
    for (int i = 0; builtIns[i]; ++i) {
        PluginCommandRegistry::instance().registerBuiltIn(builtIns[i]);
    }
    
    // Process command line args early (especially for help)
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h" || arg == "-?") {
            std::cout << "Usage: mmemu-cli [options]\n"
                      << "Options:\n"
                      << "  -m, --machine <id>        Create a machine on startup\n"
                      << "  -i, --mount <path>        Mount a disk/tape/program image\n"
                      << "  -t, --type <text>         Type text into the machine\n"
                      << "  --run                     Auto-start the loaded program\n"
                      << "  --gdb-port <port>         Start GDB RSP server on <port>\n"
                      << "  --serial-monitor-port <p> Start MEGA65 serial monitor server on <p>\n"
                      << "  --vice-monitor-port <p>   Start VICE protocol monitor server on <p> (default: 6510)\n"
                      << "  --experimental            Enable experimental features (45GS02 prefix peek-ahead,\n"
                      << "                            F018B DMA SWAP/MIX/MODULO operations)\n"
                      << "  -v, --verbose             Enable debug logging\n"
                      << "  -vv, --trace              Enable trace logging (very verbose)\n"
                      << "  -h, -?, --help            Show this help\n";
            return 0;
        }
    }

    PluginLoader::instance().loadFromStandardLocations();

    // Load JSON machines after all plugins are registered
    {
        JsonMachineLoader jsonLoader;
        jsonLoader.loadFile("machines/rawMega65.json");
    }

    CliContext ctx;
    CliInterpreter interpreter(ctx, [](const std::string& out) {
        std::cout << out;
        std::cout.flush();
    });

    // Process other command line args (machine, mount, type, gdb, run, serial-monitor, vice-monitor)
    uint16_t gdbPort = 0;
    uint16_t serialMonitorPort = 0;
    uint16_t viceMonitorPort = 0;
    bool autoRun = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--machine" || arg == "-m") && i + 1 < argc) {
            interpreter.processLine("create " + std::string(argv[++i]));
        } else if ((arg == "--mount" || arg == "-i") && i + 1 < argc) {
            interpreter.processLine("load " + std::string(argv[++i]));
        } else if ((arg == "--type" || arg == "-t") && i + 1 < argc) {
            interpreter.processLine("type " + std::string(argv[++i]));
        } else if (arg == "--run") {
            autoRun = true;
        } else if (arg == "--gdb-port" && i + 1 < argc) {
            gdbPort = std::stoi(argv[++i]);
        } else if (arg == "--serial-monitor-port" && i + 1 < argc) {
            serialMonitorPort = std::stoi(argv[++i]);
        } else if (arg == "--vice-monitor-port" && i + 1 < argc) {
            viceMonitorPort = std::stoi(argv[++i]);
        }
    }

    // Auto-start: run from the loaded PRG address
    if (autoRun && ctx.cpu) {
        interpreter.processLine("run");
    }

    // Start GDB server if requested
    std::unique_ptr<GdbServer> gdbServer;
    if (gdbPort > 0 && ctx.cpu && ctx.bus) {
        gdbServer = std::make_unique<GdbServer>(ctx.cpu, ctx.bus, ctx.dbg);
        if (gdbServer->start(gdbPort)) {
            std::cout << "GDB server listening on port " << gdbPort << "\n";
        } else {
            std::cerr << "Error: Failed to start GDB server on port " << gdbPort << "\n";
            gdbServer.reset();
        }
    } else if (gdbPort > 0) {
        std::cerr << "Warning: --gdb-port requires a machine (-m). GDB server not started.\n";
    }

    // Start Serial Monitor server if requested
    std::unique_ptr<SerialMonitorServer> serialMonitorServer;
    if (serialMonitorPort > 0 && ctx.cpu && ctx.bus) {
        serialMonitorServer = std::make_unique<SerialMonitorServer>(ctx.cpu, ctx.bus, ctx.dbg);
        if (serialMonitorServer->start(serialMonitorPort)) {
            std::cout << "Serial Monitor server listening on port " << serialMonitorPort << "\n";
        } else {
            std::cerr << "Error: Failed to start Serial Monitor server on port " << serialMonitorPort << "\n";
            serialMonitorServer.reset();
        }
    } else if (serialMonitorPort > 0) {
        std::cerr << "Warning: --serial-monitor-port requires a machine (-m). Server not started.\n";
    }

    // Start VICE Monitor server if requested
    std::unique_ptr<ViceMonitorServer> viceMonitorServer;
    if (viceMonitorPort > 0 && ctx.cpu && ctx.bus) {
        viceMonitorServer = std::make_unique<ViceMonitorServer>(ctx.cpu, ctx.bus, ctx.dbg);
        if (viceMonitorServer->start(viceMonitorPort)) {
            std::cout << "VICE Monitor server listening on port " << viceMonitorPort << " (VICE protocol compatible)\n";
        } else {
            std::cerr << "Error: Failed to start VICE Monitor server on port " << viceMonitorPort << "\n";
            viceMonitorServer.reset();
        }
    } else if (viceMonitorPort > 0) {
        std::cerr << "Warning: --vice-monitor-port requires a machine (-m). Server not started.\n";
    }

    std::cout << "Type 'help' for a list of commands.\n";
    
    std::string line;
    while (!ctx.quit) {
        if (interpreter.isAssemblyMode()) {
            std::cout << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << interpreter.getAsmAddr() << "> ";
        } else {
            std::cout << "> ";
        }
        
        if (!std::getline(std::cin, line)) break;
        interpreter.processLine(line);
    }

    PluginLoader::instance().unloadAll();
    return 0;
}
