#include "disk_loader.h"
#include "d64_parser.h"
#include "d71_parser.h"
#include "d80_parser.h"
#include "d81_parser.h"
#include "d82_parser.h"
#include "t64_parser.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/disk_file.h"
#include <algorithm>

bool DiskImageLoader::canLoad(const std::string& path) const {
    const std::string ext = diskfile::imageExt(path);   // x.d64.gz, a zip holding a .d64
    return ext == "d64" || ext == "d71" || ext == "d80" ||
           ext == "d81" || ext == "d82" || ext == "t64";
}

bool DiskImageLoader::load(const std::string& path, IBus* bus, MachineDescriptor* machine, uint32_t addr) {
    if (!bus && machine) {
        if (!machine->buses.empty()) {
            bus = machine->buses[0].bus;
        }
    }
    if (!bus) return false;

    const std::string ext = diskfile::imageExt(path);

    ICbmDiskImage* parser = nullptr;
    if      (ext == "d64") parser = new D64Parser();
    else if (ext == "d71") parser = new D71Parser();
    else if (ext == "d80") parser = new D80Parser();
    else if (ext == "d81") parser = new D81Parser();
    else if (ext == "d82") parser = new D82Parser();
    else if (ext == "t64") parser = new T64Parser();

    if (!parser) return false;

    bool success = false;
    if (parser->open(path)) {
        auto dir = parser->getDirectory();
        if (!dir.empty()) {
            std::vector<uint8_t> data;
            if (parser->readFile(dir[0].filename, data)) {
                if (data.size() >= 2) {
                    uint32_t loadAddr = data[0] | (data[1] << 8);
                    if (addr != 0) loadAddr = addr;

                    for (size_t i = 2; i < data.size(); ++i) {
                        bus->write8(loadAddr++, data[i]);
                    }
                    success = true;
                }
            }
        }
    }

    delete parser;
    return success;
}
