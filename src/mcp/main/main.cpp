#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <set>
#include <unordered_map>

#include "include/version.h"
#include "minijson.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/machines/machine_registry.h"
#include "libcore/main/image_loader.h"
#include "libcore/main/sim_config.h"
#include "libcore/main/json_machine_loader.h"
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
#include "libdebug/main/trace_buffer.h"
#include "plugins/devices/map_mmu/main/map_mmu.h"
#include "plugins/devices/map_mmu/main/key_register.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/io_registry.h"
#include "imap_controller.h"

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

static bool resolveAddrWithDiagnostic(const Json& val, DebugContext* dbg, uint32_t& result, std::string& errMsg) {
    if (val.type == Json::NUM) {
        result = (uint32_t)val.nVal;
        return true;
    }
    if (val.type == Json::STR) {
        if (ExpressionEvaluator::evaluate(val.sVal, dbg, result)) {
            return true;
        }
        errMsg = "Invalid expression: \"" + val.sVal + "\" (supports: hex $1000, decimal 4096, registers A/X/Y/SP/PC, symbols, and operators +/-/*)";
        return false;
    }
    errMsg = "Address must be a number or string expression";
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
    IAssembler*        assem   = nullptr;
    DebugContext*      dbg     = nullptr;
    std::string        id;
    std::string        machineType;  // registry type (e.g., "c64")
    std::vector<uint8_t> lastSearchPattern;
    uint32_t             lastSearchFoundAddr = 0xFFFFFFFF;
    std::string          traceFilter = "all";  // all, instructions, breakpoints, memory

    ~MachineState() {
        // MachineDescriptor owns cpu and bus
        delete dbg;
        delete disasm;
        delete assem;
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
        assem = o.assem; o.assem = nullptr;
        dbg = o.dbg; o.dbg = nullptr;
        id = std::move(o.id);
        machineType = std::move(o.machineType);
        lastSearchPattern = std::move(o.lastSearchPattern);
        lastSearchFoundAddr = o.lastSearchFoundAddr;
        traceFilter = std::move(o.traceFilter);
    }
    MachineState& operator=(MachineState&& o) noexcept {
        if (this != &o) {
            delete dbg; delete disasm; delete assem; delete machine;
            machine = o.machine; o.machine = nullptr;
            cpu = o.cpu; o.cpu = nullptr;
            bus = o.bus; o.bus = nullptr;
            disasm = o.disasm; o.disasm = nullptr;
            assem = o.assem; o.assem = nullptr;
            dbg = o.dbg; o.dbg = nullptr;
            id = std::move(o.id);
            machineType = std::move(o.machineType);
            lastSearchPattern = std::move(o.lastSearchPattern);
            lastSearchFoundAddr = o.lastSearchFoundAddr;
            traceFilter = std::move(o.traceFilter);
        }
        return *this;
    }
};

// Machine state snapshot for diff comparison
struct MachineSnapshot {
    std::map<std::string, uint32_t> regs;      // register name → value
    std::vector<uint8_t> memory;               // flat memory dump
    uint32_t memBase = 0;                      // base address of memory dump
    uint32_t memSize = 0;                      // size of memory dump
};

// ---------------------------------------------------------------------------
// Test automation helpers (Phase 1-3 of #71)
// ---------------------------------------------------------------------------

// Build structured JSON object with register values
static Json buildRegistersJson(MachineState* ms) {
    Json regs(Json::OBJ);
    int regCount = ms->cpu->regCount();
    for (int i = 0; i < regCount; ++i) {
        const auto* desc = ms->cpu->regDescriptor(i);
        if (desc->flags & REGFLAG_INTERNAL) continue;
        regs.oVal[desc->name] = Json((int)ms->cpu->regRead(i));
    }
    regs.oVal["cycles"] = Json((double)ms->cpu->cycles());
    return regs;
}

// Read memory as an array of ints
static Json buildMemoryJson(MachineState* ms, uint32_t addr, uint32_t size) {
    Json arr(Json::ARR);
    for (uint32_t i = 0; i < size; ++i)
        arr.push_back(Json((int)ms->bus->peek8(addr + i)));
    return arr;
}

// Run CPU with a step budget.  Returns stop reason string.
// Sets breakpointHit=true if a breakpoint stopped execution.
static std::string runWithBudget(MachineState* ms, int maxSteps, bool ignoreProgramEnd,
                                 bool& breakpointHit, bool stopOnBrk = false) {
    breakpointHit = false;
    ms->dbg->resume();
    int steps = 0;
    while (steps < maxSteps) {
        // Check for BRK before executing it
        if (stopOnBrk && ms->bus->peek8(ms->cpu->pc()) == 0x00) {
            return "brk";
        }
        if (ms->machine && ms->machine->schedulerStep)
            ms->machine->schedulerStep(*ms->machine);
        else
            ms->cpu->step();
        ++steps;
        if (ms->dbg->isPaused()) { breakpointHit = true; return "breakpoint"; }
        if (!ignoreProgramEnd && ms->cpu->isProgramEnd(ms->bus)) return "program_end";
    }
    return "max_steps";
}

// Dispatch a single tool call internally, returning a structured Json result.
// Used by test_sequence to batch multiple calls.
static Json dispatchToolInternal(const std::string& toolName, const Json& toolArgs);

static std::map<std::string, MachineState> g_machines;
static std::map<std::string, std::map<std::string, MachineSnapshot>> g_snapshots; // machine_id → name → snapshot
static std::map<std::string, std::string> g_assemblerOverrides;  // per-instance assembler overrides
static std::map<std::string, int> g_typeCounters;  // per-type instance counter for auto-generation

static MachineState* getMachine(const std::string& instanceId) {
    auto it = g_machines.find(instanceId);
    return (it != g_machines.end()) ? &it->second : nullptr;
}

static MachineState* createMachineInstance(const std::string& instanceId, const std::string& machineType) {
    auto* desc = MachineRegistry::instance().createMachine(machineType);
    if (desc && !desc->cpus.empty() && desc->cpus[0].cpu && !desc->buses.empty() && desc->buses[0].bus) {
        MachineState ms;
        ms.machine = desc;
        ms.cpu = desc->cpus[0].cpu;
        ms.bus = ms.cpu->getDataBus() ? ms.cpu->getDataBus() : desc->buses[0].bus;
        ms.id = instanceId;
        ms.machineType = machineType;
        ms.disasm = ToolchainRegistry::instance().createDisassembler(ms.cpu->isaName());
        // Resolve assembler with optional per-instance override
        std::string overrideName = g_assemblerOverrides.count(instanceId) ? g_assemblerOverrides[instanceId] : "";
        ms.assem = resolveAssembler(ms.cpu->isaName(), desc->preferredAssembler, overrideName);
        ms.dbg = new DebugContext(ms.cpu, ms.bus);
        ms.cpu->setObserver(ms.dbg);
        ms.bus->setObserver(ms.dbg);
        ms.dbg->onMachineLoad(desc);
        if (ms.disasm && ms.dbg) ms.disasm->setSymbolTable(&ms.dbg->symbols());

        // Trigger machine reset so CPU reads the reset vector (or enters hypervisor)
        if (desc->onReset) desc->onReset(*desc);

        g_machines[instanceId] = std::move(ms);
        return &g_machines[instanceId];
    } else if (desc) {
        delete desc;
    }
    return nullptr;
}

Json handleDescribe() {
    Json res(Json::OBJ);
    res.oVal["name"] = Json("mmemu-mcp");
    res.oVal["version"] = Json(MMSIM_VERSION_FULL);
    
    Json tools(Json::ARR);
    auto addTool = [&](const std::string& name, const std::string& desc, const Json& schema) {
        Json t(Json::OBJ);
        t.oVal["name"] = Json(name);
        t.oVal["description"] = Json(desc);
        t.oVal["inputSchema"] = schema;
        tools.push_back(t);
    };

    // Common property schemas
    Json midProp(Json::OBJ); midProp.oVal["type"] = Json("string"); midProp.oVal["description"] = Json("Machine instance ID (from create_machine or list_instances)");
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
    Json ipeProp(Json::OBJ); ipeProp.oVal["type"] = Json("boolean");
    ipeProp.oVal["description"] = Json("If true, ignore program-end detection (BRK, RTS on empty stack). Use for full-machine debugging.");
    stepProps.oVal["ignore_program_end"] = ipeProp;
    stepSchema.oVal["properties"] = stepProps;
    Json stepReq(Json::ARR); stepReq.push_back(Json("machine_id"));
    stepSchema.oVal["required"] = stepReq;
    addTool("step_cpu", "Step the CPU by count instructions (default 1). Returns registers after stepping.", stepSchema);

    // reverse_step
    {
        Json rsSchema(Json::OBJ); rsSchema.oVal["type"] = Json("object");
        Json rsProps(Json::OBJ);
        rsProps.oVal["machine_id"] = midProp;
        rsProps.oVal["count"] = cntProp;
        rsSchema.oVal["properties"] = rsProps;
        Json rsReq(Json::ARR); rsReq.push_back(Json("machine_id"));
        rsSchema.oVal["required"] = rsReq;
        addTool("reverse_step",
                "Step the CPU backward by undoing traced instructions. "
                "Restores registers and reverses memory writes for each step. "
                "Limited by trace buffer depth (default 1000 instructions).",
                rsSchema);
    }

    // undo_info
    {
        Json uiSchema(Json::OBJ); uiSchema.oVal["type"] = Json("object");
        Json uiProps(Json::OBJ);
        uiProps.oVal["machine_id"] = midProp;
        uiSchema.oVal["properties"] = uiProps;
        Json uiReq(Json::ARR); uiReq.push_back(Json("machine_id"));
        uiSchema.oVal["required"] = uiReq;
        addTool("undo_info",
                "Show undo/time-travel buffer status: entries available for reverse stepping, "
                "buffer capacity, and memory usage estimate.",
                uiSchema);
    }

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

    {
        Json rrSchema(Json::OBJ);
        rrSchema.oVal["type"] = Json("object");
        Json rrProps(Json::OBJ);
        rrProps.oVal["machine_id"] = midProp;
        rrSchema.oVal["properties"] = rrProps;
        Json rrReq(Json::ARR);
        rrReq.push_back(Json("machine_id"));
        rrSchema.oVal["required"] = rrReq;
        addTool("read_registers", "Read all current CPU registers (A, X, Y, SP, PC, flags).", rrSchema);
    }

    {
        Json wrSchema(Json::OBJ);
        wrSchema.oVal["type"] = Json("object");
        Json wrProps(Json::OBJ);
        wrProps.oVal["machine_id"] = midProp;
        Json wrNameProp(Json::OBJ); wrNameProp.oVal["type"] = Json("string"); wrNameProp.oVal["description"] = Json("Name of the register (e.g. \"A\", \"X\", \"Y\", \"SP\")");
        wrProps.oVal["reg"] = wrNameProp;
        Json wrValProp(Json::OBJ); wrValProp.oVal["type"] = Json("integer"); wrValProp.oVal["description"] = Json("Value to write");
        wrProps.oVal["value"] = wrValProp;
        wrSchema.oVal["properties"] = wrProps;
        Json wrReq(Json::ARR); wrReq.push_back(Json("machine_id")); wrReq.push_back(Json("reg")); wrReq.push_back(Json("value"));
        wrSchema.oVal["required"] = wrReq;
        addTool("write_register", "Write a value to a CPU register.", wrSchema);
    }

    Json svmSchema(Json::OBJ);
    svmSchema.oVal["type"] = Json("object");
    Json svmProps(Json::OBJ);
    svmProps.oVal["machine_id"] = midProp;
    Json pathProp(Json::OBJ); pathProp.oVal["type"] = Json("string"); pathProp.oVal["description"] = Json("Destination file path");
    svmProps.oVal["path"] = pathProp;
    svmProps.oVal["addr"] = addrProp;
    svmProps.oVal["size"] = sizeProp;
    svmSchema.oVal["properties"] = svmProps;
    Json svmReq(Json::ARR); svmReq.push_back(Json("machine_id")); svmReq.push_back(Json("path")); svmReq.push_back(Json("addr")); svmReq.push_back(Json("size"));
    svmSchema.oVal["required"] = svmReq;
    addTool("save_memory", "Save a range of memory to a binary file.", svmSchema);

    Json scrSchema(Json::OBJ);
    scrSchema.oVal["type"] = Json("object");
    Json scrProps(Json::OBJ);
    scrProps.oVal["machine_id"] = midProp;
    scrProps.oVal["path"] = pathProp;
    scrSchema.oVal["properties"] = scrProps;
    Json scrReq(Json::ARR); scrReq.push_back(Json("machine_id")); scrReq.push_back(Json("path"));
    scrSchema.oVal["required"] = scrReq;
    addTool("screenshot", "Save a screenshot of the machine's video output to a PNG file.", scrSchema);

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

    Json gmSchema(Json::OBJ);
    gmSchema.oVal["type"] = Json("object");
    Json gmProps(Json::OBJ);
    gmProps.oVal["machine_id"] = midProp;
    gmSchema.oVal["properties"] = gmProps;
    Json gmReq(Json::ARR); gmReq.push_back(Json("machine_id"));
    gmSchema.oVal["required"] = gmReq;
    addTool("get_map_state", "Read current MEGA65 MAP configuration (block offsets and enable masks). Only available on mega65 machine.", gmSchema);

    Json smSchema2(Json::OBJ);
    smSchema2.oVal["type"] = Json("object");
    Json smProps2(Json::OBJ);
    smProps2.oVal["machine_id"] = midProp;
    Json offProp(Json::OBJ); offProp.oVal["type"] = Json("string"); offProp.oVal["description"] = Json("Comma-separated list of up to 8 block offsets (e.g. \"0,1000,2000,3000,0,0,0,0\")");
    smProps2.oVal["offsets"] = offProp;
    Json enProp(Json::OBJ); enProp.oVal["type"] = Json("integer"); enProp.oVal["description"] = Json("Enable mask (0-255, bit i enables block i)");
    smProps2.oVal["enables"] = enProp;
    smSchema2.oVal["properties"] = smProps2;
    Json smReq2(Json::ARR); smReq2.push_back(Json("machine_id")); smReq2.push_back(Json("offsets")); smReq2.push_back(Json("enables"));
    smSchema2.oVal["required"] = smReq2;
    addTool("set_map_state", "Configure MEGA65 address translation by setting MAP block offsets and enable mask. Only available on mega65 machine.", smSchema2);

    Json gpSchema(Json::OBJ);
    gpSchema.oVal["type"] = Json("object");
    Json gpProps(Json::OBJ);
    gpProps.oVal["machine_id"] = midProp;
    gpSchema.oVal["properties"] = gpProps;
    Json gpReq(Json::ARR); gpReq.push_back(Json("machine_id"));
    gpSchema.oVal["required"] = gpReq;
    addTool("get_personality", "Read current I/O personality mode (C64, C65, MEGA65, or ETHERNET). Only available if KEY register is present.", gpSchema);

    Json spSchema2(Json::OBJ);
    spSchema2.oVal["type"] = Json("object");
    Json spProps2(Json::OBJ);
    spProps2.oVal["machine_id"] = midProp;
    Json persProp(Json::OBJ); persProp.oVal["type"] = Json("string"); persProp.oVal["description"] = Json("Personality mode: C64, C65, MEGA65, or ETHERNET");
    spProps2.oVal["personality"] = persProp;
    spSchema2.oVal["properties"] = spProps2;
    Json spReq2(Json::ARR); spReq2.push_back(Json("machine_id")); spReq2.push_back(Json("personality"));
    spSchema2.oVal["required"] = spReq2;
    addTool("set_personality", "Switch I/O personality mode via KEY register knock sequence. Only available if KEY register is present.", spSchema2);

    Json gtSchema(Json::OBJ);
    gtSchema.oVal["type"] = Json("object");
    Json gtProps(Json::OBJ);
    gtProps.oVal["machine_id"] = midProp;
    Json limitProp(Json::OBJ); limitProp.oVal["type"] = Json("integer"); limitProp.oVal["description"] = Json("Maximum number of entries to return (default: all)");
    gtProps.oVal["limit"] = limitProp;
    gtSchema.oVal["properties"] = gtProps;
    Json gtReq(Json::ARR); gtReq.push_back(Json("machine_id"));
    gtSchema.oVal["required"] = gtReq;
    addTool("get_trace_buffer", "Retrieve instruction execution trace entries from the trace buffer. Each entry includes address, mnemonic, registers, and cycle count.", gtSchema);

    Json tbSchema(Json::OBJ);
    tbSchema.oVal["type"] = Json("object");
    Json tbProps(Json::OBJ);
    tbProps.oVal["machine_id"] = midProp;
    tbSchema.oVal["properties"] = tbProps;
    Json tbReq(Json::ARR); tbReq.push_back(Json("machine_id"));
    tbSchema.oVal["required"] = tbReq;
    addTool("clear_trace", "Clear all entries from the trace buffer.", tbSchema);

    Json stSchema(Json::OBJ);
    stSchema.oVal["type"] = Json("object");
    Json stProps(Json::OBJ);
    stProps.oVal["machine_id"] = midProp;
    Json filterProp(Json::OBJ); filterProp.oVal["type"] = Json("string"); filterProp.oVal["description"] = Json("Trace filter mode: all, instructions, breakpoints, memory, calls");
    stProps.oVal["filter"] = filterProp;
    stSchema.oVal["properties"] = stProps;
    Json stReq(Json::ARR); stReq.push_back(Json("machine_id")); stReq.push_back(Json("filter"));
    stSchema.oVal["required"] = stReq;
    addTool("set_trace_filter", "Configure which types of events are recorded in the trace buffer.", stSchema);

    Json mtSchema(Json::OBJ);
    mtSchema.oVal["type"] = Json("object");
    Json mtProps(Json::OBJ);
    mtProps.oVal["machine_id"] = midProp;
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

    addTool("list_instances", "List all currently running machine instances with their type and display name.", emptySchema);

    // create_machine
    Json mtypeProp(Json::OBJ); mtypeProp.oVal["type"] = Json("string"); mtypeProp.oVal["description"] = Json("Machine type from list_machines (e.g. c64, rawMega65)");
    Json cmSchema(Json::OBJ); cmSchema.oVal["type"] = Json("object");
    Json cmProps(Json::OBJ); cmProps.oVal["machine_type"] = mtypeProp;
    cmProps.oVal["machine_id"] = midProp;
    cmSchema.oVal["properties"] = cmProps;
    Json cmReq(Json::ARR); cmReq.push_back(Json("machine_type"));
    cmSchema.oVal["required"] = cmReq;
    addTool("create_machine",
        "Create a new machine instance. machine_type selects the hardware (see list_machines). "
        "machine_id is an optional user-chosen instance name; auto-generated (e.g. c64_1) if omitted. "
        "Returns the instance_id to use with all other tools.", cmSchema);

    // destroy_machine
    Json dmProps(Json::OBJ); dmProps.oVal["machine_id"] = midProp;
    Json dmSchema(Json::OBJ); dmSchema.oVal["type"] = Json("object");
    dmSchema.oVal["properties"] = dmProps;
    Json dmReq(Json::ARR); dmReq.push_back(Json("machine_id"));
    dmSchema.oVal["required"] = dmReq;
    addTool("destroy_machine", "Destroy a running machine instance and release all its resources.", dmSchema);

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
    runProps.oVal["ignore_program_end"] = ipeProp;
    Json sobProp(Json::OBJ); sobProp.oVal["type"] = Json("boolean");
    sobProp.oVal["description"] = Json("If true, stop immediately when PC points to a BRK instruction (opcode $00), BEFORE executing it. Useful for debugging crashes — use reverse_step to trace back.");
    runProps.oVal["stop_on_brk"] = sobProp;
    runSchema.oVal["properties"] = runProps;
    Json runReq(Json::ARR); runReq.push_back(Json("machine_id"));
    runSchema.oVal["required"] = runReq;
    addTool("run_cpu", "Run the CPU until a breakpoint is hit, the program ends (BRK/RTS to empty stack), BRK opcode (if stop_on_brk), or max_steps is reached (default 10000000)", runSchema);

    // run_until
    {
        Json ruSchema(Json::OBJ); ruSchema.oVal["type"] = Json("object");
        Json ruProps(Json::OBJ);
        ruProps.oVal["machine_id"] = midProp;
        Json condProp(Json::OBJ); condProp.oVal["type"] = Json("string");
        condProp.oVal["description"] = Json("Expression that stops execution when it evaluates to non-zero. "
            "Supports registers (A, X, Y, Z, SP, PC, B, P), hex ($FF), comparisons (==, !=, <, >, <=, >=), "
            "arithmetic (+, -, *, /, %, <<, >>), bitwise (&, |), logical (&&, ||, !), and symbols. "
            "Examples: 'PC >= $FFFF', 'SP >> 8 == $FF', 'PC < $0800 || PC > $9FFF', 'A == $42 && X != 0'");
        ruProps.oVal["condition"] = condProp;
        Json msProp(Json::OBJ); msProp.oVal["type"] = Json("integer");
        msProp.oVal["description"] = Json("Maximum instructions to execute before stopping (default 10000000)");
        ruProps.oVal["max_steps"] = msProp;
        Json rptProp(Json::OBJ); rptProp.oVal["type"] = Json("string");
        rptProp.oVal["description"] = Json("Comma-separated report items: regs, disasm, stack, mem:ADDR:SIZE (e.g. 'regs,disasm,mem:$0400:32'). Default: regs");
        ruProps.oVal["report"] = rptProp;
        ruProps.oVal["ignore_program_end"] = ipeProp;
        Json looseProp(Json::OBJ); looseProp.oVal["type"] = Json("boolean");
        looseProp.oVal["description"] = Json("If true, check condition every 256 steps instead of every step (faster but may miss transient conditions). Default: false (check every step).");
        ruProps.oVal["loose"] = looseProp;
        ruSchema.oVal["properties"] = ruProps;
        Json ruReq(Json::ARR); ruReq.push_back(Json("machine_id")); ruReq.push_back(Json("condition"));
        ruSchema.oVal["required"] = ruReq;
        addTool("run_until", "Run the CPU until a condition expression becomes true, a breakpoint hits, program ends, or max_steps is reached. "
            "Checks condition every instruction by default (use loose=true to check every 256 steps for performance). "
            "Returns registers, disassembly, and optional memory dumps.", ruSchema);
    }

    // -----------------------------------------------------------------------
    // Test automation tools (#71)
    // -----------------------------------------------------------------------

    // test_sequence — Phase 1: batch execute commands
    {
        Json tsSchema(Json::OBJ); tsSchema.oVal["type"] = Json("object");
        Json tsProps(Json::OBJ);
        tsProps.oVal["machine_id"] = midProp;
        Json cmdsProp(Json::OBJ); cmdsProp.oVal["type"] = Json("array");
        cmdsProp.oVal["description"] = Json(
            "Array of tool calls to execute sequentially. Each element is an object with "
            "'tool' (string) and 'args' (object) fields. Example: "
            "[{\"tool\": \"set_pc\", \"args\": {\"addr\": \"$2000\"}}, "
            "{\"tool\": \"run_cpu\", \"args\": {\"max_steps\": 50000}}, "
            "{\"tool\": \"read_registers\"}]. "
            "Args inherit machine_id from the top-level parameter automatically.");
        Json cmdItemSchema(Json::OBJ); cmdItemSchema.oVal["type"] = Json("object");
        cmdsProp.oVal["items"] = cmdItemSchema;
        tsProps.oVal["commands"] = cmdsProp;
        tsSchema.oVal["properties"] = tsProps;
        Json tsReq(Json::ARR); tsReq.push_back(Json("machine_id")); tsReq.push_back(Json("commands"));
        tsSchema.oVal["required"] = tsReq;
        addTool("test_sequence",
            "Execute a batch of tool calls in sequence on a machine, returning all results in one response. "
            "Eliminates multiple round-trips. Each command's args automatically inherits machine_id. "
            "If any command fails, subsequent commands still execute (results include per-command status). "
            "Supports all existing tools: step_cpu, run_cpu, run_until, set_pc, set_breakpoint, "
            "read_memory, write_memory, read_registers, load_image, disassemble, assemble, etc.",
            tsSchema);
    }

    // test_assert — Phase 2: load + run + check assertions
    {
        Json taSchema(Json::OBJ); taSchema.oVal["type"] = Json("object");
        Json taProps(Json::OBJ);
        taProps.oVal["machine_id"] = midProp;

        Json loadProp(Json::OBJ); loadProp.oVal["type"] = Json("string");
        loadProp.oVal["description"] = Json("Path to .prg or .bin image to load (optional — skip to test current state)");
        taProps.oVal["load"] = loadProp;

        Json entryProp(Json::OBJ); entryProp.oVal["type"] = Json("string");
        entryProp.oVal["description"] = Json("Entry point address expression (default: use PRG load address or current PC)");
        taProps.oVal["entry"] = entryProp;

        Json tcProp(Json::OBJ); tcProp.oVal["type"] = Json("integer");
        tcProp.oVal["description"] = Json("Maximum CPU steps before timeout (default 10000000)");
        taProps.oVal["timeout_steps"] = tcProp;

        taProps.oVal["ignore_program_end"] = ipeProp;
        taProps.oVal["stop_on_brk"] = sobProp;

        Json assertsProp(Json::OBJ); assertsProp.oVal["type"] = Json("object");
        assertsProp.oVal["description"] = Json(
            "Assertion specifications. All are optional. Object with fields: "
            "'memory' — object mapping address expressions to expected byte arrays, e.g. {\"$4000\": [1,2,3]}. "
            "'registers' — object mapping register names to expected values, e.g. {\"A\": 66, \"X\": 0}. "
            "'halt_pc' — address expression where execution should stop. "
            "'exit_type' — expected stop reason: 'breakpoint', 'program_end', or 'max_steps'.");
        taProps.oVal["assertions"] = assertsProp;

        taSchema.oVal["properties"] = taProps;
        Json taReq(Json::ARR); taReq.push_back(Json("machine_id"));
        taSchema.oVal["required"] = taReq;
        addTool("test_assert",
            "Load a program, run it, and check assertions. Returns structured pass/fail with detailed "
            "failure information for each assertion. Combines load_image + set_pc + run_cpu + memory/register "
            "checks into a single call. Ideal for automated test validation.",
            taSchema);
    }

    // test_diagnose — Phase 3: watchpoint-driven root cause analysis
    {
        Json tdSchema(Json::OBJ); tdSchema.oVal["type"] = Json("object");
        Json tdProps(Json::OBJ);
        tdProps.oVal["machine_id"] = midProp;

        Json loadProp2(Json::OBJ); loadProp2.oVal["type"] = Json("string");
        loadProp2.oVal["description"] = Json("Path to .prg or .bin image to load (optional)");
        tdProps.oVal["load"] = loadProp2;

        Json entryProp2(Json::OBJ); entryProp2.oVal["type"] = Json("string");
        entryProp2.oVal["description"] = Json("Entry point address expression");
        tdProps.oVal["entry"] = entryProp2;

        Json tcProp2(Json::OBJ); tcProp2.oVal["type"] = Json("integer");
        tcProp2.oVal["description"] = Json("Maximum CPU steps (default 10000000)");
        tdProps.oVal["timeout_steps"] = tcProp2;

        tdProps.oVal["ignore_program_end"] = ipeProp;

        Json watchAddrProp(Json::OBJ); watchAddrProp.oVal["type"] = Json("string");
        watchAddrProp.oVal["description"] = Json("Address expression to set a write watchpoint on. "
            "When the CPU writes to this address, execution stops and the tool reports "
            "which instruction wrote there, with registers, disassembly, and trace context.");
        tdProps.oVal["watch_addr"] = watchAddrProp;

        Json watchTypeProp(Json::OBJ); watchTypeProp.oVal["type"] = Json("string");
        watchTypeProp.oVal["description"] = Json("Watchpoint type: 'write' (default) or 'read'");
        tdProps.oVal["watch_type"] = watchTypeProp;

        Json traceDepthProp(Json::OBJ); traceDepthProp.oVal["type"] = Json("integer");
        traceDepthProp.oVal["description"] = Json("Number of trace entries to include before the watchpoint hit (default 20)");
        tdProps.oVal["trace_depth"] = traceDepthProp;

        Json expectedProp(Json::OBJ); expectedProp.oVal["type"] = Json("array");
        expectedProp.oVal["description"] = Json("Expected byte values at watch_addr. If provided, reports the "
            "mismatch between expected and actual values after the write.");
        Json expectedItemProp(Json::OBJ); expectedItemProp.oVal["type"] = Json("integer");
        expectedProp.oVal["items"] = expectedItemProp;
        tdProps.oVal["expected"] = expectedProp;

        tdSchema.oVal["properties"] = tdProps;
        Json tdReq(Json::ARR); tdReq.push_back(Json("machine_id")); tdReq.push_back(Json("watch_addr"));
        tdSchema.oVal["required"] = tdReq;
        addTool("test_diagnose",
            "Root-cause analysis tool. Sets a watchpoint on an address, runs the program, and when the "
            "watchpoint fires, reports the instruction that triggered it with full context: PC, registers, "
            "disassembly of surrounding code, trace buffer history, and optional expected-vs-actual comparison. "
            "Use this after test_assert identifies a failing memory location to find what wrote the wrong value.",
            tdSchema);
    }

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
    Json bpCondProp(Json::OBJ); bpCondProp.oVal["type"] = Json("string");
    bpCondProp.oVal["description"] = Json("Expression that must be true for the breakpoint to fire. Supports registers, comparisons, memory dereference (*addr), logical operators.");
    sbpProps.oVal["condition"] = bpCondProp;
    Json physProp(Json::OBJ); physProp.oVal["type"] = Json("boolean");
    physProp.oVal["description"] = Json("If true, match against 28-bit physical address (resolved via MapMmu) instead of logical CPU address. Useful for MEGA65 where the same code appears at multiple logical addresses.");
    sbpProps.oVal["physical"] = physProp;
    sbpSchema.oVal["properties"] = sbpProps;
    Json sbpReq(Json::ARR); sbpReq.push_back(Json("machine_id")); sbpReq.push_back(Json("addr"));
    sbpSchema.oVal["required"] = sbpReq;
    addTool("set_breakpoint", "Set an execution breakpoint at an address. Returns the breakpoint ID.", sbpSchema);

    // set_watchpoint
    Json swpSchema(Json::OBJ); swpSchema.oVal["type"] = Json("object");
    Json swpProps(Json::OBJ); swpProps.oVal["machine_id"] = midProp; swpProps.oVal["addr"] = addrProp;
    Json wpTypeProp(Json::OBJ); wpTypeProp.oVal["type"] = Json("string"); wpTypeProp.oVal["description"] = Json("Watchpoint type: \"read\" or \"write\"");
    swpProps.oVal["type"] = wpTypeProp;
    swpProps.oVal["condition"] = bpCondProp;
    swpProps.oVal["physical"] = physProp;
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
    {
        Json lbSchema(Json::OBJ); lbSchema.oVal["type"] = Json("object");
        Json lbProps(Json::OBJ); lbProps.oVal["machine_id"] = midProp;
        lbSchema.oVal["properties"] = lbProps;
        Json lbReq(Json::ARR); lbReq.push_back(Json("machine_id"));
        lbSchema.oVal["required"] = lbReq;
        addTool("list_breakpoints", "List all breakpoints and watchpoints with their IDs, addresses, types, and enabled status", lbSchema);
    }

    // get_stack
    Json gsSchema(Json::OBJ); gsSchema.oVal["type"] = Json("object");
    Json gsProps(Json::OBJ); gsProps.oVal["machine_id"] = midProp; gsProps.oVal["count"] = cntProp;
    gsSchema.oVal["properties"] = gsProps;
    Json gsReq(Json::ARR); gsReq.push_back(Json("machine_id"));
    gsSchema.oVal["required"] = gsReq;
    addTool("get_stack", "Read bytes from the CPU stack. count defaults to 8 entries; use 0 for all (SP to $01FF).", gsSchema);

    // list_devices
    {
        Json ldSchema(Json::OBJ); ldSchema.oVal["type"] = Json("object");
        Json ldProps(Json::OBJ); ldProps.oVal["machine_id"] = midProp;
        ldSchema.oVal["properties"] = ldProps;
        Json ldReq(Json::ARR); ldReq.push_back(Json("machine_id"));
        ldSchema.oVal["required"] = ldReq;
        addTool("list_devices", "List all IO devices registered in the machine (name, address range)", ldSchema);
    }

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

    // set_assembler tool
    Json setAsmSchema(Json::OBJ);
    setAsmSchema.oVal["type"] = Json("object");
    Json setAsmProps(Json::OBJ);
    setAsmProps.oVal["machine_id"] = midProp;
    Json asmNameProp(Json::OBJ);
    asmNameProp.oVal["type"] = Json("string");
    asmNameProp.oVal["description"] = Json("Assembler name (e.g. 'ca45', 'kickAssembler')");
    setAsmProps.oVal["assembler_name"] = asmNameProp;
    setAsmSchema.oVal["properties"] = setAsmProps;
    Json setAsmReq(Json::ARR); setAsmReq.push_back(Json("machine_id")); setAsmReq.push_back(Json("assembler_name"));
    setAsmSchema.oVal["required"] = setAsmReq;
    addTool("set_assembler", "Override the assembler for a specific machine. Returns the new assembler name or error.", setAsmSchema);

    // get_assembler tool
    Json getAsmSchema(Json::OBJ);
    getAsmSchema.oVal["type"] = Json("object");
    Json getAsmProps(Json::OBJ);
    getAsmProps.oVal["machine_id"] = midProp;
    getAsmSchema.oVal["properties"] = getAsmProps;
    Json getAsmReq(Json::ARR); getAsmReq.push_back(Json("machine_id"));
    getAsmSchema.oVal["required"] = getAsmReq;
    addTool("get_assembler", "Get the current assembler for a machine. Returns the assembler name.", getAsmSchema);

    // diff_file — compare two binary files
    {
        Json schema(Json::OBJ);
        schema.oVal["type"] = Json("object");
        Json props(Json::OBJ);

        Json fileAProp(Json::OBJ);
        fileAProp.oVal["type"] = Json("string");
        fileAProp.oVal["description"] = Json("Path to first file");
        props.oVal["file_a"] = fileAProp;

        Json fileBProp(Json::OBJ);
        fileBProp.oVal["type"] = Json("string");
        fileBProp.oVal["description"] = Json("Path to second file");
        props.oVal["file_b"] = fileBProp;

        Json baseProp(Json::OBJ);
        baseProp.oVal["type"] = Json("string");
        baseProp.oVal["description"] = Json("Base address for display (e.g. $E000 for KERNAL). Default: $0000");
        props.oVal["base_addr"] = baseProp;

        props.oVal["machine_id"] = midProp;

        Json ctxProp(Json::OBJ);
        ctxProp.oVal["type"] = Json("number");
        ctxProp.oVal["description"] = Json("Context bytes to show around each diff region (default: 0)");
        props.oVal["context"] = ctxProp;

        schema.oVal["properties"] = props;
        Json req(Json::ARR);
        req.push_back(Json("file_a"));
        req.push_back(Json("file_b"));
        schema.oVal["required"] = req;
        addTool("diff_file",
                "Compare two binary files byte-by-byte. Returns a structured diff report "
                "with changed regions, 6502 vector table comparison (when applicable), "
                "and optional symbol annotations. "
                "Useful for comparing ROM revisions, KERNAL patches, or disk images.",
                schema);
    }

    // snapshot_save — capture machine state
    {
        Json schema(Json::OBJ);
        schema.oVal["type"] = Json("object");
        Json props(Json::OBJ);
        props.oVal["machine_id"] = midProp;

        Json nameProp(Json::OBJ);
        nameProp.oVal["type"] = Json("string");
        nameProp.oVal["description"] = Json("Name for this snapshot (e.g. \"before\", \"after\")");
        props.oVal["name"] = nameProp;

        Json rangeProp(Json::OBJ);
        rangeProp.oVal["type"] = Json("string");
        rangeProp.oVal["description"] = Json("Memory range to capture (e.g. \"$0000-$FFFF\"). Default: full 16-bit address space.");
        props.oVal["range"] = rangeProp;

        schema.oVal["properties"] = props;
        Json req(Json::ARR);
        req.push_back(Json("machine_id"));
        req.push_back(Json("name"));
        schema.oVal["required"] = req;
        addTool("snapshot_save",
                "Save a named snapshot of the current machine state (CPU registers and memory). "
                "Use with snapshot_diff to compare two points in execution.",
                schema);
    }

    // snapshot_diff — compare two snapshots
    {
        Json schema(Json::OBJ);
        schema.oVal["type"] = Json("object");
        Json props(Json::OBJ);
        props.oVal["machine_id"] = midProp;

        Json snapAProp(Json::OBJ);
        snapAProp.oVal["type"] = Json("string");
        snapAProp.oVal["description"] = Json("Name of first snapshot");
        props.oVal["snapshot_a"] = snapAProp;

        Json snapBProp(Json::OBJ);
        snapBProp.oVal["type"] = Json("string");
        snapBProp.oVal["description"] = Json("Name of second snapshot");
        props.oVal["snapshot_b"] = snapBProp;

        schema.oVal["properties"] = props;
        Json req(Json::ARR);
        req.push_back(Json("machine_id"));
        req.push_back(Json("snapshot_a"));
        req.push_back(Json("snapshot_b"));
        schema.oVal["required"] = req;
        addTool("snapshot_diff",
                "Compare two named snapshots of a machine. Reports register changes "
                "and memory changes grouped by contiguous region, with symbol annotations.",
                schema);
    }

    // snapshot_list — list saved snapshots
    {
        Json schema(Json::OBJ);
        schema.oVal["type"] = Json("object");
        Json props(Json::OBJ);
        props.oVal["machine_id"] = midProp;
        schema.oVal["properties"] = props;
        Json req(Json::ARR);
        req.push_back(Json("machine_id"));
        schema.oVal["required"] = req;
        addTool("snapshot_list",
                "List all saved snapshots for a machine instance.",
                schema);
    }

    // snapshot_delete — remove a snapshot
    {
        Json schema(Json::OBJ);
        schema.oVal["type"] = Json("object");
        Json props(Json::OBJ);
        props.oVal["machine_id"] = midProp;

        Json nameProp(Json::OBJ);
        nameProp.oVal["type"] = Json("string");
        nameProp.oVal["description"] = Json("Name of snapshot to delete, or \"*\" to delete all");
        props.oVal["name"] = nameProp;

        schema.oVal["properties"] = props;
        Json req(Json::ARR);
        req.push_back(Json("machine_id"));
        req.push_back(Json("name"));
        schema.oVal["required"] = req;
        addTool("snapshot_delete",
                "Delete a named snapshot (or all snapshots with \"*\").",
                schema);
    }

    // analyze_routine — control-flow analysis from an entry point
    {
        Json schema(Json::OBJ);
        schema.oVal["type"] = Json("object");
        Json props(Json::OBJ);
        props.oVal["machine_id"] = midProp;
        props.oVal["addr"] = addrProp;

        Json maxProp(Json::OBJ);
        maxProp.oVal["type"] = Json("number");
        maxProp.oVal["description"] = Json("Maximum instructions to analyze (default: 200)");
        props.oVal["max_instructions"] = maxProp;

        Json recProp(Json::OBJ);
        recProp.oVal["type"] = Json("boolean");
        recProp.oVal["description"] = Json("Follow into subroutines (JSR targets). Default: false.");
        props.oVal["recursive"] = recProp;

        schema.oVal["properties"] = props;
        Json req(Json::ARR);
        req.push_back(Json("machine_id"));
        req.push_back(Json("addr"));
        schema.oVal["required"] = req;
        addTool("analyze_routine",
                "Analyze a routine by walking its control flow from an entry point. "
                "Disassembles instructions, follows branches and jumps, identifies loops "
                "(backward branches), subroutine calls, I/O accesses, and exit points. "
                "Set recursive=true to also analyze called subroutines. "
                "Returns a structured analysis report with symbol annotations.",
                schema);
    }

    // generate_tests — automated test vector generation
    {
        Json schema(Json::OBJ);
        schema.oVal["type"] = Json("object");
        Json props(Json::OBJ);
        props.oVal["machine_id"] = midProp;
        props.oVal["addr"] = addrProp;

        Json inRegsProp(Json::OBJ);
        inRegsProp.oVal["type"] = Json("array");
        inRegsProp.oVal["description"] = Json("Register names to vary as inputs (default: [\"A\"])");
        Json inRegsItems(Json::OBJ);
        inRegsItems.oVal["type"] = Json("string");
        inRegsProp.oVal["items"] = inRegsItems;
        props.oVal["input_regs"] = inRegsProp;

        Json outRegsProp(Json::OBJ);
        outRegsProp.oVal["type"] = Json("array");
        outRegsProp.oVal["description"] = Json("Register names to capture as outputs (default: [\"A\", \"P\"])");
        Json outRegsItems(Json::OBJ);
        outRegsItems.oVal["type"] = Json("string");
        outRegsProp.oVal["items"] = outRegsItems;
        props.oVal["output_regs"] = outRegsProp;

        Json valuesProp(Json::OBJ);
        valuesProp.oVal["type"] = Json("array");
        valuesProp.oVal["description"] = Json(
            "Test values for each input register (default: [0, 1, 127, 128, 254, 255]). "
            "All combinations are tested.");
        Json valItems(Json::OBJ);
        valItems.oVal["type"] = Json("number");
        valuesProp.oVal["items"] = valItems;
        props.oVal["values"] = valuesProp;

        Json maxStepsProp(Json::OBJ);
        maxStepsProp.oVal["type"] = Json("number");
        maxStepsProp.oVal["description"] = Json("Max instructions per test run (default: 10000)");
        props.oVal["max_steps"] = maxStepsProp;

        schema.oVal["properties"] = props;
        Json req(Json::ARR);
        req.push_back(Json("machine_id"));
        req.push_back(Json("addr"));
        schema.oVal["required"] = req;
        addTool("generate_tests",
                "Run a routine with varied register inputs and capture outputs as test vectors. "
                "Iterates over all combinations of input values, setting input registers, "
                "running until the routine returns (RTS/RTI/BRK), and recording output registers. "
                "Returns a table of input→output mappings suitable for regression testing.",
                schema);
    }

    // record_audio — run CPU and record audio to WAV file
    {
        Json schema(Json::OBJ);
        schema.oVal["type"] = Json("object");
        Json props(Json::OBJ);
        props.oVal["machine_id"] = midProp;

        Json fileProp(Json::OBJ);
        fileProp.oVal["type"] = Json("string");
        fileProp.oVal["description"] = Json("Output WAV file path");
        props.oVal["file"] = fileProp;

        Json durProp(Json::OBJ);
        durProp.oVal["type"] = Json("number");
        durProp.oVal["description"] = Json("Recording duration in milliseconds (default: 1000)");
        props.oVal["duration_ms"] = durProp;

        schema.oVal["properties"] = props;
        Json req(Json::ARR);
        req.push_back(Json("machine_id"));
        req.push_back(Json("file"));
        schema.oVal["required"] = req;
        addTool("record_audio",
                "Run the CPU while recording audio output to a WAV file. "
                "Executes the machine for the specified duration, pulling audio samples "
                "from the SID/VIC/POKEY device, and saves as 16-bit PCM WAV. "
                "Supports both mono and stereo (dual SID) output.",
                schema);
    }

    // load_sid — load and initialize a .sid file
    {
        Json schema(Json::OBJ);
        schema.oVal["type"] = Json("object");
        Json props(Json::OBJ);
        props.oVal["machine_id"] = midProp;

        Json fileProp(Json::OBJ);
        fileProp.oVal["type"] = Json("string");
        fileProp.oVal["description"] = Json("Path to .sid/.psid file");
        props.oVal["file"] = fileProp;

        Json subtuneProp(Json::OBJ);
        subtuneProp.oVal["type"] = Json("number");
        subtuneProp.oVal["description"] = Json("Subtune number (1-based, default: startSong from header)");
        props.oVal["subtune"] = subtuneProp;

        schema.oVal["properties"] = props;
        Json req(Json::ARR);
        req.push_back(Json("machine_id"));
        req.push_back(Json("file"));
        schema.oVal["required"] = req;
        addTool("load_sid",
                "Load a PSID/RSID (.sid) file into a machine. Parses the header, "
                "loads the SID data into memory, calls the init routine with the "
                "selected subtune in the accumulator, and returns song metadata "
                "(title, author, copyright, play address, number of songs). "
                "Use record_audio after loading to capture playback.",
                schema);
    }

    // profile_cpu — hotspot analysis (#19)
    {
        Json schema(Json::OBJ); schema.oVal["type"] = Json("object");
        Json props(Json::OBJ); props.oVal["machine_id"] = midProp;
        Json stepsProp(Json::OBJ); stepsProp.oVal["type"] = Json("integer");
        stepsProp.oVal["description"] = Json("Number of CPU steps to profile (default 1000000)");
        props.oVal["steps"] = stepsProp;
        Json topProp(Json::OBJ); topProp.oVal["type"] = Json("integer");
        topProp.oVal["description"] = Json("Number of top hotspots to return (default 20)");
        props.oVal["top"] = topProp;
        schema.oVal["properties"] = props;
        Json req(Json::ARR); req.push_back(Json("machine_id"));
        schema.oVal["required"] = req;
        addTool("profile_cpu",
                "Run the CPU for N steps, sampling PC at each instruction to build a "
                "frequency histogram. Returns the top N addresses by execution count "
                "with symbol annotations and percentages. Useful for hotspot analysis.",
                schema);
    }

    // measure_region — cycle counting (#19)
    {
        Json schema(Json::OBJ); schema.oVal["type"] = Json("object");
        Json props(Json::OBJ); props.oVal["machine_id"] = midProp;
        props.oVal["addr"] = addrProp;
        Json endProp(Json::OBJ); endProp.oVal["type"] = Json("integer");
        endProp.oVal["description"] = Json("End address of the region (exclusive). Execution stops when PC leaves [addr, end_addr).");
        props.oVal["end_addr"] = endProp;
        Json maxProp(Json::OBJ); maxProp.oVal["type"] = Json("integer");
        maxProp.oVal["description"] = Json("Maximum steps before giving up (default 10000000)");
        props.oVal["max_steps"] = maxProp;
        schema.oVal["properties"] = props;
        Json req(Json::ARR); req.push_back(Json("machine_id")); req.push_back(Json("addr")); req.push_back(Json("end_addr"));
        schema.oVal["required"] = req;
        addTool("measure_region",
                "Set PC to addr and run until PC leaves the address range [addr, end_addr). "
                "Returns total cycles consumed, instruction count, and average cycles per instruction. "
                "Useful for benchmarking specific code regions.",
                schema);
    }

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
            if (ms->dbg) ms->dbg->resume();  // clear any prior breakpoint pause
            int ran = 0;
            for (int i = 0; i < count; ++i) {
                if (ms->machine && ms->machine->schedulerStep)
                    ms->machine->schedulerStep(*ms->machine);
                else
                    ms->cpu->step();
                ++ran;
                if (ms->dbg && ms->dbg->isPaused()) break;
                bool ignPE = args.contains("ignore_program_end") && args["ignore_program_end"].bVal;
                if (!ignPE && ms->cpu->isProgramEnd(ms->bus)) break;
            }
            textItem.oVal["text"] = Json("Executed " + std::to_string(ran) + " instructions.");
        }
    } else if (name == "reverse_step") {
        std::string mid = args["machine_id"].sVal;
        int count = args.contains("count") ? (int)args["count"].nVal : 1;
        if (count < 1) count = 1;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else if (!ms->dbg) {
            textItem.oVal["text"] = Json("Error: No debug context available");
            textItem.oVal["isError"] = Json(true);
        } else {
            int reversed = 0;
            for (int i = 0; i < count; ++i) {
                if (!ms->dbg->reverseStep()) break;
                ++reversed;
            }
            std::stringstream ss;
            if (reversed == 0) {
                ss << "No undo history available (trace buffer empty).";
            } else {
                ss << "Reversed " << reversed << " instruction" << (reversed > 1 ? "s" : "") << ". ";
                // Show current registers
                int regCount = ms->cpu->regCount();
                for (int i = 0; i < regCount; ++i) {
                    const auto* desc = ms->cpu->regDescriptor(i);
                    if (desc->flags & REGFLAG_INTERNAL) continue;
                    uint32_t val = ms->cpu->regRead(i);
                    ss << desc->name << ": $"
                       << toHex(val, desc->width == RegWidth::R16 ? 4 : 2) << "  ";
                }
            }
            textItem.oVal["text"] = Json(ss.str());
        }

    } else if (name == "undo_info") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else if (!ms->dbg) {
            textItem.oVal["text"] = Json("Error: No debug context available");
            textItem.oVal["isError"] = Json(true);
        } else {
            auto& tb = ms->dbg->trace();
            std::stringstream ss;
            ss << "Undo buffer: " << tb.size() << " / " << tb.capacity() << " entries\n";
            ss << "Reversible steps available: " << tb.size() << "\n";
            // Estimate memory usage
            size_t memEst = tb.size() * (sizeof(TraceEntry) + 20); // rough estimate
            if (memEst > 1024 * 1024) {
                ss << "Estimated memory: " << (memEst / (1024*1024)) << " MB\n";
            } else {
                ss << "Estimated memory: " << (memEst / 1024) << " KB\n";
            }
            textItem.oVal["text"] = Json(ss.str());
        }

    } else if (name == "set_pc") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            uint32_t addr;
            std::string errMsg;
            if (resolveAddrWithDiagnostic(args["addr"], ms->dbg, addr, errMsg)) {
                ms->cpu->setPc(addr);
                textItem.oVal["text"] = Json("PC set to $" + toHex(addr));
            } else {
                textItem.oVal["text"] = Json("Error: " + errMsg);
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
            std::string errMsg;
            if (!resolveAddrWithDiagnostic(args["addr"], ms->dbg, addr, errMsg)) {
                textItem.oVal["text"] = Json("Error: addr parameter - " + errMsg);
                textItem.oVal["isError"] = Json(true);
            } else if (!resolveAddrWithDiagnostic(args["size"], ms->dbg, size, errMsg)) {
                textItem.oVal["text"] = Json("Error: size parameter - " + errMsg);
                textItem.oVal["isError"] = Json(true);
            } else {
                std::stringstream ss;
                for (uint32_t i = 0; i < size; i += 16) {
                    ss << std::hex << std::setw(4) << std::setfill('0') << (addr + i) << ": ";
                    for (uint32_t j = 0; j < 16 && (i + j) < size; ++j) {
                        ss << std::setw(2) << (int)ms->bus->peek8(addr + i + j) << " ";
                    }
                    ss << "\n";
                }
                textItem.oVal["text"] = Json(ss.str());
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
            std::string errMsg;
            if (!resolveAddrWithDiagnostic(args["addr"], ms->dbg, addr, errMsg)) {
                textItem.oVal["text"] = Json("Error: " + errMsg);
                textItem.oVal["isError"] = Json(true);
            } else {
                for (size_t i = 0; i < bytes.aVal.size(); ++i) {
                    ms->bus->write8(addr + i, (uint8_t)bytes.aVal[i].nVal);
                }
                textItem.oVal["text"] = Json("Wrote " + std::to_string(bytes.aVal.size()) + " bytes at $" + toHex(addr));
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
    } else if (name == "write_register") {
        std::string mid = args["machine_id"].sVal;
        std::string regName = args["reg"].sVal;
        uint32_t val = (uint32_t)args["value"].nVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            int idx = ms->cpu->regIndexByName(regName.c_str());
            if (idx >= 0) {
                ms->cpu->regWrite(idx, val);
                textItem.oVal["text"] = Json("Wrote $" + toHex(val) + " to register " + regName);
            } else {
                textItem.oVal["text"] = Json("Error: Unknown register '" + regName + "'");
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "save_memory") {
        std::string mid = args["machine_id"].sVal;
        std::string path = args["path"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            uint32_t addr, size;
            std::string errMsg;
            if (resolveAddrWithDiagnostic(args["addr"], ms->dbg, addr, errMsg) &&
                resolveAddrWithDiagnostic(args["size"], ms->dbg, size, errMsg)) {
                FILE* f = fopen(path.c_str(), "wb");
                if (f) {
                    for (uint32_t i = 0; i < size; ++i) {
                        fputc(ms->bus->read8(addr + i), f);
                    }
                    fclose(f);
                    textItem.oVal["text"] = Json("Saved " + std::to_string(size) + " bytes to " + path);
                } else {
                    textItem.oVal["text"] = Json("Error: Failed to open file for writing: " + path);
                    textItem.oVal["isError"] = Json(true);
                }
            } else {
                textItem.oVal["text"] = Json("Error: " + errMsg);
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "screenshot") {
        std::string mid = args["machine_id"].sVal;
        std::string path = args["path"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            IVideoOutput* video = nullptr;
            if (ms->machine->ioRegistry) {
                std::vector<IOHandler*> handlers;
                ms->machine->ioRegistry->enumerate(handlers);
                for (auto* handler : handlers) {
                    video = dynamic_cast<IVideoOutput*>(handler);
                    if (video) break;
                }
            }
            if (video) {
                if (video->exportPng(path)) {
                    textItem.oVal["text"] = Json("Screenshot saved to " + path);
                } else {
                    textItem.oVal["text"] = Json("Error: Failed to save screenshot to " + path);
                    textItem.oVal["isError"] = Json(true);
                }
            } else {
                textItem.oVal["text"] = Json("Error: No video output device found");
                textItem.oVal["isError"] = Json(true);
            }
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
    } else if (name == "get_map_state") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            IMapController* mapCtrl = ms->cpu->getMapMmu();
            if (!mapCtrl) {
                textItem.oVal["text"] = Json("Error: Machine does not have MAP controller (only mega65 supports this)");
                textItem.oVal["isError"] = Json(true);
            } else {
                const MapState& state = mapCtrl->getMapState();
                std::stringstream ss;
                ss << "MAP State:\n";
                ss << "  Enables: $" << std::hex << std::setw(2) << std::setfill('0') << (int)state.enables << "\n";
                for (int i = 0; i < 8; i++) {
                    ss << "  Block " << i << ": offset=$" << std::hex << std::setw(5) << std::setfill('0') << state.offsets[i];
                    ss << " (enabled=" << ((state.enables & (1 << i)) ? "yes" : "no") << ")\n";
                }
                textItem.oVal["text"] = Json(ss.str());
            }
        }
    } else if (name == "set_map_state") {
        std::string mid = args["machine_id"].sVal;
        std::string offsetsStr = args["offsets"].sVal;
        uint8_t enables = (uint8_t)(int)args["enables"].nVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            IMapController* mapCtrl = ms->cpu->getMapMmu();
            if (!mapCtrl) {
                textItem.oVal["text"] = Json("Error: Machine does not have MAP controller (only mega65 supports this)");
                textItem.oVal["isError"] = Json(true);
            } else {
                MapState newState = mapCtrl->getMapState();
                newState.enables = enables;
                std::stringstream pss(offsetsStr);
                std::string token;
                int idx = 0;
                while (idx < 8 && std::getline(pss, token, ',')) {
                    try {
                        newState.offsets[idx] = std::stoul(token, nullptr, 0);
                        idx++;
                    } catch (...) {}
                }
                mapCtrl->setMapState(newState);
                std::stringstream ss;
                ss << "MAP state updated with " << idx << " offsets, enables=$" << std::hex << (int)enables;
                textItem.oVal["text"] = Json(ss.str());
            }
        }
    } else if (name == "get_personality") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            IOHandler* keyReg = ms->machine->ioRegistry ? ms->machine->ioRegistry->findHandler("KEY") : nullptr;
            if (!keyReg) {
                textItem.oVal["text"] = Json("Error: Machine does not have KEY register (I/O personality not available)");
                textItem.oVal["isError"] = Json(true);
            } else {
                KeyRegister* kr = static_cast<KeyRegister*>(keyReg);
                std::string modeStr;
                switch (kr->getCurrentPersonality()) {
                    case IopersonalityMode::C64: modeStr = "C64"; break;
                    case IopersonalityMode::C65: modeStr = "C65"; break;
                    case IopersonalityMode::MEGA65: modeStr = "MEGA65"; break;
                    case IopersonalityMode::ETHERNET: modeStr = "ETHERNET"; break;
                    default: modeStr = "UNKNOWN"; break;
                }
                textItem.oVal["text"] = Json("Current personality: " + modeStr);
            }
        }
    } else if (name == "set_personality") {
        std::string mid = args["machine_id"].sVal;
        std::string persStr = args["personality"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            IOHandler* keyReg = ms->machine->ioRegistry ? ms->machine->ioRegistry->findHandler("KEY") : nullptr;
            if (!keyReg) {
                textItem.oVal["text"] = Json("Error: Machine does not have KEY register (I/O personality not available)");
                textItem.oVal["isError"] = Json(true);
            } else {
                KeyRegister* kr = static_cast<KeyRegister*>(keyReg);
                const std::pair<uint8_t, uint8_t>* sequence = nullptr;
                if (persStr == "C64") {
                    static const std::pair<uint8_t, uint8_t> c64Seq = {0x00, 0x00};
                    sequence = &c64Seq;
                } else if (persStr == "C65") {
                    static const std::pair<uint8_t, uint8_t> c65Seq = {0xA5, 0x96};
                    sequence = &c65Seq;
                } else if (persStr == "MEGA65") {
                    static const std::pair<uint8_t, uint8_t> m65Seq = {0x47, 0x53};
                    sequence = &m65Seq;
                } else if (persStr == "ETHERNET") {
                    static const std::pair<uint8_t, uint8_t> ethernetSeq = {0x45, 0x54};
                    sequence = &ethernetSeq;
                } else {
                    textItem.oVal["text"] = Json("Error: Invalid personality (valid: C64, C65, MEGA65, ETHERNET)");
                    textItem.oVal["isError"] = Json(true);
                    sequence = nullptr;
                }

                if (sequence) {
                    kr->ioWrite(ms->bus, 0xD02F, sequence->first);
                    kr->ioWrite(ms->bus, 0xD02F, sequence->second);
                    textItem.oVal["text"] = Json("Personality switched to " + persStr);
                }
            }
        }
    } else if (name == "get_trace_buffer") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else if (!ms->dbg) {
            textItem.oVal["text"] = Json("Error: No debug context available");
            textItem.oVal["isError"] = Json(true);
        } else {
            TraceBuffer& buf = ms->dbg->trace();
            size_t limit = args.contains("limit") ? (size_t)args["limit"].nVal : buf.size();
            limit = std::min(limit, buf.size());

            // Helper: check if a trace entry is a call/return instruction
            auto isCallOrReturn = [](const std::string& mn) -> bool {
                if (mn.empty()) return false;
                // Check mnemonic prefix (e.g. "JSR $1234", "BSR $1234", "RTS", "RTI", "RTN #$02")
                if (mn.size() >= 3) {
                    std::string pfx = mn.substr(0, 3);
                    if (pfx == "JSR" || pfx == "BSR" || pfx == "RTS" || pfx == "RTI" || pfx == "RTN")
                        return true;
                }
                return false;
            };

            std::stringstream ss;
            ss << "Trace buffer: " << buf.size() << " entries (filter: " << ms->traceFilter << ")\n";

            // Collect entries matching filter, up to limit, from most recent
            std::vector<size_t> indices;
            for (size_t i = buf.size(); i > 0 && indices.size() < limit; --i) {
                size_t idx = i - 1;
                if (ms->traceFilter == "calls") {
                    if (!isCallOrReturn(buf.at(idx).mnemonic)) continue;
                }
                // Other filters (instructions, breakpoints, memory) pass all for now
                indices.push_back(idx);
            }
            std::reverse(indices.begin(), indices.end());

            ss << "Showing " << indices.size() << " entries:\n\n";

            for (size_t idx : indices) {
                const TraceEntry& e = buf.at(idx);
                ss << std::hex << std::setw(4) << std::setfill('0') << e.addr << ": " << e.mnemonic;
                if (!e.regs.empty()) {
                    ss << " | ";
                    bool first = true;
                    for (const auto& [regName, regVal] : e.regs) {
                        if (!first) ss << " ";
                        ss << regName << "=$" << std::hex << std::setw(2) << std::setfill('0') << regVal;
                        first = false;
                    }
                }
                ss << " | cycles=" << e.cycles << "\n";
            }
            textItem.oVal["text"] = Json(ss.str());
        }
    } else if (name == "clear_trace") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else if (!ms->dbg) {
            textItem.oVal["text"] = Json("Error: No debug context available");
            textItem.oVal["isError"] = Json(true);
        } else {
            ms->dbg->trace().clear();
            textItem.oVal["text"] = Json("Trace buffer cleared");
        }
    } else if (name == "set_trace_filter") {
        std::string mid = args["machine_id"].sVal;
        std::string filterStr = args["filter"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            if (filterStr == "all" || filterStr == "instructions" || filterStr == "breakpoints" || filterStr == "memory" || filterStr == "calls") {
                ms->traceFilter = filterStr;
                textItem.oVal["text"] = Json("Trace filter set to: " + filterStr);
            } else {
                textItem.oVal["text"] = Json("Error: Invalid filter (valid: all, instructions, breakpoints, memory, calls)");
                textItem.oVal["isError"] = Json(true);
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
                    // Use physical bus for loading so data goes to the literal
                    // address regardless of MAP state (#79).
                    IBus* loadBus = ms->machine->buses[0].bus;
                    if (loader->load(path, loadBus, ms->machine, addr)) {
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
    } else if (name == "list_instances") {
        std::stringstream ss;
        if (g_machines.empty()) {
            ss << "(no instances running)\n";
        } else {
            for (const auto& pair : g_machines) {
                ss << pair.first << "  [" << pair.second.machineType << "]  "
                   << pair.second.machine->displayName << "\n";
            }
        }
        textItem.oVal["text"] = Json(ss.str());
    } else if (name == "create_machine") {
        std::string machineType = args["machine_type"].sVal;

        // Resolve instance ID: use provided or auto-generate
        std::string instanceId;
        if (args.oVal.count("machine_id") && args["machine_id"].type == Json::STR && !args["machine_id"].sVal.empty()) {
            instanceId = args["machine_id"].sVal;
        } else {
            int n = ++g_typeCounters[machineType];
            instanceId = machineType + "_" + std::to_string(n);
        }

        // Destroy any existing instance with this ID
        g_machines.erase(instanceId);
        MachineState* ms = createMachineInstance(instanceId, machineType);

        if (!ms) {
            std::vector<std::string> validIds;
            MachineRegistry::instance().enumerate(validIds);
            std::string validList;
            for (size_t i = 0; i < validIds.size(); ++i) {
                if (i > 0) validList += ", ";
                validList += validIds[i];
            }
            textItem.oVal["text"] = Json("Error: Unknown machine type: " + machineType +
                ". Valid types: " + (validList.empty() ? "(none)" : validList));
            textItem.oVal["isError"] = Json(true);
        } else {
            textItem.oVal["text"] = Json("Created instance \"" + instanceId +
                "\" of machine: " + std::string(ms->machine->displayName));
        }
    } else if (name == "destroy_machine") {
        std::string mid = args["machine_id"].sVal;
        if (!g_machines.count(mid)) {
            textItem.oVal["text"] = Json("Error: No instance with ID: " + mid);
            textItem.oVal["isError"] = Json(true);
        } else {
            g_machines.erase(mid);
            g_assemblerOverrides.erase(mid);
            textItem.oVal["text"] = Json("Destroyed instance: " + mid);
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
            IAssembler* asmb = ms->assem ? ms->assem : ToolchainRegistry::instance().createAssembler(isa);
            std::vector<uint8_t> bytes;
            std::vector<std::string> errors;

            if (!asmb) {
                errors.push_back("No assembler registered for ISA: " + isa);
            } else {
                uint32_t curAddr = 0;
                bool useFileMode = false;

                // Try line-by-line assembly first; if the first line returns -1, switch to file mode
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
                        // Line-by-line not supported; switch to file-based assembly
                        useFileMode = true;
                        break;
                    } else {
                        for (int i = 0; i < n; i++) bytes.push_back(buf[i]);
                        curAddr += (uint32_t)n;
                    }
                }

                // If line-mode failed, try file-based assembly
                if (useFileMode) {
                    bytes.clear();
                    errors.clear();

                    // Write source to temporary file
                    const char* tmpSrcName = "/tmp/mmsim_asm_XXXXXX";
                    const char* tmpOutName = "/tmp/mmsim_out_XXXXXX";
                    char srcBuf[256], outBuf[256];
                    strncpy(srcBuf, tmpSrcName, sizeof(srcBuf) - 1);
                    strncpy(outBuf, tmpOutName, sizeof(outBuf) - 1);

                    int srcFd = mkstemp(srcBuf);
                    if (srcFd >= 0) {
                        ssize_t written = write(srcFd, source.c_str(), source.size());
                        close(srcFd);

                        int outFd = mkstemp(outBuf);
                        close(outFd);

                        if (written >= 0) {
                            AssemblerResult result = asmb->assemble(srcBuf, outBuf);
                            if (result.success) {
                                // Read .prg output, skipping 2-byte CBM load header if present
                                std::ifstream prg(outBuf, std::ios::binary);
                                if (prg) {
                                    uint8_t byte;
                                    int headerSkipped = 0;
                                    while (prg.read((char*)&byte, 1)) {
                                        if (headerSkipped < 2) {
                                            headerSkipped++;
                                        } else {
                                            bytes.push_back(byte);
                                        }
                                    }
                                    prg.close();
                                }
                            } else {
                                errors.push_back(result.errorMessage);
                            }
                        }
                        std::remove(srcBuf);
                        std::remove(outBuf);
                    }
                }

                if (!ms->assem) delete asmb;  // Only delete if not part of MachineState
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
    } else if (name == "set_assembler") {
        std::string mid = args["machine_id"].sVal;
        std::string asmName = args["assembler_name"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            g_assemblerOverrides[mid] = asmName;
            // Re-create the assembler with the new override
            if (ms->assem) { delete ms->assem; ms->assem = nullptr; }
            ms->assem = resolveAssembler(ms->cpu->isaName(), ms->machine->preferredAssembler, asmName);
            if (ms->assem) {
                textItem.oVal["text"] = Json("Assembler set to: " + asmName);
            } else {
                textItem.oVal["text"] = Json("Error: Assembler '" + asmName + "' not found");
                textItem.oVal["isError"] = Json(true);
            }
        }
    } else if (name == "get_assembler") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            std::string asmName;
            if (g_assemblerOverrides.count(mid)) {
                asmName = g_assemblerOverrides[mid];
            } else if (!ms->machine->preferredAssembler.empty()) {
                asmName = ms->machine->preferredAssembler;
            } else if (ms->assem) {
                asmName = ms->assem->name();
            } else {
                asmName = "none";
            }
            textItem.oVal["text"] = Json(asmName);
        }
    } else if (name == "run_cpu") {
        std::string mid = args["machine_id"].sVal;
        int maxSteps = args.contains("max_steps") ? (int)args["max_steps"].nVal : 10000000;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            bool ignPE = args.contains("ignore_program_end") && args["ignore_program_end"].bVal;
            bool stopBrk = args.contains("stop_on_brk") && args["stop_on_brk"].bVal;
            bool bpHit = false;
            std::string reason = runWithBudget(ms, maxSteps, ignPE, bpHit, stopBrk);
            std::stringstream ss;
            if (reason == "breakpoint")    ss << "Breakpoint hit. ";
            else if (reason == "brk")      ss << "BRK at $" << toHex(ms->cpu->pc()) << ". ";
            else if (reason == "max_steps") ss << "Stopped after " << maxSteps << " steps. ";
            else                            ss << "Program ended. ";
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
    } else if (name == "run_until") {
        std::string mid = args["machine_id"].sVal;
        std::string condition = args["condition"].sVal;
        int maxSteps = args.contains("max_steps") ? (int)args["max_steps"].nVal : 10000000;
        std::string reportSpec = args.contains("report") ? args["report"].sVal : "regs";
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            // Validate the condition expression first
            uint32_t testResult = 0;
            if (!ExpressionEvaluator::evaluate(condition, ms->dbg, testResult)) {
                textItem.oVal["text"] = Json("Error: Invalid condition expression: " + condition);
                textItem.oVal["isError"] = Json(true);
            } else {
                bool ignPE = args.contains("ignore_program_end") && args["ignore_program_end"].bVal;
                ms->dbg->resume();
                int steps = 0;
                bool conditionMet = false;
                std::string stopReason;

                bool loose = args.contains("loose") && args["loose"].bVal;
                while (steps < maxSteps) {
                    if (ms->machine && ms->machine->schedulerStep)
                        ms->machine->schedulerStep(*ms->machine);
                    else
                        ms->cpu->step();
                    ++steps;

                    if (ms->dbg->isPaused()) { stopReason = "Breakpoint hit"; break; }
                    if (!ignPE && ms->cpu->isProgramEnd(ms->bus)) { stopReason = "Program ended"; break; }

                    if (!loose || (steps & 0xFF) == 0 || steps < 16) {
                        uint32_t condVal = 0;
                        ExpressionEvaluator::evaluate(condition, ms->dbg, condVal);
                        if (condVal != 0) { conditionMet = true; stopReason = "Condition met: " + condition; break; }
                    }
                }
                if (stopReason.empty()) stopReason = "Stopped after " + std::to_string(maxSteps) + " steps";

                std::stringstream ss;
                ss << stopReason << " (steps=" << steps << ", cycles=" << ms->cpu->cycles() << ")\n";

                // Parse report spec and generate output
                std::istringstream rptStream(reportSpec);
                std::string item;
                while (std::getline(rptStream, item, ',')) {
                    // trim whitespace
                    while (!item.empty() && item[0] == ' ') item.erase(0, 1);
                    while (!item.empty() && item.back() == ' ') item.pop_back();

                    if (item == "regs") {
                        int regCount = ms->cpu->regCount();
                        for (int i = 0; i < regCount; ++i) {
                            const auto* rd = ms->cpu->regDescriptor(i);
                            if (rd->flags & REGFLAG_INTERNAL) continue;
                            uint32_t val = ms->cpu->regRead(i);
                            ss << rd->name << ": $" << toHex(val, rd->width == RegWidth::R16 ? 4 : 2) << "  ";
                        }
                        ss << "\n";
                    } else if (item == "disasm") {
                        uint32_t pc = ms->cpu->pc();
                        for (int i = 0; i < 10; ++i) {
                            char buf[64];
                            int bytes = ms->disasm->disasmOne(ms->bus, pc, buf, sizeof(buf));
                            ss << toHex(pc, 4) << ": " << buf << "\n";
                            pc += bytes ? bytes : 1;
                        }
                    } else if (item == "stack") {
                        ss << "SP=$" << toHex(ms->cpu->sp(), 4) << "\n";
                    } else if (item.substr(0, 4) == "mem:") {
                        // Parse mem:ADDR:SIZE
                        auto parts = item.substr(4);
                        auto colonPos = parts.find(':');
                        if (colonPos != std::string::npos) {
                            uint32_t addr = 0, size = 16;
                            ExpressionEvaluator::evaluate(parts.substr(0, colonPos), ms->dbg, addr);
                            ExpressionEvaluator::evaluate(parts.substr(colonPos + 1), ms->dbg, size);
                            if (size > 256) size = 256;
                            ss << toHex(addr, 4) << ": ";
                            for (uint32_t j = 0; j < size; ++j) {
                                ss << toHex(ms->bus->peek8(addr + j), 2) << " ";
                                if ((j & 15) == 15 && j + 1 < size) ss << "\n" << toHex(addr + j + 1, 4) << ": ";
                            }
                            ss << "\n";
                        }
                    }
                }
                textItem.oVal["text"] = Json(ss.str());
            }
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
                std::string errMsg;
                if (!resolveAddrWithDiagnostic(args["addr"], ms->dbg, addr, errMsg)) {
                    textItem.oVal["text"] = Json("Error: " + errMsg);
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
            std::string errMsg;
            if (!resolveAddrWithDiagnostic(args["addr"], ms->dbg, addr, errMsg)) {
                textItem.oVal["text"] = Json("Error: addr parameter - " + errMsg);
                textItem.oVal["isError"] = Json(true);
            } else if (!resolveAddrWithDiagnostic(args["value"], ms->dbg, value, errMsg)) {
                textItem.oVal["text"] = Json("Error: value parameter - " + errMsg);
                textItem.oVal["isError"] = Json(true);
            } else if (!resolveAddrWithDiagnostic(args["size"], ms->dbg, size, errMsg)) {
                textItem.oVal["text"] = Json("Error: size parameter - " + errMsg);
                textItem.oVal["isError"] = Json(true);
            } else {
                for (uint32_t i = 0; i < size; ++i) ms->bus->write8(addr + i, (uint8_t)value);
                textItem.oVal["text"] = Json("Filled " + std::to_string(size) + " bytes at $" + toHex(addr));
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
            std::string errMsg;
            if (!resolveAddrWithDiagnostic(args["src_addr"], ms->dbg, src, errMsg)) {
                textItem.oVal["text"] = Json("Error: src_addr parameter - " + errMsg);
                textItem.oVal["isError"] = Json(true);
            } else if (!resolveAddrWithDiagnostic(args["dst_addr"], ms->dbg, dst, errMsg)) {
                textItem.oVal["text"] = Json("Error: dst_addr parameter - " + errMsg);
                textItem.oVal["isError"] = Json(true);
            } else if (!resolveAddrWithDiagnostic(args["size"], ms->dbg, size, errMsg)) {
                textItem.oVal["text"] = Json("Error: size parameter - " + errMsg);
                textItem.oVal["isError"] = Json(true);
            } else {
                std::vector<uint8_t> buf(size);
                for (uint32_t i = 0; i < size; ++i) buf[i] = ms->bus->peek8(src + i);
                for (uint32_t i = 0; i < size; ++i) ms->bus->write8(dst + i, buf[i]);
                textItem.oVal["text"] = Json("Copied " + std::to_string(size) + " bytes from $" + toHex(src) + " to $" + toHex(dst));
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
                bool physical = args.contains("physical") && args["physical"].bVal;
                int id = ms->dbg->breakpoints().add(addr, BreakpointType::EXEC, physical);
                if (args.contains("condition") && !args["condition"].sVal.empty()) {
                    ms->dbg->breakpoints().setCondition(id, args["condition"].sVal);
                }
                std::string msg = (physical ? "Physical breakpoint " : "Breakpoint ")
                                + std::to_string(id) + " set at $" + toHex(addr);
                if (args.contains("condition") && !args["condition"].sVal.empty())
                    msg += " if " + args["condition"].sVal;
                textItem.oVal["text"] = Json(msg);
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
                bool physical = args.contains("physical") && args["physical"].bVal;
                BreakpointType btype = (type == "read") ? BreakpointType::READ_WATCH : BreakpointType::WRITE_WATCH;
                int id = ms->dbg->breakpoints().add(addr, btype, physical);
                if (args.contains("condition") && !args["condition"].sVal.empty())
                    ms->dbg->breakpoints().setCondition(id, args["condition"].sVal);
                std::string msg = (physical ? "Physical watchpoint " : "Watchpoint ")
                                + std::to_string(id) + " (" + type + ") at $" + toHex(addr);
                if (args.contains("condition") && !args["condition"].sVal.empty())
                    msg += " if " + args["condition"].sVal;
                textItem.oVal["text"] = Json(msg);
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
                ss << "ID  Type         Addr     En  Hits  Condition\n";
                for (const auto& bp : bps) {
                    std::string t = (bp.type == BreakpointType::EXEC) ? "exec" : (bp.type == BreakpointType::READ_WATCH ? "read" : "write");
                    if (bp.physical) t += "(phys)";
                    ss << std::left << std::setw(4) << bp.id << std::setw(13) << t
                       << "$" << toHex(bp.addr) << "  " << (bp.enabled ? "Y" : "N")
                       << "   " << std::setw(5) << bp.hitCount;
                    if (!bp.condition.empty()) ss << "  " << bp.condition;
                    ss << "\n";
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
    } else if (name == "snapshot_save") {
        std::string mid = args["machine_id"].sVal;
        std::string snapName = args["name"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else if (snapName.empty()) {
            textItem.oVal["text"] = Json("Error: Snapshot name cannot be empty");
            textItem.oVal["isError"] = Json(true);
        } else {
            MachineSnapshot snap;

            // Capture registers
            int regCount = ms->cpu->regCount();
            for (int i = 0; i < regCount; ++i) {
                const auto* desc = ms->cpu->regDescriptor(i);
                if (desc->flags & REGFLAG_INTERNAL) continue;
                snap.regs[desc->name] = ms->cpu->regRead(i);
            }

            // Determine memory range
            uint32_t memBase = 0;
            uint32_t memEnd = 0xFFFF; // default: full 16-bit
            if (args.contains("range") && !args["range"].sVal.empty()) {
                std::string range = args["range"].sVal;
                size_t dash = range.find('-');
                if (dash != std::string::npos) {
                    std::string startStr = range.substr(0, dash);
                    std::string endStr = range.substr(dash + 1);
                    std::string errMsg;
                    uint32_t s, e;
                    if (resolveAddrWithDiagnostic(Json(startStr), ms->dbg, s, errMsg) &&
                        resolveAddrWithDiagnostic(Json(endStr), ms->dbg, e, errMsg)) {
                        memBase = s;
                        memEnd = e;
                    }
                }
            }

            snap.memBase = memBase;
            snap.memSize = memEnd - memBase + 1;
            snap.memory.resize(snap.memSize);
            for (uint32_t i = 0; i < snap.memSize; ++i)
                snap.memory[i] = ms->bus->peek8(memBase + i);

            size_t nRegs = snap.regs.size();
            uint32_t nBytes = snap.memSize;
            g_snapshots[mid][snapName] = std::move(snap);

            std::stringstream ss;
            ss << "Snapshot \"" << snapName << "\" saved: "
               << nRegs << " registers, "
               << nBytes << " bytes ($"
               << toHex(memBase, 4) << "-$" << toHex(memEnd, 4) << ")";
            textItem.oVal["text"] = Json(ss.str());
        }

    } else if (name == "snapshot_diff") {
        std::string mid = args["machine_id"].sVal;
        std::string nameA = args["snapshot_a"].sVal;
        std::string nameB = args["snapshot_b"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else if (g_snapshots.find(mid) == g_snapshots.end() ||
                   g_snapshots[mid].find(nameA) == g_snapshots[mid].end()) {
            textItem.oVal["text"] = Json("Error: Snapshot \"" + nameA + "\" not found");
            textItem.oVal["isError"] = Json(true);
        } else if (g_snapshots[mid].find(nameB) == g_snapshots[mid].end()) {
            textItem.oVal["text"] = Json("Error: Snapshot \"" + nameB + "\" not found");
            textItem.oVal["isError"] = Json(true);
        } else {
            const auto& snapA = g_snapshots[mid][nameA];
            const auto& snapB = g_snapshots[mid][nameB];
            SymbolTable* symTab = ms->dbg ? &ms->dbg->symbols() : nullptr;

            std::stringstream ss;
            ss << "=== Snapshot Diff: \"" << nameA << "\" vs \"" << nameB << "\" ===\n\n";

            // Register diff
            ss << "--- Registers ---\n";
            int regChanges = 0;
            for (const auto& [regName, valA] : snapA.regs) {
                auto it = snapB.regs.find(regName);
                if (it != snapB.regs.end() && it->second != valA) {
                    // Determine display width from value magnitude
                    int width = (valA > 0xFF || it->second > 0xFF) ? 4 : 2;
                    ss << "  " << regName << ": $" << toHex(valA, width)
                       << " -> $" << toHex(it->second, width) << "\n";
                    ++regChanges;
                }
            }
            if (regChanges == 0) ss << "  (no register changes)\n";
            ss << "\n";

            // Memory diff — find overlapping range
            uint32_t overlapStart = std::max(snapA.memBase, snapB.memBase);
            uint32_t overlapEndA = snapA.memBase + snapA.memSize;
            uint32_t overlapEndB = snapB.memBase + snapB.memSize;
            uint32_t overlapEnd = std::min(overlapEndA, overlapEndB);

            ss << "--- Memory ---\n";
            if (overlapStart >= overlapEnd) {
                ss << "  (no overlapping memory range to compare)\n";
            } else {
                // Collect diff regions
                struct DiffRegion {
                    uint32_t addr;
                    uint32_t length;
                };
                std::vector<DiffRegion> regions;
                bool inDiff = false;

                for (uint32_t addr = overlapStart; addr < overlapEnd; ++addr) {
                    uint8_t a = snapA.memory[addr - snapA.memBase];
                    uint8_t b = snapB.memory[addr - snapB.memBase];
                    if (a != b) {
                        if (!inDiff) {
                            regions.push_back({addr, 1});
                            inDiff = true;
                        } else {
                            regions.back().length++;
                        }
                    } else {
                        inDiff = false;
                    }
                }

                uint32_t totalChanged = 0;
                for (const auto& r : regions) totalChanged += r.length;

                uint32_t rangeSize = overlapEnd - overlapStart;
                ss << "  Range: $" << toHex(overlapStart, 4) << "-$"
                   << toHex(overlapEnd - 1, 4) << " (" << rangeSize << " bytes)\n";
                ss << "  Changed: " << totalChanged << " byte"
                   << (totalChanged != 1 ? "s" : "") << " in "
                   << regions.size() << " region(s)\n\n";

                // Show regions (limit to 50)
                int shown = 0;
                for (const auto& r : regions) {
                    if (shown >= 50) {
                        ss << "  ... (" << (regions.size() - 50) << " more regions omitted)\n";
                        break;
                    }

                    ss << "  $" << toHex(r.addr, 4) << "-$"
                       << toHex(r.addr + r.length - 1, 4)
                       << " (" << r.length << " byte" << (r.length > 1 ? "s" : "") << ")";

                    if (symTab) {
                        uint32_t symOff = 0;
                        std::string label = symTab->nearest(r.addr, symOff);
                        if (!label.empty()) {
                            ss << "  ; " << label;
                            if (symOff > 0) ss << "+" << symOff;
                        }
                    }
                    ss << "\n";

                    // Show bytes (limit to 16 per region)
                    uint32_t showLen = std::min(r.length, (uint32_t)16);
                    ss << "    A: ";
                    for (uint32_t i = 0; i < showLen; ++i)
                        ss << toHex(snapA.memory[r.addr + i - snapA.memBase], 2) << " ";
                    if (r.length > 16) ss << "...";
                    ss << "\n    B: ";
                    for (uint32_t i = 0; i < showLen; ++i)
                        ss << toHex(snapB.memory[r.addr + i - snapB.memBase], 2) << " ";
                    if (r.length > 16) ss << "...";
                    ss << "\n\n";
                    ++shown;
                }

                if (regions.empty())
                    ss << "  (no memory changes)\n";
            }

            textItem.oVal["text"] = Json(ss.str());
        }

    } else if (name == "snapshot_list") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            auto it = g_snapshots.find(mid);
            if (it == g_snapshots.end() || it->second.empty()) {
                textItem.oVal["text"] = Json("No snapshots saved.");
            } else {
                std::stringstream ss;
                for (const auto& [snapName, snap] : it->second) {
                    ss << "\"" << snapName << "\": "
                       << snap.regs.size() << " regs, "
                       << snap.memSize << " bytes ($"
                       << toHex(snap.memBase, 4) << "-$"
                       << toHex(snap.memBase + snap.memSize - 1, 4) << ")\n";
                }
                textItem.oVal["text"] = Json(ss.str());
            }
        }

    } else if (name == "snapshot_delete") {
        std::string mid = args["machine_id"].sVal;
        std::string snapName = args["name"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else if (snapName == "*") {
            int count = 0;
            auto it = g_snapshots.find(mid);
            if (it != g_snapshots.end()) {
                count = it->second.size();
                it->second.clear();
            }
            textItem.oVal["text"] = Json("Deleted " + std::to_string(count) + " snapshot(s).");
        } else {
            auto it = g_snapshots.find(mid);
            if (it != g_snapshots.end() && it->second.erase(snapName)) {
                textItem.oVal["text"] = Json("Deleted snapshot \"" + snapName + "\".");
            } else {
                textItem.oVal["text"] = Json("Error: Snapshot \"" + snapName + "\" not found");
                textItem.oVal["isError"] = Json(true);
            }
        }

    } else if (name == "analyze_routine") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else if (!ms->disasm) {
            textItem.oVal["text"] = Json("Error: No disassembler available for this machine");
            textItem.oVal["isError"] = Json(true);
        } else {
            uint32_t entryAddr;
            std::string errMsg;
            if (!resolveAddrWithDiagnostic(args["addr"], ms->dbg, entryAddr, errMsg)) {
                textItem.oVal["text"] = Json("Error: addr - " + errMsg);
                textItem.oVal["isError"] = Json(true);
            } else {
                int maxInsns = args.contains("max_instructions") ? (int)args["max_instructions"].nVal : 200;
                if (maxInsns <= 0) maxInsns = 200;
                if (maxInsns > 5000) maxInsns = 5000;
                bool recursive = args.contains("recursive") && args["recursive"].bVal;

                SymbolTable* symTab = ms->dbg ? &ms->dbg->symbols() : nullptr;
                uint32_t addrMask = ms->bus->config().addrMask;

                // Control-flow walk with depth tracking
                std::set<uint32_t> visited;
                struct QueueItem { uint32_t pc; int depth; };
                std::vector<QueueItem> queue;
                queue.push_back({entryAddr, 0});

                struct CallInfo { uint32_t target; uint32_t from; };
                struct LoopInfo { uint32_t branchAddr; uint32_t target; };
                struct IoAccess { uint32_t addr; uint32_t ioAddr; bool isStore; std::string mnemonic; };

                std::vector<CallInfo> calls;
                std::vector<LoopInfo> loops;
                std::vector<IoAccess> ioAccesses;
                std::vector<uint32_t> exits;       // RTS/RTI addresses
                std::vector<uint32_t> indirectJmps; // JMP ($xxxx) addresses
                int totalInsns = 0;
                uint32_t minAddr = entryAddr, maxAddr = entryAddr;
                int branchCount = 0, jmpCount = 0;
                int maxDepth = 0;    // deepest call nesting seen
                int curDepth = 0;    // current call depth during walk

                while (!queue.empty() && totalInsns < maxInsns) {
                    auto item = queue.back();
                    queue.pop_back();
                    uint32_t pc = item.pc;
                    curDepth = item.depth;

                    while (totalInsns < maxInsns) {
                        if (visited.count(pc)) break;
                        visited.insert(pc);

                        DisasmEntry entry;
                        int bytes = ms->disasm->disasmEntry(ms->bus, pc, entry);
                        if (bytes <= 0) break;

                        ++totalInsns;
                        if (pc < minAddr) minAddr = pc;
                        if (pc + bytes - 1 > maxAddr) maxAddr = pc + bytes - 1;

                        uint8_t opcode = ms->bus->peek8(pc);

                        // Detect I/O accesses (reads/writes to $D000-$DFFF on 16-bit bus)
                        if (bytes >= 3) {
                            uint16_t operandAddr = ms->bus->peek8(pc+1) | (ms->bus->peek8(pc+2) << 8);
                            if (operandAddr >= 0xD000 && operandAddr <= 0xDFFF) {
                                bool isStore = (entry.mnemonic.find("ST") == 0);
                                ioAccesses.push_back({pc, operandAddr, isStore, entry.complete});
                            }
                        }

                        if (entry.isReturn) {
                            exits.push_back(pc);
                            break;
                        } else if (entry.isCall) {
                            uint32_t target = entry.targetAddr & addrMask;
                            calls.push_back({target, pc});
                            if (recursive && !visited.count(target)) {
                                int childDepth = curDepth + 1;
                                if (childDepth > maxDepth) maxDepth = childDepth;
                                queue.push_back({target, childDepth});
                            }
                            pc = (pc + bytes) & addrMask;
                        } else if (entry.isBranch) {
                            ++branchCount;
                            uint32_t target = entry.targetAddr & addrMask;
                            uint32_t fallthrough = (pc + bytes) & addrMask;

                            // Backward branch = loop
                            if (target <= pc) {
                                loops.push_back({pc, target});
                            }

                            if (!visited.count(target)) queue.push_back({target, curDepth});
                            pc = fallthrough;
                        } else if (opcode == 0x4C) {
                            // JMP absolute
                            ++jmpCount;
                            pc = entry.targetAddr & addrMask;
                        } else if (opcode == 0x6C) {
                            // JMP indirect — can't follow statically
                            indirectJmps.push_back(pc);
                            break;
                        } else if (opcode == 0x00) {
                            // BRK — treat as exit
                            exits.push_back(pc);
                            break;
                        } else {
                            pc = (pc + bytes) & addrMask;
                        }
                    }
                }

                // Build report
                std::stringstream ss;
                std::string entryLabel;
                if (symTab) entryLabel = symTab->getLabel(entryAddr);

                ss << "=== Routine Analysis: $" << toHex(entryAddr, 4);
                if (!entryLabel.empty()) ss << " (" << entryLabel << ")";
                if (recursive) ss << " [recursive]";
                ss << " ===\n";
                ss << "Size: " << (maxAddr - minAddr + 1) << " bytes ($"
                   << toHex(minAddr, 4) << "-$" << toHex(maxAddr, 4) << "), "
                   << totalInsns << " instructions\n";
                ss << "Max call depth: " << maxDepth << "\n";
                if (totalInsns >= maxInsns)
                    ss << "NOTE: Analysis truncated at " << maxInsns << " instructions\n";
                ss << "\n";

                // Calls
                if (!calls.empty()) {
                    ss << "--- Calls (" << calls.size() << ") ---\n";
                    // Deduplicate by target
                    std::map<uint32_t, int> callCounts;
                    for (const auto& c : calls) callCounts[c.target]++;
                    for (const auto& [target, count] : callCounts) {
                        ss << "  JSR $" << toHex(target, 4);
                        if (symTab) {
                            std::string lbl = symTab->getLabel(target);
                            if (!lbl.empty()) ss << " (" << lbl << ")";
                        }
                        if (count > 1) ss << " x" << count;
                        ss << "\n";
                    }
                    ss << "\n";
                }

                // Loops
                if (!loops.empty()) {
                    ss << "--- Loops (" << loops.size() << ") ---\n";
                    for (const auto& l : loops) {
                        ss << "  $" << toHex(l.target, 4) << "-$" << toHex(l.branchAddr, 4)
                           << " (branch at $" << toHex(l.branchAddr, 4) << " -> $"
                           << toHex(l.target, 4) << ")";
                        if (symTab) {
                            std::string lbl = symTab->getLabel(l.target);
                            if (!lbl.empty()) ss << "  ; " << lbl;
                        }
                        ss << "\n";
                    }
                    ss << "\n";
                }

                // I/O accesses
                if (!ioAccesses.empty()) {
                    ss << "--- I/O Accesses (" << ioAccesses.size() << ") ---\n";
                    // Deduplicate by I/O address
                    std::map<uint32_t, std::pair<bool, bool>> ioMap; // addr → {read, write}
                    for (const auto& io : ioAccesses) {
                        auto& rw = ioMap[io.ioAddr];
                        if (io.isStore) rw.second = true;
                        else rw.first = true;
                    }
                    for (const auto& [ioAddr, rw] : ioMap) {
                        ss << "  $" << toHex(ioAddr, 4);
                        if (symTab) {
                            std::string lbl = symTab->getLabel(ioAddr);
                            if (!lbl.empty()) ss << " (" << lbl << ")";
                        }
                        ss << ": ";
                        if (rw.first && rw.second) ss << "read+write";
                        else if (rw.first) ss << "read";
                        else ss << "write";
                        ss << "\n";
                    }
                    ss << "\n";
                }

                // Control flow summary
                ss << "--- Control Flow ---\n";
                ss << "  Branches: " << branchCount
                   << ", Jumps: " << jmpCount
                   << ", Calls: " << calls.size() << "\n";
                if (!exits.empty()) {
                    ss << "  Exits: ";
                    for (size_t i = 0; i < exits.size(); ++i) {
                        if (i > 0) ss << ", ";
                        uint8_t op = ms->bus->peek8(exits[i]);
                        ss << (op == 0x60 ? "RTS" : (op == 0x40 ? "RTI" : "BRK"))
                           << " at $" << toHex(exits[i], 4);
                    }
                    ss << "\n";
                }
                if (!indirectJmps.empty()) {
                    ss << "  Indirect jumps: ";
                    for (size_t i = 0; i < indirectJmps.size(); ++i) {
                        if (i > 0) ss << ", ";
                        ss << "$" << toHex(indirectJmps[i], 4);
                    }
                    ss << " (not followed)\n";
                }

                textItem.oVal["text"] = Json(ss.str());
            }
        }

    } else if (name == "generate_tests") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            uint32_t entryAddr;
            std::string errMsg;
            if (!resolveAddrWithDiagnostic(args["addr"], ms->dbg, entryAddr, errMsg)) {
                textItem.oVal["text"] = Json("Error: addr - " + errMsg);
                textItem.oVal["isError"] = Json(true);
            } else {
                // Parse input/output register names
                std::vector<std::string> inputRegs = {"A"};
                std::vector<std::string> outputRegs = {"A", "P"};
                if (args.contains("input_regs") && args["input_regs"].type == Json::ARR) {
                    inputRegs.clear();
                    for (const auto& r : args["input_regs"].aVal)
                        inputRegs.push_back(r.sVal);
                }
                if (args.contains("output_regs") && args["output_regs"].type == Json::ARR) {
                    outputRegs.clear();
                    for (const auto& r : args["output_regs"].aVal)
                        outputRegs.push_back(r.sVal);
                }

                // Parse test values
                std::vector<uint8_t> values = {0, 1, 0x7F, 0x80, 0xFE, 0xFF};
                if (args.contains("values") && args["values"].type == Json::ARR) {
                    values.clear();
                    for (const auto& v : args["values"].aVal)
                        values.push_back((uint8_t)v.nVal);
                }

                int maxSteps = args.contains("max_steps") ? (int)args["max_steps"].nVal : 10000;
                if (maxSteps <= 0) maxSteps = 10000;

                // Validate register names
                bool regsOk = true;
                auto findRegIndex = [&](const std::string& regName) -> int {
                    int rc = ms->cpu->regCount();
                    for (int i = 0; i < rc; ++i) {
                        const auto* d = ms->cpu->regDescriptor(i);
                        if (regName == d->name) return i;
                    }
                    return -1;
                };
                for (const auto& r : inputRegs) {
                    if (findRegIndex(r) < 0) {
                        textItem.oVal["text"] = Json("Error: Unknown input register: " + r);
                        textItem.oVal["isError"] = Json(true);
                        regsOk = false;
                        break;
                    }
                }
                if (regsOk) {
                    for (const auto& r : outputRegs) {
                        if (findRegIndex(r) < 0) {
                            textItem.oVal["text"] = Json("Error: Unknown output register: " + r);
                            textItem.oVal["isError"] = Json(true);
                            regsOk = false;
                            break;
                        }
                    }
                }

                if (regsOk) {
                    // Compute total combinations
                    size_t totalTests = 1;
                    for (size_t i = 0; i < inputRegs.size(); ++i) {
                        totalTests *= values.size();
                        if (totalTests > 256) { totalTests = 256; break; }
                    }

                    // Save full machine state: all registers + memory snapshot
                    // (We save/restore registers and the memory region the routine might touch)
                    std::map<int, uint32_t> savedRegs;
                    int regCount = ms->cpu->regCount();
                    for (int i = 0; i < regCount; ++i)
                        savedRegs[i] = ms->cpu->regRead(i);

                    // Save stack page ($0100-$01FF) which the routine will use
                    std::vector<uint8_t> savedStack(256);
                    for (int i = 0; i < 256; ++i)
                        savedStack[i] = ms->bus->peek8(0x0100 + i);

                    SymbolTable* symTab = ms->dbg ? &ms->dbg->symbols() : nullptr;

                    // Run tests
                    struct TestVector {
                        std::vector<uint8_t> inputs;
                        std::vector<uint32_t> outputs;
                        int steps;
                        bool completed; // routine returned normally
                    };
                    std::vector<TestVector> results;

                    // Iterate over all input combinations
                    size_t numVals = values.size();
                    for (size_t combo = 0; combo < totalTests; ++combo) {
                        // Restore registers
                        for (auto& [idx, val] : savedRegs)
                            ms->cpu->regWrite(idx, val);
                        // Restore stack
                        for (int i = 0; i < 256; ++i)
                            ms->bus->write8(0x0100 + i, savedStack[i]);

                        // Decode combination index into per-register values
                        TestVector tv;
                        tv.inputs.resize(inputRegs.size());
                        size_t ci = combo;
                        for (size_t r = 0; r < inputRegs.size(); ++r) {
                            tv.inputs[r] = values[ci % numVals];
                            ci /= numVals;
                        }

                        // Set input registers
                        for (size_t r = 0; r < inputRegs.size(); ++r)
                            ms->cpu->regWriteByName(inputRegs[r].c_str(), tv.inputs[r]);

                        // Set PC
                        ms->cpu->setPc(entryAddr);

                        // Run until routine returns (RTS/RTI at call depth 0) or max steps
                        int callDepth = 0;
                        tv.completed = false;
                        tv.steps = 0;
                        for (int s = 0; s < maxSteps; ++s) {
                            uint8_t op = ms->bus->peek8(ms->cpu->pc());
                            // Check for routine completion BEFORE stepping
                            if (op == 0x60 && callDepth == 0) { tv.completed = true; break; } // RTS
                            if (op == 0x40 && callDepth == 0) { tv.completed = true; break; } // RTI
                            if (op == 0x00) { tv.completed = true; break; } // BRK

                            if (op == 0x20) callDepth++; // JSR
                            ms->cpu->step();
                            if (op == 0x60) callDepth--; // RTS from nested
                            ++tv.steps;
                        }

                        // Capture output registers
                        tv.outputs.resize(outputRegs.size());
                        for (size_t r = 0; r < outputRegs.size(); ++r)
                            tv.outputs[r] = ms->cpu->regReadByName(outputRegs[r].c_str());

                        results.push_back(tv);
                    }

                    // Restore machine state
                    for (auto& [idx, val] : savedRegs)
                        ms->cpu->regWrite(idx, val);
                    for (int i = 0; i < 256; ++i)
                        ms->bus->write8(0x0100 + i, savedStack[i]);

                    // Build report
                    std::stringstream ss;
                    std::string entryLabel;
                    if (symTab) entryLabel = symTab->getLabel(entryAddr);

                    ss << "=== Test Vectors: $" << toHex(entryAddr, 4);
                    if (!entryLabel.empty()) ss << " (" << entryLabel << ")";
                    ss << " ===\n";
                    ss << results.size() << " tests, inputs: [";
                    for (size_t i = 0; i < inputRegs.size(); ++i) {
                        if (i > 0) ss << ", ";
                        ss << inputRegs[i];
                    }
                    ss << "], outputs: [";
                    for (size_t i = 0; i < outputRegs.size(); ++i) {
                        if (i > 0) ss << ", ";
                        ss << outputRegs[i];
                    }
                    ss << "]\n\n";

                    // Table header
                    ss << std::setw(4) << "#" << " |";
                    for (const auto& r : inputRegs)
                        ss << std::setw(6) << (r + "(in)") << " |";
                    for (const auto& r : outputRegs)
                        ss << std::setw(7) << (r + "(out)") << " |";
                    ss << " steps | ok\n";

                    // Separator
                    int colWidth = 5 + (int)inputRegs.size() * 8 + (int)outputRegs.size() * 9 + 14;
                    ss << std::string(colWidth, '-') << "\n";

                    // Rows
                    for (size_t i = 0; i < results.size(); ++i) {
                        const auto& tv = results[i];
                        ss << std::setw(4) << (i + 1) << " |";
                        for (size_t r = 0; r < tv.inputs.size(); ++r)
                            ss << "    $" << toHex(tv.inputs[r], 2) << " |";
                        for (size_t r = 0; r < tv.outputs.size(); ++r)
                            ss << "     $" << toHex(tv.outputs[r], 2) << " |";
                        ss << std::setw(6) << tv.steps << " | "
                           << (tv.completed ? "Y" : "N") << "\n";
                    }

                    textItem.oVal["text"] = Json(ss.str());
                }
            }
        }

    } else if (name == "load_sid") {
        std::string mid = args["machine_id"].sVal;
        std::string filePath = args["file"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            // Read the SID file
            std::ifstream sidFile(filePath, std::ios::binary);
            if (!sidFile) {
                textItem.oVal["text"] = Json("Error: Cannot open file: " + filePath);
                textItem.oVal["isError"] = Json(true);
            } else {
                sidFile.seekg(0, std::ios::end);
                size_t fileSize = sidFile.tellg();
                sidFile.seekg(0, std::ios::beg);

                if (fileSize < 0x7C) {
                    textItem.oVal["text"] = Json("Error: File too small for PSID header");
                    textItem.oVal["isError"] = Json(true);
                } else {
                    std::vector<uint8_t> raw(fileSize);
                    sidFile.read(reinterpret_cast<char*>(raw.data()), fileSize);

                    // Parse PSID/RSID header (big-endian)
                    auto rd16be = [&](size_t off) -> uint16_t {
                        return (raw[off] << 8) | raw[off + 1];
                    };

                    std::string magic(reinterpret_cast<char*>(raw.data()), 4);
                    if (magic != "PSID" && magic != "RSID") {
                        textItem.oVal["text"] = Json("Error: Not a PSID/RSID file (magic: " + magic + ")");
                        textItem.oVal["isError"] = Json(true);
                    } else {
                        uint16_t version    = rd16be(0x04);
                        uint16_t dataOffset = rd16be(0x06);
                        uint16_t loadAddr   = rd16be(0x08);
                        uint16_t initAddr   = rd16be(0x0A);
                        uint16_t playAddr   = rd16be(0x0C);
                        uint16_t songs      = rd16be(0x0E);
                        uint16_t startSong  = rd16be(0x10);

                        // Extract strings (null-terminated within 32 bytes)
                        auto extractStr = [&](size_t off) -> std::string {
                            char buf[33];
                            std::memcpy(buf, &raw[off], 32);
                            buf[32] = '\0';
                            return std::string(buf);
                        };
                        std::string title    = extractStr(0x16);
                        std::string author   = extractStr(0x36);
                        std::string released = extractStr(0x56);

                        // If loadAddress is 0, first 2 bytes of data are the load address (LE)
                        const uint8_t* data = raw.data() + dataOffset;
                        size_t dataLen = fileSize - dataOffset;

                        if (loadAddr == 0 && dataLen >= 2) {
                            loadAddr = data[0] | (data[1] << 8);
                            data += 2;
                            dataLen -= 2;
                        }

                        // If initAddress is 0, use loadAddress
                        if (initAddr == 0) initAddr = loadAddr;

                        // Select subtune
                        int subtune = args.contains("subtune") ? (int)args["subtune"].nVal : startSong;
                        if (subtune < 1) subtune = 1;
                        if (subtune > songs) subtune = songs;

                        // Load data into machine memory
                        for (size_t i = 0; i < dataLen; ++i) {
                            ms->bus->write8(loadAddr + i, data[i]);
                        }

                        // Set up CPU to call init routine:
                        // A = subtune - 1 (0-based), then JSR initAddr
                        // Write a small trampoline at $0002:
                        //   $0002: LDA #subtune   (A9 xx)
                        //   $0004: JSR initAddr   (20 lo hi)
                        //   $0007: BRK            (00)
                        uint8_t saved[6];
                        for (int i = 0; i < 6; ++i)
                            saved[i] = ms->bus->peek8(0x0002 + i);

                        ms->bus->write8(0x0002, 0xA9);
                        ms->bus->write8(0x0003, (uint8_t)(subtune - 1));
                        ms->bus->write8(0x0004, 0x20);
                        ms->bus->write8(0x0005, initAddr & 0xFF);
                        ms->bus->write8(0x0006, (initAddr >> 8) & 0xFF);
                        ms->bus->write8(0x0007, 0x00); // BRK

                        // Run the init routine
                        ms->cpu->setPc(0x0002);
                        int steps = 0;
                        while (steps < 1000000) {
                            if (ms->cpu->pc() == 0x0007) break; // Hit BRK after init
                            if (ms->bus->peek8(ms->cpu->pc()) == 0x00 && ms->cpu->pc() != 0x0002) break;
                            ms->cpu->step();
                            ++steps;
                        }

                        // Restore trampoline bytes
                        for (int i = 0; i < 6; ++i)
                            ms->bus->write8(0x0002 + i, saved[i]);

                        // If play address is non-zero, set up for playback:
                        // Install a simple play loop at $0002:
                        //   $0002: JSR playAddr  (20 lo hi)
                        //   $0005: JMP $0002     (4C 02 00)
                        if (playAddr != 0) {
                            ms->bus->write8(0x0002, 0x20);
                            ms->bus->write8(0x0003, playAddr & 0xFF);
                            ms->bus->write8(0x0004, (playAddr >> 8) & 0xFF);
                            ms->bus->write8(0x0005, 0x4C);
                            ms->bus->write8(0x0006, 0x02);
                            ms->bus->write8(0x0007, 0x00);
                            ms->cpu->setPc(0x0002);
                        }

                        std::stringstream ss;
                        ss << "Loaded " << magic << " v" << version << ": " << title << "\n"
                           << "Author: " << author << "\n"
                           << "Released: " << released << "\n"
                           << "Songs: " << songs << ", playing subtune " << subtune << "\n"
                           << "Load: $" << toHex(loadAddr, 4)
                           << ", Init: $" << toHex(initAddr, 4)
                           << ", Play: $" << toHex(playAddr, 4) << "\n"
                           << "Data: " << dataLen << " bytes loaded at $"
                           << toHex(loadAddr, 4) << "-$"
                           << toHex((uint16_t)(loadAddr + dataLen - 1), 4) << "\n";
                        if (playAddr != 0) {
                            ss << "Play loop installed at $0002. Use record_audio to capture.";
                        } else {
                            ss << "No play address (CIA timer-driven). "
                               << "Use run_cpu + record_audio to capture.";
                        }
                        textItem.oVal["text"] = Json(ss.str());
                    }
                }
            }
        }

    } else if (name == "record_audio") {
        std::string mid = args["machine_id"].sVal;
        std::string filePath = args["file"].sVal;
        int durationMs = args.contains("duration_ms") ? (int)args["duration_ms"].nVal : 1000;
        if (durationMs <= 0) durationMs = 1000;
        if (durationMs > 60000) durationMs = 60000;

        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            // Find the IAudioOutput device
            IAudioOutput* audioDev = nullptr;
            bool isStereo = false;
            if (ms->machine->ioRegistry) {
                std::vector<IOHandler*> handlers;
                ms->machine->ioRegistry->enumerate(handlers);
                for (auto* h : handlers) {
                    auto* ao = dynamic_cast<IAudioOutput*>(h);
                    if (ao) {
                        audioDev = ao;
                        // Check if stereo by device name heuristic
                        std::string devName = h->name();
                        if (devName.find("SidPair") != std::string::npos ||
                            devName.find("Pair") != std::string::npos ||
                            devName.find("Stereo") != std::string::npos) {
                            isStereo = true;
                        }
                        break;
                    }
                }
            }

            if (!audioDev) {
                textItem.oVal["text"] = Json("Error: No audio output device found on this machine");
                textItem.oVal["isError"] = Json(true);
            } else {
                int sampleRate = audioDev->nativeSampleRate();
                if (sampleRate <= 0) sampleRate = 44100;
                int numChannels = isStereo ? 2 : 1;
                int totalSamples = (int)((int64_t)sampleRate * durationMs / 1000) * numChannels;

                // Accumulate recorded samples
                std::vector<float> recorded;
                recorded.reserve(totalSamples);

                // Calculate CPU cycles to run
                // Approximate: 1 MHz CPU = 1000 cycles/ms
                uint32_t clockHz = 985248; // PAL C64 default
                int64_t totalCycles = (int64_t)clockHz * durationMs / 1000;

                // Run CPU in batches, pulling audio periodically
                float pullBuf[4096];
                int64_t cyclesRun = 0;
                int batchSize = sampleRate / 20; // Pull ~50 times per second
                if (batchSize < 256) batchSize = 256;
                int cyclesPerBatch = (int)((int64_t)clockHz * batchSize / sampleRate / numChannels);
                if (cyclesPerBatch < 100) cyclesPerBatch = 100;

                while (cyclesRun < totalCycles && (int)recorded.size() < totalSamples) {
                    // Run CPU for a batch
                    for (int c = 0; c < cyclesPerBatch; ++c) {
                        if (ms->machine && ms->machine->schedulerStep) {
                            ms->machine->schedulerStep(*ms->machine);
                        } else {
                            ms->cpu->step();
                        }
                        ++cyclesRun;
                    }

                    // Pull available audio samples
                    int pulled = audioDev->pullSamples(pullBuf, 4096);
                    for (int i = 0; i < pulled && (int)recorded.size() < totalSamples; ++i) {
                        recorded.push_back(pullBuf[i]);
                    }
                }

                // Final drain
                while ((int)recorded.size() < totalSamples) {
                    int pulled = audioDev->pullSamples(pullBuf, 4096);
                    if (pulled <= 0) break;
                    for (int i = 0; i < pulled && (int)recorded.size() < totalSamples; ++i) {
                        recorded.push_back(pullBuf[i]);
                    }
                }

                // Convert float32 to 16-bit PCM
                std::vector<int16_t> pcm(recorded.size());
                for (size_t i = 0; i < recorded.size(); ++i) {
                    float s = recorded[i];
                    if (s > 1.0f) s = 1.0f;
                    if (s < -1.0f) s = -1.0f;
                    pcm[i] = (int16_t)(s * 32767.0f);
                }

                // Write WAV file
                std::ofstream wav(filePath, std::ios::binary);
                if (!wav) {
                    textItem.oVal["text"] = Json("Error: Cannot open file for writing: " + filePath);
                    textItem.oVal["isError"] = Json(true);
                } else {
                    uint32_t dataSize = pcm.size() * 2; // 16-bit = 2 bytes per sample
                    uint32_t fileSize = 36 + dataSize;
                    uint16_t bitsPerSample = 16;
                    uint16_t blockAlign = numChannels * (bitsPerSample / 8);
                    uint32_t byteRate = sampleRate * blockAlign;

                    // RIFF header
                    wav.write("RIFF", 4);
                    wav.write(reinterpret_cast<char*>(&fileSize), 4);
                    wav.write("WAVE", 4);

                    // fmt chunk
                    wav.write("fmt ", 4);
                    uint32_t fmtSize = 16;
                    wav.write(reinterpret_cast<char*>(&fmtSize), 4);
                    uint16_t audioFormat = 1; // PCM
                    uint16_t nc = numChannels;
                    uint32_t sr = sampleRate;
                    wav.write(reinterpret_cast<char*>(&audioFormat), 2);
                    wav.write(reinterpret_cast<char*>(&nc), 2);
                    wav.write(reinterpret_cast<char*>(&sr), 4);
                    wav.write(reinterpret_cast<char*>(&byteRate), 4);
                    wav.write(reinterpret_cast<char*>(&blockAlign), 2);
                    wav.write(reinterpret_cast<char*>(&bitsPerSample), 2);

                    // data chunk
                    wav.write("data", 4);
                    wav.write(reinterpret_cast<char*>(&dataSize), 4);
                    wav.write(reinterpret_cast<const char*>(pcm.data()), dataSize);

                    float durationActual = (float)recorded.size() / numChannels / sampleRate;
                    std::stringstream ss;
                    ss << "Recorded " << std::fixed << std::setprecision(2) << durationActual
                       << "s of audio to " << filePath << " ("
                       << (isStereo ? "stereo" : "mono") << ", "
                       << sampleRate << " Hz, 16-bit PCM, "
                       << dataSize << " bytes)";
                    textItem.oVal["text"] = Json(ss.str());
                }
            }
        }

    } else if (name == "diff_file") {
        std::string fileA = args["file_a"].sVal;
        std::string fileB = args["file_b"].sVal;
        uint32_t baseAddr = 0;
        int context = 0;

        // Optional base address
        if (args.contains("base_addr") && !args["base_addr"].sVal.empty()) {
            try {
                std::string ba = args["base_addr"].sVal;
                if (ba.size() > 1 && ba[0] == '$')
                    baseAddr = std::stoul(ba.substr(1), nullptr, 16);
                else
                    baseAddr = std::stoul(ba, nullptr, 0);
            } catch (...) {}
        }

        // Optional context bytes
        if (args.contains("context"))
            context = (int)args["context"].nVal;

        // Optional symbol table from machine
        SymbolTable* symTab = nullptr;
        if (args.contains("machine_id") && !args["machine_id"].sVal.empty()) {
            MachineState* ms = getMachine(args["machine_id"].sVal);
            if (ms && ms->dbg) symTab = &ms->dbg->symbols();
        }

        // Read both files
        auto readFile = [](const std::string& path, std::vector<uint8_t>& out) -> bool {
            std::ifstream f(path, std::ios::binary);
            if (!f) return false;
            f.seekg(0, std::ios::end);
            size_t sz = f.tellg();
            f.seekg(0, std::ios::beg);
            out.resize(sz);
            f.read(reinterpret_cast<char*>(out.data()), sz);
            return true;
        };

        std::vector<uint8_t> dataA, dataB;
        if (!readFile(fileA, dataA)) {
            textItem.oVal["text"] = Json("Error: Cannot read file_a: " + fileA);
            textItem.oVal["isError"] = Json(true);
        } else if (!readFile(fileB, dataB)) {
            textItem.oVal["text"] = Json("Error: Cannot read file_b: " + fileB);
            textItem.oVal["isError"] = Json(true);
        } else {
            size_t maxLen = std::max(dataA.size(), dataB.size());
            size_t minLen = std::min(dataA.size(), dataB.size());

            // Collect diff regions (runs of consecutive changed bytes)
            struct DiffRegion {
                uint32_t offset;
                uint32_t length;
            };
            std::vector<DiffRegion> regions;
            bool inDiff = false;

            for (size_t i = 0; i < maxLen; ++i) {
                uint8_t a = (i < dataA.size()) ? dataA[i] : 0;
                uint8_t b = (i < dataB.size()) ? dataB[i] : 0;
                bool differs = (a != b);

                if (differs && !inDiff) {
                    regions.push_back({(uint32_t)i, 1});
                    inDiff = true;
                } else if (differs && inDiff) {
                    regions.back().length++;
                } else {
                    inDiff = false;
                }
            }

            // Count total changed bytes
            uint32_t totalChanged = 0;
            for (const auto& r : regions) totalChanged += r.length;

            std::stringstream ss;

            // Summary
            ss << "=== File Diff Report ===\n";
            ss << "File A: " << fileA << " (" << dataA.size() << " bytes)\n";
            ss << "File B: " << fileB << " (" << dataB.size() << " bytes)\n";
            if (dataA.size() != dataB.size())
                ss << "WARNING: Files differ in size!\n";
            ss << "Changed: " << totalChanged << " / " << maxLen << " bytes ("
               << std::fixed << std::setprecision(1)
               << (maxLen > 0 ? (100.0 * totalChanged / maxLen) : 0.0) << "%)\n";
            ss << "Regions: " << regions.size() << " contiguous diff region(s)\n\n";

            // Vector table comparison (6502 standard vectors)
            if (minLen >= 6 && dataA.size() >= 6 && dataB.size() >= 6) {
                // Check if files are large enough for standard vector locations
                // Vectors are at the END of the ROM: NMI=$FFFA, RESET=$FFFC, IRQ=$FFFE
                // Relative to base_addr, offset = 0xFFFA - base_addr (if base covers that range)
                size_t vecOff = 0;
                bool hasVectors = false;

                if (baseAddr <= 0xFFFA && baseAddr + dataA.size() >= 0x10000 &&
                    baseAddr + dataB.size() >= 0x10000) {
                    vecOff = 0xFFFA - baseAddr;
                    hasVectors = true;
                } else if (dataA.size() >= 6 && dataB.size() >= 6) {
                    // Try end-of-file vectors (common for raw ROM dumps)
                    vecOff = minLen - 6;
                    // Only show if the values look like plausible 6502 vectors
                    uint16_t nmiA = dataA[vecOff] | (dataA[vecOff+1] << 8);
                    uint16_t rstA = dataA[vecOff+2] | (dataA[vecOff+3] << 8);
                    if (nmiA >= 0x8000 && rstA >= 0x8000) hasVectors = true;
                }

                if (hasVectors && vecOff + 6 <= dataA.size() && vecOff + 6 <= dataB.size()) {
                    ss << "--- Vector Table ---\n";
                    const char* vecNames[] = {"NMI", "RESET", "IRQ"};
                    for (int v = 0; v < 3; ++v) {
                        size_t off = vecOff + v * 2;
                        uint16_t addrA = dataA[off] | (dataA[off+1] << 8);
                        uint16_t addrB = dataB[off] | (dataB[off+1] << 8);
                        ss << vecNames[v] << ": $" << toHex(addrA, 4);
                        if (addrA != addrB)
                            ss << " -> $" << toHex(addrB, 4) << " CHANGED";
                        else
                            ss << " (unchanged)";
                        ss << "\n";
                    }
                    ss << "\n";
                }
            }

            // Diff regions detail (limit output to first 50 regions)
            int shown = 0;
            for (const auto& r : regions) {
                if (shown >= 50) {
                    ss << "... (" << (regions.size() - 50) << " more regions omitted)\n";
                    break;
                }

                uint32_t dispAddr = baseAddr + r.offset;
                ss << "$" << toHex(dispAddr, 4) << "-$"
                   << toHex(dispAddr + r.length - 1, 4)
                   << " (" << r.length << " byte" << (r.length > 1 ? "s" : "") << ")";

                // Symbol annotation
                if (symTab) {
                    uint32_t symOff = 0;
                    std::string label = symTab->nearest(dispAddr, symOff);
                    if (!label.empty()) {
                        ss << "  ; " << label;
                        if (symOff > 0) ss << "+" << symOff;
                    }
                }
                ss << "\n";

                // Show bytes (limit to 16 per region for readability)
                uint32_t showLen = std::min(r.length, (uint32_t)16);
                uint32_t ctxStart = (context > 0 && r.offset >= (uint32_t)context)
                                    ? r.offset - context : r.offset;
                uint32_t ctxEnd = std::min((uint32_t)maxLen, r.offset + showLen + context);

                ss << "  A: ";
                for (uint32_t i = ctxStart; i < ctxEnd; ++i) {
                    if (i == r.offset) ss << "[";
                    ss << toHex(i < dataA.size() ? dataA[i] : 0, 2);
                    if (i == r.offset + showLen - 1) ss << "]";
                    ss << " ";
                }
                ss << "\n  B: ";
                for (uint32_t i = ctxStart; i < ctxEnd; ++i) {
                    if (i == r.offset) ss << "[";
                    ss << toHex(i < dataB.size() ? dataB[i] : 0, 2);
                    if (i == r.offset + showLen - 1) ss << "]";
                    ss << " ";
                }
                if (r.length > 16) ss << "...";
                ss << "\n\n";
                ++shown;
            }

            if (regions.empty())
                ss << "Files are identical.\n";

            textItem.oVal["text"] = Json(ss.str());
        }
    // -----------------------------------------------------------------------
    // Test automation tools (#71)
    // -----------------------------------------------------------------------

    } else if (name == "test_sequence") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else if (!args.contains("commands") || !args["commands"].is_array()) {
            textItem.oVal["text"] = Json("Error: 'commands' must be an array");
            textItem.oVal["isError"] = Json(true);
        } else {
            Json results(Json::ARR);
            for (const auto& cmd : args["commands"].aVal) {
                std::string tool = cmd.contains("tool") ? cmd["tool"].sVal : "";
                Json toolArgs = cmd.contains("args") ? cmd["args"] : Json(Json::OBJ);
                // Auto-inject machine_id
                if (!toolArgs.contains("machine_id"))
                    toolArgs.oVal["machine_id"] = Json(mid);

                Json cmdResult(Json::OBJ);
                cmdResult.oVal["tool"] = Json(tool);

                if (tool.empty()) {
                    cmdResult.oVal["error"] = Json("Missing 'tool' field");
                } else {
                    Json inner = dispatchToolInternal(tool, toolArgs);
                    cmdResult.oVal["result"] = inner;
                }
                results.push_back(cmdResult);
            }
            // Build a structured response with all results
            Json structured(Json::OBJ);
            structured.oVal["sequence_results"] = results;
            structured.oVal["final_registers"] = buildRegistersJson(ms);
            textItem.oVal["text"] = Json(structured.stringify());
        }

    } else if (name == "test_assert") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            int maxSteps = args.contains("timeout_steps") ? (int)args["timeout_steps"].nVal : 10000000;
            bool ignorePE = args.contains("ignore_program_end") && args["ignore_program_end"].bVal;
            bool stopBrk = args.contains("stop_on_brk") && args["stop_on_brk"].bVal;
            uint32_t entryAddr = 0;
            bool hasEntry = false;
            bool loadOk = true;

            // Load image if specified
            if (args.contains("load") && !args["load"].sVal.empty()) {
                std::string path = args["load"].sVal;
                IBus* loadBus = ms->machine->buses[0].bus; // physical bus (#79)
                auto* loader = ImageLoaderRegistry::instance().findLoader(path);
                if (loader && loader->load(path, loadBus, ms->machine, 0)) {
                    // Extract PRG load address as default entry
                    if (std::string(loader->name()).find("PRG") != std::string::npos) {
                        std::ifstream f(path, std::ios::binary);
                        uint8_t h[2]; f.read((char*)h, 2);
                        entryAddr = h[0] | (h[1] << 8);
                        hasEntry = true;
                    }
                } else {
                    textItem.oVal["text"] = Json("Error: Failed to load image");
                    textItem.oVal["isError"] = Json(true);
                    loadOk = false;
                }
            }

            if (loadOk) {
                // Set entry point
                if (args.contains("entry") && !args["entry"].sVal.empty()) {
                    if (resolveAddr(args["entry"], ms->dbg, entryAddr))
                        hasEntry = true;
                }
                if (hasEntry) ms->cpu->setPc(entryAddr);

                // Run
                bool bpHit = false;
                std::string stopReason = runWithBudget(ms, maxSteps, ignorePE, bpHit, stopBrk);

                // Build result
                Json result(Json::OBJ);
                result.oVal["stop_reason"] = Json(stopReason);
                result.oVal["registers"] = buildRegistersJson(ms);
                result.oVal["pc"] = Json("$" + toHex(ms->cpu->pc()));

                // Evaluate assertions
                bool allPass = true;
                Json failures(Json::ARR);

                if (args.contains("assertions") && args["assertions"].is_object()) {
                    const Json& asserts = args["assertions"];

                    // Check halt_pc
                    if (asserts.contains("halt_pc")) {
                        uint32_t expectedPc;
                        if (resolveAddr(asserts["halt_pc"], ms->dbg, expectedPc)) {
                            if (ms->cpu->pc() != expectedPc) {
                                Json f(Json::OBJ);
                                f.oVal["type"] = Json("halt_pc");
                                f.oVal["expected"] = Json("$" + toHex(expectedPc));
                                f.oVal["actual"] = Json("$" + toHex(ms->cpu->pc()));
                                failures.push_back(f);
                                allPass = false;
                            }
                        }
                    }

                    // Check exit_type
                    if (asserts.contains("exit_type")) {
                        std::string expected = asserts["exit_type"].sVal;
                        if (stopReason != expected) {
                            Json f(Json::OBJ);
                            f.oVal["type"] = Json("exit_type");
                            f.oVal["expected"] = Json(expected);
                            f.oVal["actual"] = Json(stopReason);
                            failures.push_back(f);
                            allPass = false;
                        }
                    }

                    // Check registers
                    if (asserts.contains("registers") && asserts["registers"].is_object()) {
                        for (const auto& kv : asserts["registers"].oVal) {
                            std::string regName = kv.first;
                            int expected = (int)kv.second.nVal;
                            // Find register by name
                            int regCount = ms->cpu->regCount();
                            bool found = false;
                            for (int i = 0; i < regCount; ++i) {
                                const auto* desc = ms->cpu->regDescriptor(i);
                                if (desc->name == regName) {
                                    int actual = (int)ms->cpu->regRead(i);
                                    if (actual != expected) {
                                        Json f(Json::OBJ);
                                        f.oVal["type"] = Json("register");
                                        f.oVal["register"] = Json(regName);
                                        f.oVal["expected"] = Json(expected);
                                        f.oVal["actual"] = Json(actual);
                                        failures.push_back(f);
                                        allPass = false;
                                    }
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) {
                                Json f(Json::OBJ);
                                f.oVal["type"] = Json("register");
                                f.oVal["register"] = Json(regName);
                                f.oVal["error"] = Json("Unknown register name");
                                failures.push_back(f);
                                allPass = false;
                            }
                        }
                    }

                    // Check memory
                    if (asserts.contains("memory") && asserts["memory"].is_object()) {
                        for (const auto& kv : asserts["memory"].oVal) {
                            uint32_t addr;
                            if (!ExpressionEvaluator::evaluate(kv.first, ms->dbg, addr)) {
                                Json f(Json::OBJ);
                                f.oVal["type"] = Json("memory");
                                f.oVal["address"] = Json(kv.first);
                                f.oVal["error"] = Json("Invalid address expression");
                                failures.push_back(f);
                                allPass = false;
                                continue;
                            }
                            if (!kv.second.is_array()) continue;
                            for (size_t i = 0; i < kv.second.aVal.size(); ++i) {
                                int expected = (int)kv.second.aVal[i].nVal;
                                int actual = (int)ms->bus->peek8(addr + i);
                                if (actual != expected) {
                                    Json f(Json::OBJ);
                                    f.oVal["type"] = Json("memory");
                                    f.oVal["address"] = Json("$" + toHex(addr + (uint32_t)i));
                                    f.oVal["offset"] = Json((int)i);
                                    f.oVal["expected"] = Json(expected);
                                    f.oVal["actual"] = Json(actual);
                                    failures.push_back(f);
                                    allPass = false;
                                }
                            }
                        }
                    }
                }

                result.oVal["pass"] = Json(allPass);
                result.oVal["failures"] = failures;
                textItem.oVal["text"] = Json(result.stringify());
            }
        }

    } else if (name == "test_diagnose") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            uint32_t watchAddr;
            std::string errMsg;
            if (!resolveAddrWithDiagnostic(args["watch_addr"], ms->dbg, watchAddr, errMsg)) {
                textItem.oVal["text"] = Json("Error: watch_addr - " + errMsg);
                textItem.oVal["isError"] = Json(true);
            } else {
                int maxSteps = args.contains("timeout_steps") ? (int)args["timeout_steps"].nVal : 10000000;
                int traceDepth = args.contains("trace_depth") ? (int)args["trace_depth"].nVal : 20;
                bool ignorePE = args.contains("ignore_program_end") && args["ignore_program_end"].bVal;
                std::string watchType = args.contains("watch_type") ? args["watch_type"].sVal : "write";
                bool loadOk = true;
                uint32_t entryAddr = 0;
                bool hasEntry = false;

                // Load image if specified
                if (args.contains("load") && !args["load"].sVal.empty()) {
                    std::string path = args["load"].sVal;
                    IBus* loadBus = ms->machine->buses[0].bus; // physical bus (#79)
                    auto* loader = ImageLoaderRegistry::instance().findLoader(path);
                    if (loader && loader->load(path, loadBus, ms->machine, 0)) {
                        if (std::string(loader->name()).find("PRG") != std::string::npos) {
                            std::ifstream f(path, std::ios::binary);
                            uint8_t h[2]; f.read((char*)h, 2);
                            entryAddr = h[0] | (h[1] << 8);
                            hasEntry = true;
                        }
                    } else {
                        textItem.oVal["text"] = Json("Error: Failed to load image");
                        textItem.oVal["isError"] = Json(true);
                        loadOk = false;
                    }
                }

                if (loadOk) {
                    // Set entry point
                    if (args.contains("entry") && !args["entry"].sVal.empty()) {
                        if (resolveAddr(args["entry"], ms->dbg, entryAddr))
                            hasEntry = true;
                    }
                    if (hasEntry) ms->cpu->setPc(entryAddr);

                    // Set watchpoint
                    BreakpointType btype = (watchType == "read") ?
                        BreakpointType::READ_WATCH : BreakpointType::WRITE_WATCH;
                    int wpId = ms->dbg->breakpoints().add(watchAddr, btype);

                    // Run until watchpoint or timeout
                    bool bpHit = false;
                    std::string stopReason = runWithBudget(ms, maxSteps, ignorePE, bpHit);

                    // Remove the watchpoint we added
                    ms->dbg->breakpoints().remove(wpId);

                    // Build result
                    Json result(Json::OBJ);
                    result.oVal["stop_reason"] = Json(stopReason);
                    result.oVal["watchpoint_hit"] = Json(bpHit);
                    result.oVal["watch_addr"] = Json("$" + toHex(watchAddr));
                    result.oVal["registers"] = buildRegistersJson(ms);
                    result.oVal["pc"] = Json("$" + toHex(ms->cpu->pc()));

                    // Actual value at watched address
                    if (args.contains("expected") && args["expected"].is_array()) {
                        Json comparison(Json::ARR);
                        for (size_t i = 0; i < args["expected"].aVal.size(); ++i) {
                            int expected = (int)args["expected"].aVal[i].nVal;
                            int actual = (int)ms->bus->peek8(watchAddr + i);
                            Json entry(Json::OBJ);
                            entry.oVal["address"] = Json("$" + toHex(watchAddr + (uint32_t)i));
                            entry.oVal["expected"] = Json(expected);
                            entry.oVal["actual"] = Json(actual);
                            entry.oVal["match"] = Json(expected == actual);
                            comparison.push_back(entry);
                        }
                        result.oVal["comparison"] = comparison;
                    }

                    // Disassemble around current PC
                    if (ms->disasm && bpHit) {
                        // Show a few instructions before and at current PC
                        uint32_t disasmStart = ms->cpu->pc() >= 8 ? ms->cpu->pc() - 8 : 0;
                        Json disasmArr(Json::ARR);
                        uint32_t addr = disasmStart;
                        for (int i = 0; i < 8 && addr <= 0xFFFF; ++i) {
                            char buf[64];
                            int len = ms->disasm->disasmOne(ms->bus, addr, buf, sizeof(buf));
                            if (len <= 0) break;
                            Json line(Json::OBJ);
                            line.oVal["addr"] = Json("$" + toHex(addr));
                            line.oVal["instruction"] = Json(buf);
                            line.oVal["current"] = Json(addr == ms->cpu->pc());
                            disasmArr.push_back(line);
                            addr += len;
                        }
                        result.oVal["disassembly"] = disasmArr;
                    }

                    // Trace buffer context
                    if (ms->dbg && bpHit) {
                        auto& tb = ms->dbg->trace();
                        int avail = (int)tb.size();
                        int show = std::min(traceDepth, avail);
                        Json traceArr(Json::ARR);
                        for (int i = avail - show; i < avail; ++i) {
                            const auto& entry = tb.at(i);
                            std::stringstream ss;
                            ss << "$" << toHex(entry.addr) << ": ";
                            if (!entry.mnemonic.empty()) {
                                ss << entry.mnemonic;
                            } else if (ms->disasm) {
                                char buf[64];
                                ms->disasm->disasmOne(ms->bus, entry.addr, buf, sizeof(buf));
                                ss << buf;
                            }
                            ss << "  [";
                            bool first = true;
                            for (const auto& rv : entry.regs) {
                                if (!first) ss << " ";
                                int w = (rv.first == "SP" || rv.first == "PC") ? 4 : 2;
                                ss << rv.first << "=$" << toHex(rv.second, w);
                                first = false;
                            }
                            ss << "]";
                            traceArr.push_back(Json(ss.str()));
                        }
                        result.oVal["trace"] = traceArr;
                    }

                    textItem.oVal["text"] = Json(result.stringify());
                }
            }
        }

    } else if (name == "profile_cpu") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            int steps = args.contains("steps") ? (int)args["steps"].nVal : 1000000;
            int top   = args.contains("top")   ? (int)args["top"].nVal   : 20;
            if (steps <= 0) steps = 1000000;
            if (top <= 0) top = 20;

            // Sample PC at each step
            std::unordered_map<uint32_t, int> histogram;
            uint64_t startCycles = ms->cpu->cycles();
            for (int i = 0; i < steps; ++i) {
                uint32_t pc = ms->cpu->pc();
                histogram[pc]++;
                if (ms->machine && ms->machine->schedulerStep)
                    ms->machine->schedulerStep(*ms->machine);
                else
                    ms->cpu->step();
                if (ms->dbg->isPaused()) break;
            }
            uint64_t totalCycles = ms->cpu->cycles() - startCycles;

            // Sort by count descending
            std::vector<std::pair<uint32_t, int>> sorted(histogram.begin(), histogram.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            if ((int)sorted.size() > top) sorted.resize(top);

            int totalSamples = 0;
            for (const auto& [addr, cnt] : histogram) totalSamples += cnt;

            Json result(Json::OBJ);
            result.oVal["total_steps"] = Json(totalSamples);
            result.oVal["total_cycles"] = Json((double)totalCycles);
            result.oVal["unique_addresses"] = Json((int)histogram.size());

            Json hotspots(Json::ARR);
            for (const auto& [addr, cnt] : sorted) {
                Json entry(Json::OBJ);
                entry.oVal["addr"] = Json("$" + toHex(addr));
                entry.oVal["count"] = Json(cnt);
                double pct = totalSamples > 0 ? (100.0 * cnt / totalSamples) : 0;
                entry.oVal["percent"] = Json(pct);
                // Symbol annotation
                if (ms->dbg) {
                    std::string sym = ms->dbg->symbols().getLabel(addr);
                    if (!sym.empty()) entry.oVal["symbol"] = Json(sym);
                }
                // Disassembly
                if (ms->disasm) {
                    char buf[64];
                    ms->disasm->disasmOne(ms->bus, addr, buf, sizeof(buf));
                    entry.oVal["instruction"] = Json(std::string(buf));
                }
                hotspots.push_back(entry);
            }
            result.oVal["hotspots"] = hotspots;
            textItem.oVal["text"] = Json(result.stringify());
        }

    } else if (name == "measure_region") {
        std::string mid = args["machine_id"].sVal;
        MachineState* ms = getMachine(mid);
        if (!ms) {
            textItem.oVal["text"] = Json("Error: Invalid machine ID");
            textItem.oVal["isError"] = Json(true);
        } else {
            uint32_t startAddr = 0, endAddr = 0;
            if (!resolveAddr(args["addr"], ms->dbg, startAddr) ||
                !resolveAddr(args["end_addr"], ms->dbg, endAddr)) {
                textItem.oVal["text"] = Json("Error: Invalid address expression");
                textItem.oVal["isError"] = Json(true);
            } else {
                int maxSteps = args.contains("max_steps") ? (int)args["max_steps"].nVal : 10000000;
                ms->cpu->setPc(startAddr);
                uint64_t startCycles = ms->cpu->cycles();
                int instrCount = 0;

                for (int i = 0; i < maxSteps; ++i) {
                    uint32_t pc = ms->cpu->pc();
                    if (pc < startAddr || pc >= endAddr) break;
                    if (ms->machine && ms->machine->schedulerStep)
                        ms->machine->schedulerStep(*ms->machine);
                    else
                        ms->cpu->step();
                    instrCount++;
                    if (ms->dbg->isPaused()) break;
                }
                uint64_t totalCycles = ms->cpu->cycles() - startCycles;
                double avgCpi = instrCount > 0 ? (double)totalCycles / instrCount : 0;

                Json result(Json::OBJ);
                result.oVal["start_addr"] = Json("$" + toHex(startAddr));
                result.oVal["end_addr"] = Json("$" + toHex(endAddr));
                result.oVal["instructions"] = Json(instrCount);
                result.oVal["total_cycles"] = Json((double)totalCycles);
                result.oVal["avg_cycles_per_instruction"] = Json(avgCpi);
                result.oVal["final_pc"] = Json("$" + toHex(ms->cpu->pc()));
                textItem.oVal["text"] = Json(result.stringify());
            }
        }

    } else {
        textItem.oVal["text"] = Json("Error: Unknown tool " + name);
        textItem.oVal["isError"] = Json(true);
    }

    content.push_back(textItem);
    res.oVal["content"] = content;
    return res;
}

// Implementation of dispatchToolInternal — wraps handleToolsCall for batch use
static Json dispatchToolInternal(const std::string& toolName, const Json& toolArgs) {
    Json params(Json::OBJ);
    params.oVal["name"] = Json(toolName);
    params.oVal["arguments"] = toolArgs;
    Json fullResult = handleToolsCall(params);
    // Extract the text content from the standard MCP response
    if (fullResult.contains("content") && fullResult["content"].is_array() &&
        !fullResult["content"].aVal.empty()) {
        const Json& item = fullResult["content"].aVal[0];
        Json extracted(Json::OBJ);
        if (item.contains("text")) extracted.oVal["text"] = item["text"];
        if (item.contains("isError")) extracted.oVal["error"] = Json(true);
        return extracted;
    }
    Json err(Json::OBJ);
    err.oVal["error"] = Json("Internal dispatch failure");
    return err;
}

static Json handleInitialize(const Json& params) {
    (void)params;
    Json res(Json::OBJ);
    res.oVal["protocolVersion"] = Json("2024-11-05");

    Json info(Json::OBJ);
    info.oVal["name"] = Json("mmemu-mcp");
    info.oVal["version"] = Json(MMSIM_VERSION_FULL);
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
    LogRegistry::instance().init();

    // Parse verbosity flags
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-vv" || arg == "--trace")
            LogRegistry::instance().setGlobalLevel(spdlog::level::trace);
        else if (arg == "-v" || arg == "--verbose")
            LogRegistry::instance().setGlobalLevel(spdlog::level::debug);
    }
    PluginLoader::instance().loadFromStandardLocations();
    SimConfig::instance().load();

    // Load JSON machines after all plugins are registered
    JsonMachineLoader jsonLoader;
    jsonLoader.loadFile("machines/rawMega65.json");

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
