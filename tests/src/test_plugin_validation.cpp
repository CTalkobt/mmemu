#include "test_harness.h"
#include "libcore/main/core_registry.h"
#include "libcore/main/machines/machine_registry.h"
#include "libdevices/main/device_registry.h"
#include "libtoolchain/main/toolchain_registry.h"
#include "plugin_loader/main/plugin_loader.h"
#include <iostream>
#include <vector>
#include <string>
#include <stdlib.h>

static void loadAllPlugins() {
    PluginLoader::instance().loadFromDir("./lib");
}

TEST_CASE(validate_all_plugins) {
    loadAllPlugins();

    setenv("XEMU_NO_DIALOGS", "1", 1); 

    std::cout << "Validating Cores..." << std::endl;
    std::vector<CoreRegistry::CoreInfo> cores;
    CoreRegistry::instance().enumerate(cores);
    for (const auto& info : cores) {
        ICore* core = CoreRegistry::instance().createCore(info.name);
        ASSERT(core != nullptr);
        ASSERT(core->isaName() != nullptr);
        std::cout << "  Core: " << info.name << " (ISA: " << core->isaName() << ")" << std::endl;
        delete core;
    }

    std::cout << "Validating Toolchains..." << std::endl;
    // We can't easily enumerate toolchains because enumerate is private/missing or just returns ISA names
    // Let's assume we can at least validate those used by cores
    for (const auto& info : cores) {
        IDisassembler* dis = ToolchainRegistry::instance().createDisassembler(info.name);
        if (dis) {
            ASSERT(dis->isaName() != nullptr);
            std::cout << "  Disassembler for " << info.name << ": " << dis->isaName() << std::endl;
            delete dis;
        }
    }

    std::cout << "Validating Devices..." << std::endl;
    std::vector<std::string> deviceTypes;
    DeviceRegistry::instance().enumerate(deviceTypes);
    for (const auto& type : deviceTypes) {
        IOHandler* device = DeviceRegistry::instance().createDevice(type);
        ASSERT(device != nullptr);
        const char* name = device->name();
        if (name == nullptr) {
            std::cerr << "CRITICAL: Device type '" << type << "' returned NULL for name()!" << std::endl;
        }
        ASSERT(name != nullptr);
        std::cout << "  Device: " << type << " (Instance name: " << name << ")" << std::endl;
        delete device;
    }

    std::cout << "Validating Machines..." << std::endl;
    std::vector<std::string> machineIds;
    MachineRegistry::instance().enumerate(machineIds);
    std::cout << "Total machines to validate: " << machineIds.size() << std::endl;
    for (const auto& id : machineIds) {
        std::cout << "  Creating machine: " << id << std::flush << std::endl;
        std::cerr << "[DEBUG] About to create machine: " << id << std::endl;
        std::cerr.flush();
        MachineDescriptor* machine = MachineRegistry::instance().createMachine(id);
        std::cout << "  Created machine: " << id << std::flush << std::endl;
        ASSERT(machine != nullptr);
        ASSERT(!machine->machineId.empty());
        // Every plugin's machine names its vendor (the GUI's machine tree
        // groups by it; other tests register throwaway machines without)
        const std::string vendor = MachineRegistry::instance().vendorOf(id);
        if (!MachineRegistry::instance().sourceOf(id).empty()) {
            if (vendor.empty())
                std::cerr << "Machine '" << id << "' (" << MachineRegistry::instance().sourceOf(id)
                          << ") was registered without a vendor" << std::endl;
            ASSERT(!vendor.empty());
        }
        if (!vendor.empty()) ASSERT_EQ(machine->vendor, vendor);
        std::cout << "  Machine: " << id << " (Descriptor ID: " << machine->machineId << ")" << std::endl;
        
        // Also check all devices in the machine
        if (machine->ioRegistry) {
            std::vector<IOHandler*> handlers;
            machine->ioRegistry->enumerate(handlers);
            std::cout << "    Devices in machine: " << handlers.size() << std::endl;
            for (size_t i = 0; i < handlers.size(); ++i) {
                std::cout << "      [" << i << "] Checking device pointer..." << std::flush;
                auto* h = handlers[i];
                if (h == nullptr) {
                    std::cerr << " NULL!" << std::endl;
                    ASSERT(h != nullptr);
                }
                std::cout << " (0x" << std::hex << (uintptr_t)h << std::dec << ") calling name()..." << std::flush;
                const char* hname = h->name();
                std::cout << " got 0x" << std::hex << (uintptr_t)hname << std::dec << std::endl;
                if (hname == nullptr) {
                    std::cerr << "CRITICAL: Machine '" << id << "' device[" << i << "] has NULL name!" << std::endl;
                }
                ASSERT(hname != nullptr);
                std::cout << "      [" << i << "] Device name: " << hname << std::endl;
            }
        }
        
        delete machine;
    }
}
