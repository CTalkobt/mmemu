#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <algorithm>
#include <cctype>

#include "minijson.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/machines/machine_registry.h"
#include "libcore/main/image_loader.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "plugin_loader/main/plugin_loader.h"
#include "libmem/main/memory_bus.h"
#include "libcore/main/icore.h"
#include "libtoolchain/main/toolchain_registry.h"
#include "libtoolchain/main/idisasm.h"
#include "libdebug/main/debug_context.h"
#include "libdebug/main/breakpoint_list.h"
#include "libdebug/main/stack_trace.h"
#include "libdebug/main/expression_evaluator.h"

static bool resolveAddr(const Json& val, DebugContext* dbg, uint32_t& result) {
    if (val.type == Json::NUM) {
        result = (uint32_t)val.nVal;
        return true;
    }
    if (val.type == Json::STR) {
        return ExpressionEvaluator::evaluate(val.sVal, dbg, result);
    }
    return false;
}

#include "plugin_tool_registry.h"
#include "include/util/logging.h"

static std::string toHex(uint32_t v, int width = 4) {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0') << std::setw(width) << v;
    return ss.str();
}

struct MachineState {
    MachineDescriptor* machine = nullptr;
    ICore*             cpu     = nullptr;
    IBus*              bus     = nullptr;
    IDisassembler*     disasm  = nullptr;
    DebugContext*      dbg     = nullptr;
    std::string        id;
    std::vector<uint8_t> lastSearchPattern;
    uint32_t             lastSearchFoundAddr = 0xFFFFFFFF;

    ~MachineState() {
        // MachineDescriptor owns cpu and bus
        delete dbg;
        delete disasm;
        delete machine;
    }
    
    // Disable copy since we own pointers
    MachineState() = default;
    MachineState(const MachineState&) = delete;
    MachineState& operator=(const MachineState&) = delete;
    MachineState(MachineState&& o) noexcept {
        machine = o.machine; o.machine = nullptr;
        cpu = o.cpu; o.cpu = nullptr;
        bus = o.bus; o.bus = nullptr;
        disasm = o.disasm; o.disasm = nullptr;
        dbg = o.dbg; o.dbg = nullptr;
        id = std::move(o.id);
        lastSearchPattern = std::move(o.lastSearchPattern);
        lastSearchFoundAddr = o.lastSearchFoundAddr;
    }
    MachineState& operator=(MachineState&& o) noexcept {
        if (this != &o) {
            delete dbg; delete disasm; delete machine;
            machine = o.machine; o.machine = nullptr;
            cpu = o.cpu; o.cpu = nullptr;
            bus = o.bus; o.bus = nullptr;
            disasm = o.disasm; o.disasm = nullptr;
            dbg = o.dbg; o.dbg = nullptr;
            id = std::move(o.id);
            lastSearchPattern = std::move(o.lastSearchPattern);
            lastSearchFoundAddr = o.lastSearchFoundAddr;
        }
        return *this;
    }
};

static std::map<std::string, MachineState> g_machines;

static MachineState* getMachine(const std::string& id) {
    if (g_machines.count(id)) return &g_machines[id];
    
    auto* desc = MachineRegistry::instance().createMachine(id);
    if (desc && !desc->cpus.empty() && desc->cpus[0].cpu && !desc->buses.empty() && desc->buses[0].bus) {
        MachineState ms;
        ms.machine = desc;
        ms.cpu = desc->cpus[0].cpu;
        ms.bus = desc->buses[0].bus;
        ms.id = id;
        ms.disasm = ToolchainRegistry::instance().createDisassembler(ms.cpu->isaName());
        ms.dbg = new DebugContext(ms.cpu, ms.bus);
        ms.cpu->setObserver(ms.dbg);
        ms.bus->setObserver(ms.dbg);
        ms.dbg->onMachineLoad(desc);
        if (ms.disasm && ms.dbg) ms.disasm->setSymbolTable(&ms.dbg->symbols());

        g_machines[id] = std::move(ms);
        return &g_machines[id];
        } else if (desc) {        delete desc;
    }
    return nullptr;
}

Json handleDescribe() {
    Json res(Json::OBJ);
    res.oVal["name"] = Json("mmemu-mcp");
    res.oVal["version"] = Json("0.1.0");
    
    Json tools(Json::ARR);
    auto addTool = [&](const std::string& name, const std::string& desc, const Json& schema) {
        Json t(Json::OBJ);
        t.oVal["name"] = Json(name);
        t.oVal["description"] = Json(desc);
        t.oVal["inputSchema"] = schema;
        tools.push_back(t);
    };

    // Common property schemas
    Json midProp(Json::OBJ); midProp.oVal["type"] = Json("string"); midProp.oVal["description"] = Json("Machine type from list_machines (e.g. rawMega65, c64)");
    Json addrProp(Json::OBJ); addrProp.oVal["type"] = Json("string"); addrProp.oVal["description"] = Json("Address expression (e.g. $1000, start+5, %1010, decimal)");
    Json cntProp(Json::OBJ); cntProp.oVal["type"] = Json("integer"); cntProp.oVal["description"] = Json("Number of items (instructions, bytes, stack entries, etc.)");
    Json sizeProp(Json::OBJ); sizeProp.oVal["type"] = Json("string"); sizeProp.oVal["description"] = Json("Size expression (hex or decimal)");
    Json pattProp(Json::OBJ); pattProp.oVal["type"] = Json("string"); pattProp.oVal["description"] = Json("Search pattern — ASCII text or hex bytes if is_hex is true");
    Json ishProp(Json::OBJ); ishProp.oVal["type"] = Json("boolean"); ishProp.oVal["description"] = Json("If true, interpret the value as hexadecimal");
    Json textProp(Json::OBJ); textProp.oVal["type"] = Json("string"); textProp.oVal["description"] = Json("Text string to type or process");

    Json typeSchema(Json::OBJ);
    typeSchema.oVal["type"] = Json("object");
    Json typeProps(Json::OBJ);
    typeProps.oVal["machine_id"] = midProp;
    typeProps.oVal["text"] = textProp;
    typeSchema.oVal["properties"] = typeProps;
    Json typeReq(Json::ARR); typeReq.push_back(Json("machine_id")); typeReq.push_back(Json("text"));
    typeSchema.oVal["required"] = typeReq;
    addTool("type_string", "Type text into the machine's virtual keyboard, simulating sequential key presses", typeSchema);

    Json stepSchema(Json::OBJ);
    stepSchema.oVal["type"] = Json("object");
    Json stepProps(Json::OBJ);
    stepProps.oVal["machine_id"] = midProp;
    stepProps.oVal["count"] = cntProp;
    stepSchema.oVal["properties"] = stepProps;
    Json stepReq(Json::ARR); stepReq.push_back(Json("machine_id"));
    stepSchema.oVal["required"] = stepReq;
    addTool("step_cpu", "Step the CPU by count instructions (default 1). Returns registers after stepping.", stepSchema);

    Json spcSchema(Json::OBJ);
    spcSchema.oVal["type"] = Json("object");
    Json spcProps(Json::OBJ);
    spcProps.oVal["machine_id"] = midProp;
    spcProps.oVal["addr"] = addrProp;
    spcSchema.oVal["properties"] = spcProps;
    Json spcReq(Json::ARR); spcReq.push_back(Json("machine_id")); spcReq.push_back(Json("addr"));
    spcSchema.oVal["required"] = spcReq;
    addTool("set_pc", "Set the CPU program counter to an address expression", spcSchema);

    Json rmSchema(Json::OBJ);
    rmSchema.oVal["type"] = Json("object");
    Json rmProps(Json::OBJ);
    rmProps.oVal["machine_id"] = midProp;
    rmProps.oVal["addr"] = addrProp;
    rmProps.oVal["size"] = sizeProp;
    rmSchema.oVal["properties"] = rmProps;
    Json rmReq(Json::ARR); rmReq.push_back(Json("machine_id")); rmReq.push_back(Json("addr")); rmReq.push_back(Json("size"));
    rmSchema.oVal["required"] = rmReq;
    addTool("read_memory", "Read a range of memory bytes. Returns hex dump output.", rmSchema);

    Json wmSchema(Json::OBJ);
    wmSchema.oVal["type"] = Json("object");
    Json wmProps(Json::OBJ);
    wmProps.oVal["machine_id"] = midProp;
    wmProps.oVal["addr"] = addrProp;
    Json bytesProp(Json::OBJ); bytesProp.oVal["type"] = Json("array"); bytesProp.oVal["description"] = Json("Array of byte values (0-255) to write sequentially starting at addr");
    Json itemsProp(Json::OBJ); itemsProp.oVal["type"] = Json("integer");
    bytesProp.oVal["items"] = itemsProp;
    wmProps.oVal["bytes"] = bytesProp;
    wmSchema.oVal["properties"] = wmProps;
    Json wmReq(Json::ARR); wmReq.push_back(Json("machine_id")); wmReq.push_back(Json("addr")); wmReq.push_back(Json("bytes"));
    wmSchema.oVal["required"] = wmReq;
    addTool("write_memory", "Write an array of bytes to memory starting at addr", wmSchema);

    addTool("read_registers", "Read all current CPU registers (A, X, Y, SP, PC, flags). Only machine_id is required.", spcSchema); // reuse machine_id + addr (addr ignored)

    Json smSchema(Json::OBJ);
    smSchema.oVal["type"] = Json("object");
    Json smProps(Json::OBJ);
    smProps.oVal["machine_id"] = midProp;
    smProps.oVal["pattern"] = pattProp;
    smProps.oVal["is_hex"] = ishProp;
    smProps.oVal["start_addr"] = addrProp;
    smSchema.oVal["properties"] = smProps;
    Json smReq(Json::ARR); smReq.push_back(Json("machine_id")); smReq.push_back(Json("pattern"));
    smSchema.oVal["required"] = smReq;
    addTool("search_memory", "Search for a byte pattern in memory. Use is_hex=true for hex patterns (e.g. \"A9 00\"), otherwise ASCII text. Optional start_addr limits search range.", smSchema);

    Json snSchema(Json::OBJ);
    snSchema.oVal["type"] = Json("object");
    Json snProps(Json::OBJ);
    snProps.oVal["machine_id"] = midProp;
    snSchema.oVal["properties"] = snProps;
    Json snReq(Json::ARR); snReq.push_back(Json("machine_id"));
    snSchema.oVal["required"] = snReq;
    addTool("search_next", "Find the next occurrence of the last search_memory pattern, wrapping at end of address space.", snSchema);

    Json spSchema(Json::OBJ);
    spSchema.oVal["type"] = Json("object");
    Json spProps(Json::OBJ);
    spProps.oVal["machine_id"] = midProp;
    spSchema.oVal["properties"] = spProps;
    Json spReq(Json::ARR); spReq.push_back(Json("machine_id"));
    spSchema.oVal["required"] = spReq;
    addTool("search_prior", "Find the previous occurrence of the last search_memory pattern, wrapping at start of address space.", spSchema);

    Json mtSchema(Json::OBJ);
    mtSchema.oVal["type"] = Json("object");
    Json mtProps(Json::OBJ);
    mtProps.oVal["machine_id"] = midProp;
    Json pathProp(Json::OBJ); pathProp.oVal["type"] = Json("string"); pathProp.oVal["description"] = Json("Filesystem path to the file");
    mtProps.oVal["path"] = pathProp;
    mtSchema.oVal["properties"] = mtProps;
    Json mtReq(Json::ARR); mtReq.push_back(Json("machine_id")); mtReq.push_back(Json("path"));
    mtSchema.oVal["required"] = mtReq;
    addTool("mount_tape", "Mount a .tap tape image file into the machine's datasette drive", mtSchema);

    Json mdSchema(Json::OBJ);
    mdSchema.oVal["type"] = Json("object");
    Json mdProps(Json::OBJ);
    mdProps.oVal["machine_id"] = midProp;
    Json unitProp(Json::OBJ); unitProp.oVal["type"] = Json("integer"); unitProp.oVal["description"] = Json("Drive unit number (e.g. 8, 9, 10, 11)");
    mdProps.oVal["unit"] = unitProp;
    mdProps.oVal["path"] = pathProp;
    mdSchema.oVal["properties"] = mdProps;
    Json mdReq(Json::ARR); mdReq.push_back(Json("machine_id")); mdReq.push_back(Json("unit")); mdReq.push_back(Json("path"));
    mdSchema.oVal["required"] = mdReq;
    addTool("mount_disk", "Mount a .d64/.g64/.d81 disk image into a drive unit", mdSchema);

    Json edSchema(Json::OBJ);
    edSchema.oVal["type"] = Json("object");
    Json edProps(Json::OBJ);
    edProps.oVal["machine_id"] = midProp;
    edProps.oVal["unit"] = unitProp;
    edSchema.oVal["properties"] = edProps;
    Json edReq(Json::ARR); edReq.push_back(Json("machine_id")); edReq.push_back(Json("unit"));
    edSchema.oVal["required"] = edReq;
    addTool("eject_disk", "Eject the disk image from a drive unit", edSchema);

    Json ctSchema(Json::OBJ);
    ctSchema.oVal["type"] = Json("object");
    Json ctProps(Json::OBJ);
    ctProps.oVal["machine_id"] = midProp;
    Json opProp(Json::OBJ);
    opProp.oVal["type"] = Json("string");
    opProp.oVal["description"] = Json("Tape operation: \"play\", \"stop\", \"rewind\", \"record\", or \"stoprecord\"");
    ctProps.oVal["operation"] = opProp;
    ctSchema.oVal["properties"] = ctProps;
    Json ctReq(Json::ARR); ctReq.push_back(Json("machine_id")); ctReq.push_back(Json("operation"));
    ctSchema.oVal["required"] = ctReq;
    addTool("control_tape", "Control the datasette tape transport (play, stop, rewind, record, stoprecord)", ctSchema);

    Json rtSchema(Json::OBJ);
    rtSchema.oVal["type"] = Json("object");
    Json rtProps(Json::OBJ);
    rtProps.oVal["machine_id"] = midProp;
    rtSchema.oVal["properties"] = rtProps;
    Json rtReq(Json::ARR); rtReq.push_back(Json("machine_id"));
    rtSchema.oVal["required"] = rtReq;
    addTool("record_tape", "Arm the datasette for recording (captures CPU write-line data)", rtSchema);

    Json strSchema(Json::OBJ);
    strSchema.oVal["type"] = Json("object");
    Json strProps(Json::OBJ);
    strProps.oVal["machine_id"] = midProp;
    Json strPathProp(Json::OBJ); strPathProp.oVal["type"] = Json("string"); strPathProp.oVal["description"] = Json("Output .tap file path to save the recording");
    strProps.oVal["path"] = strPathProp;
    strSchema.oVal["properties"] = strProps;
    Json strReq(Json::ARR); strReq.push_back(Json("machine_id")); strReq.push_back(Json("path"));
    strSchema.oVal["required"] = strReq;
    addTool("save_tape_recording", "Stop recording and save captured tape data to a .tap file", strSchema);

    Json kSchema(Json::OBJ);
    kSchema.oVal["type"] = Json("object");
    Json kProps(Json::OBJ);
    kProps.oVal["machine_id"] = midProp;
    Json keyProp(Json::OBJ); keyProp.oVal["type"] = Json("string"); keyProp.oVal["description"] = Json("Key name (e.g. \"a\", \"return\", \"space\", \"f1\", \"shift\")");
    kProps.oVal["key"] = keyProp;
    Json ksProp(Json::OBJ); ksProp.oVal["type"] = Json("boolean"); ksProp.oVal["description"] = Json("true for key-down (press), false for key-up (release)");
    kProps.oVal["down"] = ksProp;
    kSchema.oVal["properties"] = kProps;
    Json kReq(Json::ARR); kReq.push_back(Json("machine_id")); kReq.push_back(Json("key")); kReq.push_back(Json("down"));
    kSchema.oVal["required"] = kReq;
    addTool("press_key", "Press or release a single key on the machine's virtual keyboard", kSchema);

    Json liSchema(Json::OBJ);
    liSchema.oVal["type"] = Json("object");
    Json liProps(Json::OBJ);
    liProps.oVal["machine_id"] = midProp;
    liProps.oVal["path"] = pathProp;
    liProps.oVal["addr"] = addrProp;
    Json autoProp(Json::OBJ); autoProp.oVal["type"] = Json("boolean"); autoProp.oVal["description"] = Json("If true, set PC to load address after loading (auto-run)");
    liProps.oVal["auto_start"] = autoProp;
    liSchema.oVal["properties"] = liProps;
    Json liReq(Json::ARR); liReq.push_back(Json("machine_id")); liReq.push_back(Json("path"));
    liSchema.oVal["required"] = liReq;
    addTool("load_image", "Load a .prg or .bin image into memory. Optional addr overrides the PRG load address; auto_start sets PC to the entry point.", liSchema);

    Json acSchema(Json::OBJ);
    acSchema.oVal["type"] = Json("object");
    Json acProps(Json::OBJ);
    acProps.oVal["machine_id"] = midProp;
    acProps.oVal["path"] = pathProp;
    Json resetProp(Json::OBJ); resetProp.oVal["type"] = Json("boolean"); resetProp.oVal["description"] = Json("If true, reset the machine after attaching the cartridge");
    acProps.oVal["reset"] = resetProp;
    acSchema.oVal["properties"] = acProps;
    Json acReq(Json::ARR); acReq.push_back(Json("machine_id")); acReq.push_back(Json("path"));
    acSchema.oVal["required"] = acReq;
    addTool("attach_cartridge", "Attach a cartridge ROM image (.crt/.bin) to the machine", acSchema);

    Json ecSchema(Json::OBJ);
    ecSchema.oVal["type"] = Json("object");
    Json ecProps(Json::OBJ);
    ecProps.oVal["machine_id"] = midProp;
    ecSchema.oVal["properties"] = ecProps;
    Json ecReq(Json::ARR); ecReq.push_back(Json("machine_id"));
    ecSchema.oVal["required"] = ecReq;
    addTool("eject_cartridge", "Eject the currently attached cartridge from the machine", ecSchema);

    Json rstSchema(Json::OBJ);
    rstSchema.oVal["type"] = Json("object");
    Json rstProps(Json::OBJ);
    rstProps.oVal["machine_id"] = midProp;
    rstSchema.oVal["properties"] = rstProps;
    Json rstReq(Json::ARR); rstReq.push_back(Json("machine_id"));
    rstSchema.oVal["required"] = rstReq;
    addTool("reset_machine", "Reset a machine to its power-on state (cold reset)", rstSchema);

    Json emptySchema(Json::OBJ);
    emptySchema.oVal["type"] = Json("object");
    addTool("list_loggers", "List all registered loggers and their current log levels", emptySchema);

    Json sllSchema(Json::OBJ);
    sllSchema.oVal["type"] = Json("object");
    Json sllProps(Json::OBJ);
    Json sllTarget(Json::OBJ); sllTarget.oVal["type"] = Json("string"); sllTarget.oVal["description"] = Json("Logger name from list_loggers, or \"all\" to set all loggers");
    Json sllLevel(Json::OBJ); sllLevel.oVal["type"] = Json("string"); sllLevel.oVal["description"] = Json("Log level: \"trace\", \"debug\", \"info\", \"warn\", \"error\", \"critical\", or \"off\"");
    sllProps.oVal["target"] = sllTarget;
    sllProps.oVal["level"] = sllLevel;
    sllSchema.oVal["properties"] = sllProps;
    Json sllReq(Json::ARR); sllReq.push_back(Json("target")); sllReq.push_back(Json("level"));
    sllSchema.oVal["required"] = sllReq;
    addTool("set_log_level", "Set the log level for a specific logger or \"all\" loggers", sllSchema);

    // list_machines
    addTool("list_machines", "List all available machine types with their descriptions. Use these IDs with create_machine.", emptySchema);

    // create_machine
    Json cmSchema(Json::OBJ); cmSchema.oVal["type"] = Json("object");
    Json cmProps(Json::OBJ); cmProps.oVal["machine_id"] = midProp;
    cmSchema.oVal["properties"] = cmProps;
    Json cmReq(Json::ARR); cmReq.push_back(Json("machine_id"));
    cmSchema.oVal["required"] = cmReq;
    addTool("create_machine", "Create (or re-create) a machine instance. Use list_machines to see valid IDs.", cmSchema);

    // list_symbols
    Json lsSchema(Json::OBJ); lsSchema.oVal["type"] = Json("object");
    Json lsProps(Json::OBJ); lsProps.oVal["machine_id"] = midProp;
    lsSchema.oVal["properties"] = lsProps;
    Json lsReq(Json::ARR); lsReq.push_back(Json("machine_id"));
    lsSchema.oVal["required"] = lsReq;
    addTool("list_symbols", "List all defined symbols in the machine's symbol table (address → label)", lsSchema);

    // add_symbol
    Json asSchema(Json::OBJ); asSchema.oVal["type"] = Json("object");
    Json asProps(Json::OBJ); asProps.oVal["machine_id"] = midProp;
    Json lblProp(Json::OBJ); lblProp.oVal["type"] = Json("string"); lblProp.oVal["description"] = Json("Symbol label name (e.g. \"start\", \"irq_handler\")");
    asProps.oVal["label"] = lblProp; asProps.oVal["addr"] = addrProp;
    asSchema.oVal["properties"] = asProps;
    Json asReq(Json::ARR); asReq.push_back(Json("machine_id")); asReq.push_back(Json("label")); asReq.push_back(Json("addr"));
    asSchema.oVal["required"] = asReq;
    addTool("add_symbol", "Add a named symbol at an address to the machine's symbol table", asSchema);

    // remove_symbol
    Json rsSchema(Json::OBJ); rsSchema.oVal["type"] = Json("object");
    Json rsProps(Json::OBJ); rsProps.oVal["machine_id"] = midProp;
    rsProps.oVal["label"] = lblProp;
    rsSchema.oVal["properties"] = rsProps;
    Json rsReq(Json::ARR); rsReq.push_back(Json("machine_id")); rsReq.push_back(Json("label"));
    rsSchema.oVal["required"] = rsReq;
    addTool("remove_symbol", "Remove a named symbol from the machine's symbol table", rsSchema);

    // clear_symbols
    addTool("clear_symbols", "Remove all symbols from the machine's symbol table", lsSchema);

    // load_symbols
    Json ldsSchema(Json::OBJ); ldsSchema.oVal["type"] = Json("object");
    Json ldsProps(Json::OBJ); ldsProps.oVal["machine_id"] = midProp;
    ldsProps.oVal["path"] = pathProp;
    ldsSchema.oVal["properties"] = ldsProps;
    Json ldsReq(Json::ARR); ldsReq.push_back(Json("machine_id")); ldsReq.push_back(Json("path"));
    ldsSchema.oVal["required"] = ldsReq;
    addTool("load_symbols", "Load symbols from a KickAssembler .sym file into the machine's symbol table", ldsSchema);

    // run_cpu
    Json runSchema(Json::OBJ); runSchema.oVal["type"] = Json("object");
    Json runProps(Json::OBJ); runProps.oVal["machine_id"] = midProp;
    Json maxStepsProp(Json::OBJ); maxStepsProp.oVal["type"] = Json("integer"); maxStepsProp.oVal["description"] = Json("Maximum instructions to execute before stopping (default 10000000)");
    runProps.oVal["max_steps"] = maxStepsProp;
    runSchema.oVal["properties"] = runProps;
    Json runReq(Json::ARR); runReq.push_back(Json("machine_id"));
    runSchema.oVal["required"] = runReq;
    addTool("run_cpu", "Run the CPU until a breakpoint is hit, the program ends (BRK/RTS to empty stack), or max_steps is reached (default 10000000)", runSchema);

    // disassemble
    Json daSchema(Json::OBJ); daSchema.oVal["type"] = Json("object");
    Json daProps(Json::OBJ); daProps.oVal["machine_id"] = midProp; daProps.oVal["addr"] = addrProp; daProps.oVal["count"] = cntProp;
    daSchema.oVal["properties"] = daProps;
    Json daReq(Json::ARR); daReq.push_back(Json("machine_id"));
    daSchema.oVal["required"] = daReq;
    addTool("disassemble", "Disassemble instructions starting at addr (defaults to PC). count defaults to 10.", daSchema);

    // fill_memory
    Json fmSchema(Json::OBJ); fmSchema.oVal["type"] = Json("object");
    Json fmProps(Json::OBJ); fmProps.oVal["machine_id"] = midProp; fmProps.oVal["addr"] = addrProp;
    Json valProp(Json::OBJ); valProp.oVal["type"] = Json("integer"); valProp.oVal["description"] = Json("Byte value (0-255) to fill with");
    fmProps.oVal["value"] = valProp; fmProps.oVal["size"] = sizeProp;
    fmSchema.oVal["properties"] = fmProps;
    Json fmReq(Json::ARR); fmReq.push_back(Json("machine_id")); fmReq.push_back(Json("addr")); fmReq.push_back(Json("value")); fmReq.push_back(Json("size"));
    fmSchema.oVal["required"] = fmReq;
    addTool("fill_memory", "Fill a memory range starting at addr for size bytes with a single byte value", fmSchema);

    // copy_memory
    Json cpSchema(Json::OBJ); cpSchema.oVal["type"] = Json("object");
    Json cpProps(Json::OBJ); cpProps.oVal["machine_id"] = midProp;
    Json srcProp(Json::OBJ); srcProp.oVal["type"] = Json("integer"); srcProp.oVal["description"] = Json("Source start address");
    Json dstProp(Json::OBJ); dstProp.oVal["type"] = Json("integer"); dstProp.oVal["description"] = Json("Destination start address");
    cpProps.oVal["src_addr"] = srcProp; cpProps.oVal["dst_addr"] = dstProp; cpProps.oVal["size"] = sizeProp;
    cpSchema.oVal["properties"] = cpProps;
    Json cpReq(Json::ARR); cpReq.push_back(Json("machine_id")); cpReq.push_back(Json("src_addr")); cpReq.push_back(Json("dst_addr")); cpReq.push_back(Json("size"));
    cpSchema.oVal["required"] = cpReq;
    addTool("copy_memory", "Copy size bytes from src_addr to dst_addr", cpSchema);

    // swap_memory
    Json swmSchema(Json::OBJ); swmSchema.oVal["type"] = Json("object");
    Json swmProps(Json::OBJ); swmProps.oVal["machine_id"] = midProp;
    Json addr1Prop(Json::OBJ); addr1Prop.oVal["type"] = Json("integer"); addr1Prop.oVal["description"] = Json("First region start address");
    Json addr2Prop(Json::OBJ); addr2Prop.oVal["type"] = Json("integer"); addr2Prop.oVal["description"] = Json("Second region start address");
    swmProps.oVal["addr1"] = addr1Prop; swmProps.oVal["addr2"] = addr2Prop; swmProps.oVal["size"] = sizeProp;
    swmSchema.oVal["properties"] = swmProps;
    Json swmReq(Json::ARR); swmReq.push_back(Json("machine_id")); swmReq.push_back(Json("addr1")); swmReq.push_back(Json("addr2")); swmReq.push_back(Json("size"));
    swmSchema.oVal["required"] = swmReq;
    addTool("swap_memory", "Swap the contents of two equal-sized memory regions", swmSchema);

    // set_breakpoint
    Json sbpSchema(Json::OBJ); sbpSchema.oVal["type"] = Json("object");
    Json sbpProps(Json::OBJ); sbpProps.oVal["machine_id"] = midProp; sbpProps.oVal["addr"] = addrProp;
    sbpSchema.oVal["properties"] = sbpProps;
    Json sbpReq(Json::ARR); sbpReq.push_back(Json("machine_id")); sbpReq.push_back(Json("addr"));
    sbpSchema.oVal["required"] = sbpReq;
    addTool("set_breakpoint", "Set an execution breakpoint at an address. Returns the breakpoint ID.", sbpSchema);

    // set_watchpoint
    Json swpSchema(Json::OBJ); swpSchema.oVal["type"] = Json("object");
    Json swpProps(Json::OBJ); swpProps.oVal["machine_id"] = midProp; swpProps.oVal["addr"] = addrProp;
    Json wpTypeProp(Json::OBJ); wpTypeProp.oVal["type"] = Json("string"); wpTypeProp.oVal["description"] = Json("Watchpoint type: \"read\" or \"write\"");
    swpProps.oVal["type"] = wpTypeProp;
    swpSchema.oVal["properties"] = swpProps;
    Json swpReq(Json::ARR); swpReq.push_back(Json("machine_id")); swpReq.push_back(Json("addr")); swpReq.push_back(Json("type"));
    swpSchema.oVal["required"] = swpReq;
    addTool("set_watchpoint", "Set a memory watchpoint that triggers on read or write access. Returns the watchpoint ID.", swpSchema);

    // delete_breakpoint / enable_breakpoint / disable_breakpoint share the same schema
    Json bpIdSchema(Json::OBJ); bpIdSchema.oVal["type"] = Json("object");
    Json bpIdProps(Json::OBJ); bpIdProps.oVal["machine_id"] = midProp;
    Json idProp(Json::OBJ); idProp.oVal["type"] = Json("integer"); idProp.oVal["description"] = Json("Breakpoint/watchpoint ID from set_breakpoint, set_watchpoint, or list_breakpoints");
    bpIdProps.oVal["id"] = idProp;
    bpIdSchema.oVal["properties"] = bpIdProps;
    Json bpIdReq(Json::ARR); bpIdReq.push_back(Json("machine_id")); bpIdReq.push_back(Json("id"));
    bpIdSchema.oVal["required"] = bpIdReq;
    addTool("delete_breakpoint",  "Permanently remove a breakpoint or watchpoint by its ID", bpIdSchema);
    addTool("enable_breakpoint",  "Re-enable a previously disabled breakpoint or watchpoint", bpIdSchema);
    addTool("disable_breakpoint", "Temporarily disable a breakpoint or watchpoint without deleting it", bpIdSchema);

    // list_breakpoints
    addTool("list_breakpoints", "List all breakpoints and watchpoints with their IDs, addresses, types, and enabled status", cmSchema);  // reuse machine_id-only schema

    // get_stack
    Json gsSchema(Json::OBJ); gsSchema.oVal["type"] = Json("object");
    Json gsProps(Json::OBJ); gsProps.oVal["machine_id"] = midProp; gsProps.oVal["count"] = cntProp;
    gsSchema.oVal["properties"] = gsProps;
    Json gsReq(Json::ARR); gsReq.push_back(Json("machine_id"));
    gsSchema.oVal["required"] = gsReq;
    addTool("get_stack", "Read bytes from the CPU stack. count defaults to 8 entries; use 0 for all (SP to $01FF).", gsSchema);

    // list_devices
    addTool("list_devices", "List all IO devices registered in the machine (name, address range)", cmSchema);

    // get_device_info
    Json gdiSchema(Json::OBJ); gdiSchema.oVal["type"] = Json("object");
    Json gdiProps(Json::OBJ); gdiProps.oVal["machine_id"] = midProp;
    Json devProp(Json::OBJ); devProp.oVal["type"] = Json("string"); devProp.oVal["description"] = Json("Device name from list_devices");
    gdiProps.oVal["device"] = devProp;
    gdiSchema.oVal["properties"] = gdiProps;
    Json gdiReq(Json::ARR); gdiReq.push_back(Json("machine_id")); gdiReq.push_back(Json("device"));
    gdiSchema.oVal["required"] = gdiReq;
    addTool("get_device_info", "Get detailed register and status information for a specific device", gdiSchema);

    // asm
    Json asmSchema(Json::OBJ); asmSchema.oVal["type"] = Json("object");
    Json asmProps(Json::OBJ);
    asmProps.oVal["machine_id"] = midProp;
    Json asmSrcProp(Json::OBJ); asmSrcProp.oVal["type"] = Json("string");
    asmSrcProp.oVal["description"] = Json("Assembly source code to compile");
    asmProps.oVal["source"] = asmSrcProp;
    Json asmLaProp(Json::OBJ); asmLaProp.oVal["type"] = Json("integer");
    asmLaProp.oVal["description"] = Json("If set, write assembled bytes into machine memory at this address");
    asmProps.oVal["load_addr"] = asmLaProp;
    asmSchema.oVal["properties"] = asmProps;
    Json asmReq(Json::ARR); asmReq.push_back(Json("machine_id")); asmReq.push_back(Json("source"));
    asmSchema.oVal["required"] = asmReq;
    addTool("asm", "Assemble source code for the machine's ISA. Returns JSON: {\"bytes\":[...],\"symbols\":{},\"errors\":[...]}. Optional load_addr writes assembled bytes into machine memory.", asmSchema);

    std::vector<std::string> pluginTools;
    PluginToolRegistry::instance().listTools(pluginTools);
    for (const auto& name : pluginTools) {
        std::string schema = PluginToolRegistry::instance().getSchema(name);
        Json s = Json::parse(schema);
        addTool(name, "Plugin-provided tool", s);
    }

    res.oVal["tools"] = tools;
    return res;
}

Json handleResourcesList() {
    Json res(Json::OBJ);
    Json resources(Json::ARR);

    Json r(Json::OBJ);
    r.oVal["uri"] = Json("machine_state");
    r.oVal["name"] = Json("Machine State Snapshot");
    r.oVal["description"] = Json("Snapshot of current session state");
    resources.push_back(r);

    res.oVal["resources"] = resources;
    return res;
}

Json handleResourcesRead(const Json& params) {
    std::string uri = params["uri"].sVal;
    Json res(Json::OBJ);
    Json contents(Json::ARR);
    Json item(Json::OBJ);
    item.oVal["uri"] = Json(uri);
    item.oVal["mimeType"] = Json("text/plain");

    std::stringstream ss;
    for (const auto& kv : g_machines) {
        ss << "Machine [" << kv.first << "] Cycles: " << kv.second.cpu->cycles() << "\n";
    }
    if (g_machines.empty()) ss << "No machines initialized.\n";

    item.oVal["text"] = Json(ss.str());
    contents.push_back(item);
    res.oVal["contents"] = contents;
    return res;
}

Json handleToolsCall(const Json& params) {
    std::string name = params["name"].sVal;
    Json args = params["arguments"];
    
    Json res(Json::OBJ);
    Json content(Json::ARR);
    Json textItem(Json::OBJ);
    textItem.oVal["type"] = Json("text");

    if (name == "step_cpu") {
        std::string mid = args["machine_id"].sVal;
        int count = args.contains("count") ? (int)args["count"].nVal : 1;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            for (int i = 0; i < count; ++i) ms->cpu->step();
            textItem.oVal["text"] = Json("Executed " + std::to_string(count) + " instructions.");
        }
    } else if (name == "set_pc") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            uint32_t addr;
            if (resolveAddr(args["addr"], ms->dbg, addr)) {
                ms->cpu->setPc(addr);
                textItem.oVal["text"] = Json("PC set to $" + toHex(addr));
            } else {
                textItem.oVal["text"] = Json("Error: Invalid address expression");
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "read_memory") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            uint32_t addr, size;
            if (resolveAddr(args["addr"], ms->dbg, addr) &&
                resolveAddr(args["size"], ms->dbg, size)) 
            {
                std::stringstream ss;
                for (uint32_t i = 0; i < size; i += 16) {
                    ss << std::hex << std::setw(4) << std::setfill('0') << (addr + i) << ": ";
                    for (uint32_t j = 0; j < 16 && (i + j) < size; ++j) {
                        ss << std::setw(2) << (int)ms->bus->peek8(addr + i + j) << " ";
                    }
                    ss << "\n";
                }
                textItem.oVal["text"] = Json(ss.str());
            } else {
                textItem.oVal["text"] = Json("Error: Invalid address/size expression");
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "write_memory") {
        std::string mid = args["machine_id"].sVal;
        Json bytes = args["bytes"];
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else if (!bytes.is_array()) {
            textItem.oVal["text"] = Json("Error: bytes must be an array");
            textItem.oVal["isError"] = Json(true);
        } else {
            uint32_t addr;
            if (resolveAddr(args["addr"], ms->dbg, addr)) {
                for (size_t i = 0; i < bytes.aVal.size(); ++i) {
                    ms->bus->write8(addr + i, (uint8_t)bytes.aVal[i].nVal);
                }
                textItem.oVal["text"] = Json("Wrote " + std::to_string(bytes.aVal.size()) + " bytes at $" + toHex(addr));
            } else {
                textItem.oVal["text"] = Json("Error: Invalid address expression");
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "read_registers") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            std::stringstream ss;
            int count = ms->cpu->regCount();
            for (int i = 0; i < count; ++i) {
                const auto* desc = ms->cpu->regDescriptor(i);
                if (desc->flags & REGFLAG_INTERNAL) continue;
                uint32_t val = ms->cpu->regRead(i);
                ss << desc->name << ": $" << std::hex << std::setw(desc->width == RegWidth::R16 ? 4 : 2) << std::setfill('0') << val << "  ";
            }
            ss << std::dec << "\nCycles: " << ms->cpu->cycles();
            textItem.oVal["text"] = Json(ss.str());
        }
    } else if (name == "search_memory") {
        std::string mid = args["machine_id"].sVal;
        std::string pattern = args["pattern"].sVal;
        bool isHex = args.contains("is_hex") ? args["is_hex"].bVal : true;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            uint32_t startAddr = 0;
            if (args.contains("start_addr") && !resolveAddr(args["start_addr"], ms->dbg, startAddr)) {
                textItem.oVal["text"] = Json("Error: Invalid start_addr expression");
                textItem.oVal["isError"] = Json(true);
            } else {
                std::vector<uint8_t> bytes;
                if (isHex) {
                    std::stringstream pss(pattern);
                    std::string byteStr;
                    while (pss >> byteStr) {
                        try { bytes.push_back((uint8_t)std::stoul(byteStr, nullptr, 16)); } catch (...) {}
                    }
                } else {
                    for (char c : pattern) bytes.push_back((uint8_t)c);
                }

                if (bytes.empty()) {
                    textItem.oVal["text"] = Json("Error: Empty or invalid pattern");
                    textItem.oVal["isError"] = Json(true);
                } else {
                    ms->lastSearchPattern = bytes;
                    ms->lastSearchFoundAddr = 0xFFFFFFFF;
                    uint32_t found = 0xFFFFFFFF;
                    uint32_t mask = ms->bus->config().addrMask;
                    for (uint32_t i = startAddr; i <= mask && (i + bytes.size() <= mask + 1); ++i) {
                        bool match = true;
                        for (size_t j = 0; j < bytes.size(); ++j) {
                            if (ms->bus->peek8((i + j) & mask) != bytes[j]) {
                                match = false; break;
                            }
                        }
                        if (match) { found = i; break; }
                    }
                    if (found != 0xFFFFFFFF) {
                        ms->lastSearchFoundAddr = found;
                        std::stringstream ss;
                        ss << "Found at $" << std::hex << std::setw(4) << std::setfill('0') << found;
                        textItem.oVal["text"] = Json(ss.str());
                    } else {
                        textItem.oVal["text"] = Json("Pattern not found");
                    }
                }
            }
        }
    } else if (name == "search_next" || name == "search_prior") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else if (ms->lastSearchPattern.empty()) {
            textItem.oVal["text"] = Json("Error: No previous search — call search_memory first");
            textItem.oVal["isError"] = Json(true);
        } else {
            const auto& pattern = ms->lastSearchPattern;
            uint32_t addrMask = ms->bus->config().addrMask;
            uint32_t sz = (uint32_t)pattern.size();
            uint32_t found = 0xFFFFFFFF;
            bool forward = (name == "search_next");

            uint32_t cur = ms->lastSearchFoundAddr;
            uint32_t start = (cur == 0xFFFFFFFF)
                ? (forward ? 0 : addrMask)
                : (forward ? (cur + 1) & addrMask : (cur - 1) & addrMask);

            for (uint32_t step = 0; step <= addrMask; step++) {
                uint32_t addr = forward
                    ? (start + step) & addrMask
                    : (start - step) & addrMask;
                bool match = true;
                for (uint32_t j = 0; j < sz && match; j++)
                    if (ms->bus->peek8((addr + j) & addrMask) != pattern[j]) match = false;
                if (match) { found = addr; break; }
            }

            if (found == 0xFFFFFFFF) {
                textItem.oVal["text"] = Json("Pattern not found");
            } else {
                ms->lastSearchFoundAddr = found;
                std::stringstream ss;
                ss << "Found at $" << std::hex << std::setw(4) << std::setfill('0') << found;
                textItem.oVal["text"] = Json(ss.str());
            }
        }
    } else if (name == "mount_tape") {
        std::string mid = args["machine_id"].sVal;
        std::string path = args["path"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            IOHandler* tape = ms->machine->ioRegistry ? ms->machine->ioRegistry->findHandler("Tape") : nullptr;
            if (tape) {
                if (tape->mountTape(path)) {
                    textItem.oVal["text"] = Json("Mounted tape: " + path);
                } else {
                    textItem.oVal["text"] = Json("Error: Failed to mount tape");
                    textItem.oVal["isError"] = Json(true);
                }
            } else {
                textItem.oVal["text"] = Json("Error: No datasette found in machine");
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "mount_disk") {
        std::string mid = args["machine_id"].sVal;
        int unit = (int)args["unit"].nVal;
        std::string path = args["path"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            bool handled = false;
            if (ms->machine->ioRegistry) {
                std::vector<IOHandler*> handlers;
                ms->machine->ioRegistry->enumerate(handlers);
                for (auto* h : handlers) {
                    if (h->mountDisk(unit, path)) {
                        textItem.oVal["text"] = Json("Mounted disk '" + path + "' on unit " + std::to_string(unit));
                        handled = true;
                        break;
                    }
                }
            }
            if (!handled) {
                textItem.oVal["text"] = Json("Error: Failed to mount disk on unit " + std::to_string(unit));
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "eject_disk") {
        std::string mid = args["machine_id"].sVal;
        int unit = (int)args["unit"].nVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            if (ms->machine->ioRegistry) {
                std::vector<IOHandler*> handlers;
                ms->machine->ioRegistry->enumerate(handlers);
                for (auto* h : handlers) {
                    h->ejectDisk(unit);
                }
            }
            textItem.oVal["text"] = Json("Ejected disk from unit " + std::to_string(unit));
        }
    } else if (name == "control_tape") {
        std::string mid = args["machine_id"].sVal;
        std::string op = args["operation"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            IOHandler* tape = ms->machine->ioRegistry ? ms->machine->ioRegistry->findHandler("Tape") : nullptr;
            if (tape) {
                if      (op == "play")   tape->controlTape("play");
                else if (op == "stop")   tape->controlTape("stop");
                else if (op == "rewind") tape->controlTape("rewind");
                else {
                    textItem.oVal["text"] = Json("Error: Unknown operation " + op);
                    textItem.oVal["isError"] = Json(true);
                }
                if (!textItem.oVal.count("isError"))
                    textItem.oVal["text"] = Json("Tape: " + op);
            } else {
                textItem.oVal["text"] = Json("Error: No datasette found in machine");
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "record_tape") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            IOHandler* tape = ms->machine->ioRegistry ? ms->machine->ioRegistry->findHandler("Tape") : nullptr;
            if (!tape) {
                textItem.oVal["text"] = Json("Error: No datasette found in machine");
                textItem.oVal["isError"] = Json(true);
            } else if (tape->startTapeRecord()) {
                textItem.oVal["text"] = Json("Tape: recording started");
            } else {
                textItem.oVal["text"] = Json("Error: Could not start recording (write line not connected?)");
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "save_tape_recording") {
        std::string mid = args["machine_id"].sVal;
        std::string path = args["path"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            IOHandler* tape = ms->machine->ioRegistry ? ms->machine->ioRegistry->findHandler("Tape") : nullptr;
            if (!tape) {
                textItem.oVal["text"] = Json("Error: No datasette found in machine");
                textItem.oVal["isError"] = Json(true);
            } else {
                tape->stopTapeRecord();
                if (tape->saveTapeRecording(path)) {
                    textItem.oVal["text"] = Json("Tape recording saved: " + path);
                } else {
                    textItem.oVal["text"] = Json("Error: Failed to save tape recording");
                    textItem.oVal["isError"] = Json(true);
                }
            }
        }
    } else if (name == "press_key") {
        std::string mid = args["machine_id"].sVal;
        std::string key = args["key"].sVal;
        bool down = args["down"].bVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else if (!ms->machine->onKey) {
            textItem.oVal["text"] = Json("Error: Machine has no keyboard");
            textItem.oVal["isError"] = Json(true);
        } else {
            if (ms->machine->onKey(key, down)) {
                textItem.oVal["text"] = Json("Key " + key + (down ? " pressed" : " released"));
            } else {
                textItem.oVal["text"] = Json("Error: Unknown key " + key);
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "type_string") {
        std::string mid = args["machine_id"].sVal;
        std::string text = args["text"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            IKeyboardMatrix* kbd = nullptr;
            if (ms->machine->ioRegistry) {
                std::vector<IOHandler*> handlers;
                ms->machine->ioRegistry->enumerate(handlers);
                for (auto* h : handlers) {
                    if ((kbd = dynamic_cast<IKeyboardMatrix*>(h))) break;
                }
            }
            if (kbd) {
                kbd->enqueueText(text);
                textItem.oVal["text"] = Json("Text enqueued for typing on " + mid);
            } else {
                textItem.oVal["text"] = Json("Error: Machine has no keyboard matrix device");
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "load_image") {
        std::string mid = args["machine_id"].sVal;
        std::string path = args["path"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            uint32_t addr = 0;
            if (args.contains("addr") && !resolveAddr(args["addr"], ms->dbg, addr)) {
                textItem.oVal["text"] = Json("Error: Invalid address expression");
                textItem.oVal["isError"] = Json(true);
            } else {
                bool autoStart = args.contains("auto_start") ? args["auto_start"].bVal : false;
                auto* loader = ImageLoaderRegistry::instance().findLoader(path);
                if (loader) {
                    if (loader->load(path, ms->bus, ms->machine, addr)) {
                        uint32_t startAddr = addr;
                        if (startAddr == 0 && std::string(loader->name()).find("PRG") != std::string::npos) {
                            std::ifstream f(path, std::ios::binary);
                            uint8_t h[2];
                            f.read((char*)h, 2);
                            startAddr = h[0] | (h[1] << 8);
                        }
                        if (autoStart) {
                            ms->cpu->setPc(startAddr);
                        }
                        std::stringstream sss;
                        sss << "Loaded '" << path << "' at $" << std::hex << startAddr;
                        textItem.oVal["text"] = Json(sss.str());
                    } else {
                        textItem.oVal["text"] = Json("Error: Failed to load image");
                        textItem.oVal["isError"] = Json(true);
                    }
                } else {
                    textItem.oVal["text"] = Json("Error: No loader found for file type");
                    textItem.oVal["isError"] = Json(true);
                }
            }
        }
    } else if (name == "attach_cartridge") {
        std::string mid = args["machine_id"].sVal;
        std::string path = args["path"].sVal;
        bool doReset = args.contains("reset") ? args["reset"].bVal : true;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            auto handler = ImageLoaderRegistry::instance().createCartridgeHandler(path);
            if (handler) {
                if (handler->attach(ms->bus, ms->machine)) {
                    auto md = handler->metadata();
                    ImageLoaderRegistry::instance().setActiveCartridge(ms->bus, std::move(handler));
                    if (doReset && ms->machine->onReset) ms->machine->onReset(*ms->machine);
                    textItem.oVal["text"] = Json("Attached cartridge: " + md.displayName + " (" + md.type + ")");
                } else {
                    textItem.oVal["text"] = Json("Error: Failed to attach cartridge");
                    textItem.oVal["isError"] = Json(true);
                }
            } else {
                textItem.oVal["text"] = Json("Error: Unsupported cartridge format");
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "eject_cartridge") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            auto* cart = ImageLoaderRegistry::instance().getActiveCartridge(ms->bus);
            if (cart) {
                cart->eject(ms->bus);
                ImageLoaderRegistry::instance().setActiveCartridge(ms->bus, nullptr);
                if (ms->machine->onReset) ms->machine->onReset(*ms->machine);
                textItem.oVal["text"] = Json("Cartridge ejected.");
            } else {
                textItem.oVal["text"] = Json("No cartridge attached.");
            }
        }
    } else if (name == "reset_machine") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            if (ms->machine->onReset) {
                ms->machine->onReset(*ms->machine);
            } else {
                // Raw machines don't set onReset — reset components directly
                if (ms->machine->ioRegistry) ms->machine->ioRegistry->resetAll();
                for (auto& slot : ms->machine->cpus)
                    if (slot.cpu) slot.cpu->reset();
                for (auto& slot : ms->machine->buses)
                    if (slot.bus) slot.bus->reset();
            }
            if (ms->dbg) {
                ms->dbg->breakpoints().clearHitCounts();
                ms->dbg->stackTrace().clear();
            }
            textItem.oVal["text"] = Json("Machine " + mid + " reset.");
        }
    } else if (name == "list_loggers") {
        auto names = LogRegistry::instance().getLoggerNames();
        std::stringstream ss;
        ss << "Registered loggers:\n";
        for (const auto& n : names) {
            auto l = LogRegistry::instance().getLogger(n);
            std::string lvl = spdlog::level::to_string_view(l->level()).data();
            ss << "  " << n << " [" << lvl << "]\n";
        }
        textItem.oVal["text"] = Json(ss.str());
    } else if (name == "set_log_level") {
        std::string target = args["target"].sVal;
        std::string levelStr = args["level"].sVal;
        spdlog::level::level_enum lvl = spdlog::level::from_str(levelStr);
        if (target == "all") {
            LogRegistry::instance().setGlobalLevel(lvl);
            textItem.oVal["text"] = Json("Set all loggers to " + levelStr);
        } else {
            auto l = LogRegistry::instance().getLogger(target);
            l->set_level(lvl);
            textItem.oVal["text"] = Json("Set logger '" + target + "' to " + levelStr);
        }
    } else if (name == "list_machines") {
        std::vector<std::pair<std::string, std::string>> entries;
        MachineRegistry::instance().enumerateDetailed(entries);
        std::stringstream ss;
        for (const auto& entry : entries) {
            ss << entry.first;
            if (!entry.second.empty()) ss << " — " << entry.second;
            ss << "\n";
        }
        textItem.oVal["text"] = Json(ss.str().empty() ? "(none)\n" : ss.str());
    } else if (name == "create_machine") {
        std::string mid = args["machine_id"].sVal;
        // Destroy any existing instance so getMachine creates a fresh one
        g_machines.erase(mid);
        MachineState* ms = getMachine(mid);
        if (!ms) {
            std::vector<std::string> validIds;
            MachineRegistry::instance().enumerate(validIds);
            std::string validList;
            for (size_t i = 0; i < validIds.size(); ++i) {
                if (i > 0) validList += ", ";
                validList += validIds[i];
            }
            textItem.oVal["text"] = Json("Error: Unknown machine ID: " + mid +
                ". Valid IDs: " + (validList.empty() ? "(none)" : validList));
            textItem.oVal["isError"] = Json(true);
        } else {
            textItem.oVal["text"] = Json("Created machine: " + std::string(ms->machine->displayName));
        }
    } else if (name == "list_symbols") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            auto syms = ms->dbg->symbols().symbols();
            std::stringstream ss;
            ss << std::hex << std::uppercase << std::setfill('0');
            for (const auto& pair : syms) {
                ss << "$" << std::setw(4) << pair.first << "  " << pair.second << "\n";
            }
            textItem.oVal["text"] = Json(ss.str().empty() ? "(no symbols)\n" : ss.str());
        }
    } else if (name == "add_symbol") {
        std::string mid = args["machine_id"].sVal;
        std::string label = args["label"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            uint32_t addr;
            if (resolveAddr(args["addr"], ms->dbg, addr)) {
                ms->dbg->symbols().addSymbol(addr, label);
                textItem.oVal["text"] = Json("Symbol added: " + label + " at $" + toHex(addr));
            } else {
                textItem.oVal["text"] = Json("Error: Invalid address expression");
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "remove_symbol") {
        std::string mid = args["machine_id"].sVal;
        std::string label = args["label"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            ms->dbg->symbols().removeSymbol(label);
            textItem.oVal["text"] = Json("Symbol removed: " + label);
        }
    } else if (name == "clear_symbols") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            ms->dbg->symbols().clear();
            textItem.oVal["text"] = Json("Symbols cleared for " + mid);
        }
    } else if (name == "load_symbols") {
        std::string mid = args["machine_id"].sVal;
        std::string path = args["path"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            if (ms->dbg->symbols().loadSym(path)) {
                textItem.oVal["text"] = Json("Symbols loaded from: " + path);
            } else {
                textItem.oVal["text"] = Json("Error: Failed to load symbols from: " + path);
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "list_devices") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            std::vector<IOHandler*> handlers;
            if (ms->machine->ioRegistry) ms->machine->ioRegistry->enumerate(handlers);
            std::stringstream ss;
            for (auto* h : handlers) ss << h->name() << "\n";
            textItem.oVal["text"] = Json(ss.str().empty() ? "(none)\n" : ss.str());
        }
    } else if (name == "get_device_info") {
        std::string mid = args["machine_id"].sVal;
        std::string devName = args["device"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            IOHandler* handler = ms->machine->ioRegistry ? ms->machine->ioRegistry->findHandler(devName) : nullptr;
            if (!handler && ms->machine->ioRegistry) {
                std::vector<IOHandler*> handlers;
                ms->machine->ioRegistry->enumerate(handlers);
                for (auto* h : handlers) {
                    std::string hname = h->name();
                    std::string target = devName;
                    std::transform(hname.begin(), hname.end(), hname.begin(), ::tolower);
                    std::transform(target.begin(), target.end(), target.begin(), ::tolower);
                    if (hname == target || hname.find(target) != std::string::npos) {
                        handler = h;
                        break;
                    }
                }
            }
            if (handler) {
                DeviceInfo info;
                handler->getDeviceInfo(info);
                Json res(Json::OBJ);
                res.oVal["name"] = Json(info.name);
                res.oVal["baseAddr"] = Json((double)info.baseAddr);
                res.oVal["addrMask"] = Json((double)info.addrMask);
                Json deps(Json::ARR);
                for (const auto& d : info.dependencies) {
                    Json dj(Json::OBJ); dj.oVal["name"] = Json(d.first); dj.oVal["value"] = Json(d.second);
                    deps.push_back(dj);
                }
                res.oVal["dependencies"] = deps;
                Json state(Json::ARR);
                for (const auto& s : info.state) {
                    Json sj(Json::OBJ); sj.oVal["name"] = Json(s.first); sj.oVal["value"] = Json(s.second);
                    state.push_back(sj);
                }
                res.oVal["state"] = state;
                Json regs(Json::ARR);
                for (const auto& r : info.registers) {
                    Json rj(Json::OBJ);
                    rj.oVal["name"] = Json(r.name);
                    rj.oVal["offset"] = Json((double)r.offset);
                    rj.oVal["value"] = Json((double)r.value);
                    rj.oVal["description"] = Json(r.description);
                    regs.push_back(rj);
                }
                res.oVal["registers"] = regs;
                textItem.oVal["text"] = Json(res.stringify());
            } else {
                textItem.oVal["text"] = Json("Error: Device '" + devName + "' not found");
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "asm") {
        std::string mid = args["machine_id"].sVal;
        std::string source = args["source"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            std::string isa = ms->cpu->isaName();
            IAssembler* asmb = ToolchainRegistry::instance().createAssembler(isa);
            std::vector<uint8_t> bytes;
            std::vector<std::string> errors;

            if (!asmb) {
                errors.push_back("No assembler registered for ISA: " + isa);
            } else {
                uint32_t curAddr = 0;
                std::istringstream lineStream(source);
                std::string line;
                while (std::getline(lineStream, line)) {
                    std::string trimmed = line;
                    trimmed.erase(0, trimmed.find_first_not_of(" \t"));
                    if (trimmed.empty() || trimmed[0] == ';') continue;
                    // Detect .org / *= directive to rebase current address
                    std::string low = trimmed;
                    std::transform(low.begin(), low.end(), low.begin(), ::tolower);
                    if (low.size() >= 4 && low.substr(0, 4) == ".org") {
                        size_t pos = line.find_first_of("$0123456789");
                        if (pos != std::string::npos) {
                            std::string addrStr = line.substr(pos);
                            try {
                                if (addrStr[0] == '$')
                                    curAddr = std::stoul(addrStr.substr(1), nullptr, 16);
                                else
                                    curAddr = std::stoul(addrStr, nullptr, 0);
                            } catch (...) { }
                        }
                        continue;
                    }
                    uint8_t buf[32];
                    int n = asmb->assembleLine(line, buf, sizeof(buf), curAddr);
                    if (n < 0) {
                        errors.push_back("Error on line: " + line);
                    } else {
                        for (int i = 0; i < n; i++) bytes.push_back(buf[i]);
                        curAddr += (uint32_t)n;
                    }
                }
                delete asmb;
            }

            // Optionally write to machine memory
            if (errors.empty() && !bytes.empty() && args.contains("load_addr")) {
                uint32_t loadAddr = (uint32_t)args["load_addr"].nVal;
                for (size_t i = 0; i < bytes.size(); i++)
                    ms->bus->write8(loadAddr + (uint32_t)i, bytes[i]);
            }

            // Build result JSON
            Json result(Json::OBJ);
            Json byteArr(Json::ARR);
            for (auto b : bytes) byteArr.push_back(Json((double)b));
            result.oVal["bytes"] = byteArr;
            result.oVal["symbols"] = Json(Json::OBJ);
            Json errArr(Json::ARR);
            for (auto& e : errors) errArr.push_back(Json(e));
            result.oVal["errors"] = errArr;
            textItem.oVal["text"] = Json(result.stringify());
        }
    } else if (name == "run_cpu") {
        std::string mid = args["machine_id"].sVal;
        int maxSteps = args.contains("max_steps") ? (int)args["max_steps"].nVal : 10000000;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            ms->dbg->resume();
            int steps = 0;
            while (!ms->dbg->isPaused() && steps < maxSteps) {
                if (ms->machine && ms->machine->schedulerStep) {
                    ms->machine->schedulerStep(*ms->machine);
                } else {
                    ms->cpu->step();
                }
                if (ms->cpu->isProgramEnd(ms->bus)) break;
                ++steps;
            }
            std::stringstream ss;
            if (ms->dbg->isPaused())   ss << "Breakpoint hit. ";
            else if (steps >= maxSteps) ss << "Stopped after " << maxSteps << " steps. ";
            else                        ss << "Program ended. ";
            int regCount = ms->cpu->regCount();
            for (int i = 0; i < regCount; ++i) {
                const auto* desc = ms->cpu->regDescriptor(i);
                if (desc->flags & REGFLAG_INTERNAL) continue;
                uint32_t val = ms->cpu->regRead(i);
                ss << desc->name << ": $"
                   << toHex(val, desc->width == RegWidth::R16 ? 4 : 2) << "  ";
            }
            textItem.oVal["text"] = Json(ss.str());
        }
    } else if (name == "disassemble") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms || !ms->disasm || !ms->bus || !ms->cpu) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID or missing component (disasm/bus/cpu)");
            textItem.oVal["isError"] = Json(true);
        } else {
            uint32_t addr;
            if (args.contains("addr")) {
                if (!resolveAddr(args["addr"], ms->dbg, addr)) {
                    textItem.oVal["text"] = Json("Error: Invalid address expression");
                    textItem.oVal["isError"] = Json(true);
                    content.push_back(textItem);
                    res.oVal["content"] = content;
                    return res;
                }
            } else {
                addr = ms->cpu->pc();
            }
            int count = args.contains("count") ? (int)args["count"].nVal : 10;
            std::stringstream ss;
            for (int i = 0; i < count; ++i) {
                char buf[64];
                ss << toHex(addr) << ": ";
                int bytes = ms->disasm->disasmOne(ms->bus, addr, buf, sizeof(buf));
                ss << buf << "\n";
                addr += bytes;
            }
            textItem.oVal["text"] = Json(ss.str());
        }
    } else if (name == "fill_memory") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            uint32_t addr, value, size;
            if (resolveAddr(args["addr"], ms->dbg, addr) &&
                resolveAddr(args["value"], ms->dbg, value) &&
                resolveAddr(args["size"], ms->dbg, size)) 
            {
                for (uint32_t i = 0; i < size; ++i) ms->bus->write8(addr + i, (uint8_t)value);
                textItem.oVal["text"] = Json("Filled " + std::to_string(size) + " bytes at $" + toHex(addr));
            } else {
                textItem.oVal["text"] = Json("Error: Invalid expression in fill_memory");
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "copy_memory") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            uint32_t src, dst, size;
            if (resolveAddr(args["src_addr"], ms->dbg, src) &&
                resolveAddr(args["dst_addr"], ms->dbg, dst) &&
                resolveAddr(args["size"], ms->dbg, size))
            {
                std::vector<uint8_t> buf(size);
                for (uint32_t i = 0; i < size; ++i) buf[i] = ms->bus->peek8(src + i);
                for (uint32_t i = 0; i < size; ++i) ms->bus->write8(dst + i, buf[i]);
                textItem.oVal["text"] = Json("Copied " + std::to_string(size) + " bytes from $" + toHex(src) + " to $" + toHex(dst));
            } else {
                textItem.oVal["text"] = Json("Error: Invalid expression in copy_memory");
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "swap_memory") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            uint32_t addr1, addr2, size;
            if (resolveAddr(args["addr1"], ms->dbg, addr1) &&
                resolveAddr(args["addr2"], ms->dbg, addr2) &&
                resolveAddr(args["size"], ms->dbg, size))
            {
                std::vector<uint8_t> tmp(size);
                for (uint32_t i = 0; i < size; ++i) {
                    uint8_t v1 = ms->bus->read8(addr1 + i);
                    uint8_t v2 = ms->bus->read8(addr2 + i);
                    tmp[i] = v1;
                    ms->bus->write8(addr1 + i, v2);
                }
                for (uint32_t i = 0; i < size; ++i) ms->bus->write8(addr2 + i, tmp[i]);
                textItem.oVal["text"] = Json("Swapped " + std::to_string(size) + " bytes between $" + toHex(addr1) + " and $" + toHex(addr2));
            } else {
                textItem.oVal["text"] = Json("Error: Invalid expression in swap_memory");
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "set_breakpoint") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            uint32_t addr;
            if (resolveAddr(args["addr"], ms->dbg, addr)) {
                int id = ms->dbg->breakpoints().add(addr, BreakpointType::EXEC);
                textItem.oVal["text"] = Json("Breakpoint " + std::to_string(id) + " set at $" + toHex(addr));
            } else {
                textItem.oVal["text"] = Json("Error: Invalid address expression");
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "set_watchpoint") {
        std::string mid = args["machine_id"].sVal;
        std::string type = args["type"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else if (type != "read" && type != "write") {
            textItem.oVal["text"] = Json("Error: type must be \"read\" or \"write\"");
            textItem.oVal["isError"] = Json(true);
        } else {
            uint32_t addr;
            if (resolveAddr(args["addr"], ms->dbg, addr)) {
                BreakpointType btype = (type == "read") ? BreakpointType::READ_WATCH : BreakpointType::WRITE_WATCH;
                int id = ms->dbg->breakpoints().add(addr, btype);
                textItem.oVal["text"] = Json("Watchpoint " + std::to_string(id) + " (" + type + ") at $" + toHex(addr));
            } else {
                textItem.oVal["text"] = Json("Error: Invalid address expression");
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "delete_breakpoint") {
        std::string mid = args["machine_id"].sVal;
        int id = (int)args["id"].nVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            ms->dbg->breakpoints().remove(id);
            textItem.oVal["text"] = Json("Deleted breakpoint " + std::to_string(id));
        }
    } else if (name == "enable_breakpoint") {
        std::string mid = args["machine_id"].sVal;
        int id = (int)args["id"].nVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            ms->dbg->breakpoints().setEnabled(id, true);
            textItem.oVal["text"] = Json("Enabled breakpoint " + std::to_string(id));
        }
    } else if (name == "disable_breakpoint") {
        std::string mid = args["machine_id"].sVal;
        int id = (int)args["id"].nVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            ms->dbg->breakpoints().setEnabled(id, false);
            textItem.oVal["text"] = Json("Disabled breakpoint " + std::to_string(id));
        }
    } else if (name == "list_breakpoints") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            const auto& bps = ms->dbg->breakpoints().breakpoints();
            if (bps.empty()) {
                textItem.oVal["text"] = Json("No breakpoints set");
            } else {
                std::stringstream ss;
                ss << "ID  Type   Addr  En  Hits\n";
                for (const auto& bp : bps) {
                    const char* t = (bp.type == BreakpointType::EXEC) ? "exec" : (bp.type == BreakpointType::READ_WATCH ? "read" : "write");
                    ss << std::left << std::setw(4) << bp.id << std::setw(7) << t << "$" << toHex(bp.addr) << "  " << (bp.enabled ? "Y" : "N") << "   " << bp.hitCount << "\n";
                }
                textItem.oVal["text"] = Json(ss.str());
            }
        }
    } else if (name == "get_stack") {
        std::string mid = args["machine_id"].sVal;
        int count = args.contains("count") ? (int)args["count"].nVal : 8;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            auto trace = ms->dbg->stackTrace().recent(count);
            std::stringstream ss;
            for (const auto& e : trace) {
                const char* t = (e.type == StackPushType::CALL) ? "CALL" : (e.type == StackPushType::BRK ? "BRK" : "PUSH");
                ss << t << " from $" << toHex(e.pushedByPc) << " val: $" << toHex(e.value, 2) << "\n";
            }
            textItem.oVal["text"] = Json(ss.str().empty() ? "(stack empty)\n" : ss.str());
        }
    } else {
        textItem.oVal["text"] = Json("Error: Unknown tool " + name);
        textItem.oVal["isError"] = Json(true);
    }

    content.push_back(textItem);
    res.oVal["content"] = content;
    return res;
}

static Json handleInitialize(const Json& params) {
    (void)params;
    Json res(Json::OBJ);
    res.oVal["protocolVersion"] = Json("2024-11-05");

    Json info(Json::OBJ);
    info.oVal["name"] = Json("mmemu-mcp");
    info.oVal["version"] = Json("0.1.0");
    res.oVal["serverInfo"] = info;

    Json caps(Json::OBJ);
    Json toolsCap(Json::OBJ);
    caps.oVal["tools"] = toolsCap;
    Json resCap(Json::OBJ);
    caps.oVal["resources"] = resCap;
    res.oVal["capabilities"] = caps;
    return res;
}

static Json handleCall(const std::string& method, const Json& params) {
    if (method == "initialize")              return handleInitialize(params);
    if (method == "tools/list")              return handleDescribe();
    if (method == "tools/call")              return handleToolsCall(params);
    if (method == "resources/list")           return handleResourcesList();
    if (method == "resources/read")           return handleResourcesRead(params);
    // Keep legacy names for backward compat
    if (method == "list_resources")           return handleResourcesList();
    if (method == "read_resource")            return handleResourcesRead(params);
    if (method == "call_tool")               return handleToolsCall(params);
    if (method == "describe")                return handleDescribe();

    Json res(Json::OBJ);
    res.oVal["error"] = Json("Method not found");
    return res;
}

void mcpCleanup() {
    g_machines.clear();
    PluginLoader::instance().unloadAll();
}

#ifndef TEST_BUILD
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    LogRegistry::instance().init();
    PluginLoader::instance().loadFromStandardLocations();

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        try {
            Json req = Json::parse(line);
            std::string method = req["method"].sVal;
            Json params = req.contains("params") ? req["params"] : Json(Json::OBJ);

            // Notifications (no id) get no response
            if (method.rfind("notifications/", 0) == 0) continue;

            Json inner = handleCall(method, params);

            // Wrap in JSON-RPC 2.0 envelope
            Json rpc(Json::OBJ);
            rpc.oVal["jsonrpc"] = Json("2.0");
            if (req.contains("id")) rpc.oVal["id"] = req["id"];
            if (inner.contains("error")) {
                Json err(Json::OBJ);
                err.oVal["code"] = Json(-32601.0);
                err.oVal["message"] = inner["error"];
                rpc.oVal["error"] = err;
            } else {
                rpc.oVal["result"] = inner;
            }
            std::cout << rpc.stringify() << std::endl;
        } catch (const std::exception& e) {
            Json rpc(Json::OBJ);
            rpc.oVal["jsonrpc"] = Json("2.0");
            rpc.oVal["id"] = Json(); // null
            Json err(Json::OBJ);
            err.oVal["code"] = Json(-32700.0);
            err.oVal["message"] = Json(e.what());
            rpc.oVal["error"] = err;
            std::cout << rpc.stringify() << std::endl;
        }
    }

    g_machines.clear();
    PluginLoader::instance().unloadAll();

    return 0;
}
#endif
