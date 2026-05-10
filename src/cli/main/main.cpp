#include <iostream>
#include <string>
#include "cli_interpreter.h"
#include "plugin_loader/main/plugin_loader.h"
#include "plugin_command_registry.h"
#include "include/util/logging.h"
#include "libcore/main/json_machine_loader.h"

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    std::cout << "mmemu - Multi Machine Emulator (CLI)\n";
    std::cout << "Version 0.1.0-dev\n";
    
    LogRegistry::instance().init();

    PluginLoader::instance().setCommandRegisterFn([](const PluginCommandInfo* info) {
        PluginCommandRegistry::instance().registerCommand(info);
    });
    
    // Register built-ins to prevent collisions
    const char* builtIns[] = {"help", "?", "list", "create", "reset", "step", "setpc", "regs", "m", "f", "copy", "search", "searcha", "findnext", "findprior", "disasm", "asm", "type", "key", "load", "quit", "q", nullptr};
    for (int i = 0; builtIns[i]; ++i) {
        PluginCommandRegistry::instance().registerBuiltIn(builtIns[i]);
    }
    
    // Process command line args early (especially for help)
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h" || arg == "-?") {
            std::cout << "Usage: mmemu-cli [options]\n"
                      << "Options:\n"
                      << "  -m, --machine <id>  Create a machine on startup\n"
                      << "  -i, --mount <path>  Mount a disk/tape/program image\n"
                      << "  -t, --type <text>   Type text into the machine\n"
                      << "  -h, -?, --help      Show this help\n";
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

    // Process other command line args (machine, mount, type)
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--machine" || arg == "-m") && i + 1 < argc) {
            interpreter.processLine("create " + std::string(argv[++i]));
        } else if ((arg == "--mount" || arg == "-i") && i + 1 < argc) {
            interpreter.processLine("load " + std::string(argv[++i]));
        } else if ((arg == "--type" || arg == "-t") && i + 1 < argc) {
            interpreter.processLine("type " + std::string(argv[++i]));
        }
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
