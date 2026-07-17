#include "../src/test_harness.h"
#include "mcp/main/mcp_test_interface.h"
#include "minijson.h"
#include <iostream>

// Global test machine for all tests
static std::string g_testMachineId;

// Helper to verify tool response
static bool isToolSuccess(const Json& result) {
    return !result.contains("error") || !result["error"].bVal;
}

static std::string getToolError(const Json& result) {
    if (result.contains("error")) {
        if (result["error"].type == Json::STR) {
            return result["error"].sVal;
        }
        return "Tool error";
    }
    return "";
}

TEST_CASE(mcp_tool_system_basic) {
    // Verify MCP test interface is available
    std::string machineId = MCPTest::createTestMachine("c64", "test_c64");
    ASSERT(machineId.length() > 0);

    // Verify machine was created
    auto machines = MCPTest::listTestMachines();
    ASSERT(machines.size() > 0);

    MCPTest::destroyTestMachine(machineId);
}

TEST_CASE(mcp_machine_create_c64_actual) {
    std::string machineId = MCPTest::createTestMachine("c64", "test_c64_create");
    ASSERT(machineId.length() > 0);

    // Verify basic machine operations work
    MCPTest::writeMemory(machineId, 0x0800, 0xAA);
    uint8_t val = MCPTest::readMemory(machineId, 0x0800);
    ASSERT_EQ(val, 0xAA);

    MCPTest::destroyTestMachine(machineId);
}

TEST_CASE(mcp_machine_create_vic20_actual) {
    std::string machineId = MCPTest::createTestMachine("vic20", "test_vic20_create");
    ASSERT(machineId.length() > 0);

    // Verify machine was created by checking we can access it
    uint64_t cycles = MCPTest::getCycles(machineId);
    ASSERT(cycles >= 0);

    MCPTest::destroyTestMachine(machineId);
}

TEST_CASE(mcp_memory_read_actual) {
    std::string machineId = MCPTest::createTestMachine("c64", "test_read_mem");
    ASSERT(machineId.length() > 0);

    Json args(Json::OBJ);
    args.oVal["machine_id"] = Json(machineId);
    args.oVal["addr"] = Json(0x0800);
    args.oVal["size"] = Json(16);

    Json result = MCPTest::invokeTool("read_memory", args);
    ASSERT(isToolSuccess(result));
    ASSERT(result.contains("text"));

    MCPTest::destroyTestMachine(machineId);
}

TEST_CASE(mcp_memory_write_actual) {
    std::string machineId = MCPTest::createTestMachine("c64", "test_write_mem");
    ASSERT(machineId.length() > 0);

    Json args(Json::OBJ);
    args.oVal["machine_id"] = Json(machineId);
    args.oVal["addr"] = Json(0x1000);

    Json bytes(Json::ARR);
    bytes.aVal.push_back(Json(0xA9));  // LDA immediate
    bytes.aVal.push_back(Json(0x42));  // Load $42
    args.oVal["bytes"] = bytes;

    Json result = MCPTest::invokeTool("write_memory", args);
    ASSERT(isToolSuccess(result));
    ASSERT(result.contains("text"));

    // Verify the write by reading back
    ASSERT_EQ(MCPTest::readMemory(machineId, 0x1000), 0xA9);
    ASSERT_EQ(MCPTest::readMemory(machineId, 0x1001), 0x42);

    MCPTest::destroyTestMachine(machineId);
}

TEST_CASE(mcp_cpu_step_actual) {
    std::string machineId = MCPTest::createTestMachine("c64", "test_step");
    ASSERT(machineId.length() > 0);

    uint64_t initialCycles = MCPTest::getCycles(machineId);

    Json args(Json::OBJ);
    args.oVal["machine_id"] = Json(machineId);
    args.oVal["count"] = Json(5);

    Json result = MCPTest::invokeTool("step_cpu", args);
    ASSERT(isToolSuccess(result));

    uint64_t finalCycles = MCPTest::getCycles(machineId);
    ASSERT(finalCycles > initialCycles);

    MCPTest::destroyTestMachine(machineId);
}

TEST_CASE(mcp_registers_read_actual) {
    std::string machineId = MCPTest::createTestMachine("c64", "test_read_regs");
    ASSERT(machineId.length() > 0);

    Json args(Json::OBJ);
    args.oVal["machine_id"] = Json(machineId);

    Json result = MCPTest::invokeTool("read_registers", args);
    ASSERT(isToolSuccess(result));
    ASSERT(result.contains("text"));

    MCPTest::destroyTestMachine(machineId);
}

TEST_CASE(mcp_register_write_actual) {
    std::string machineId = MCPTest::createTestMachine("c64", "test_write_reg");
    ASSERT(machineId.length() > 0);

    Json args(Json::OBJ);
    args.oVal["machine_id"] = Json(machineId);
    args.oVal["reg"] = Json("A");
    args.oVal["value"] = Json(0x42);

    Json result = MCPTest::invokeTool("write_register", args);
    ASSERT(isToolSuccess(result));

    // Verify the write
    uint32_t val = MCPTest::readRegister(machineId, "A");
    ASSERT_EQ(val, 0x42);

    MCPTest::destroyTestMachine(machineId);
}

TEST_CASE(mcp_set_pc_actual) {
    std::string machineId = MCPTest::createTestMachine("c64", "test_set_pc");
    ASSERT(machineId.length() > 0);

    Json args(Json::OBJ);
    args.oVal["machine_id"] = Json(machineId);
    args.oVal["addr"] = Json(0x0800);

    Json result = MCPTest::invokeTool("set_pc", args);
    ASSERT(isToolSuccess(result));

    // Verify the PC was set
    uint32_t pc = MCPTest::readRegister(machineId, "PC");
    ASSERT_EQ(pc, 0x0800);

    MCPTest::destroyTestMachine(machineId);
}

TEST_CASE(mcp_disassemble_actual) {
    std::string machineId = MCPTest::createTestMachine("c64", "test_disasm");
    ASSERT(machineId.length() > 0);

    // Write some code to disassemble
    MCPTest::writeMemory(machineId, 0x0800, 0xA9);  // LDA immediate
    MCPTest::writeMemory(machineId, 0x0801, 0x42);  // value

    Json args(Json::OBJ);
    args.oVal["machine_id"] = Json(machineId);
    args.oVal["addr"] = Json(0x0800);
    args.oVal["count"] = Json(3);

    Json result = MCPTest::invokeTool("disassemble", args);
    ASSERT(isToolSuccess(result));
    ASSERT(result.contains("text"));

    MCPTest::destroyTestMachine(machineId);
}

TEST_CASE(mcp_memory_copy_actual) {
    std::string machineId = MCPTest::createTestMachine("c64", "test_copy_mem");
    ASSERT(machineId.length() > 0);

    // Write source data
    for (int i = 0; i < 16; ++i) {
        MCPTest::writeMemory(machineId, 0x0800 + i, i);
    }

    Json args(Json::OBJ);
    args.oVal["machine_id"] = Json(machineId);
    args.oVal["src"] = Json(0x0800);
    args.oVal["dest"] = Json(0x0900);
    args.oVal["size"] = Json(16);

    Json result = MCPTest::invokeTool("copy_memory", args);
    ASSERT(isToolSuccess(result));

    // Verify copy
    for (int i = 0; i < 16; ++i) {
        uint8_t val = MCPTest::readMemory(machineId, 0x0900 + i);
        ASSERT_EQ(val, i);
    }

    MCPTest::destroyTestMachine(machineId);
}

TEST_CASE(mcp_memory_fill_actual) {
    std::string machineId = MCPTest::createTestMachine("c64", "test_fill_mem");
    ASSERT(machineId.length() > 0);

    Json args(Json::OBJ);
    args.oVal["machine_id"] = Json(machineId);
    args.oVal["addr"] = Json(0x2000);
    args.oVal["size"] = Json(32);
    args.oVal["value"] = Json(0xAA);

    Json result = MCPTest::invokeTool("fill_memory", args);
    ASSERT(isToolSuccess(result));

    // Verify fill
    for (int i = 0; i < 32; ++i) {
        uint8_t val = MCPTest::readMemory(machineId, 0x2000 + i);
        ASSERT_EQ(val, 0xAA);
    }

    MCPTest::destroyTestMachine(machineId);
}

TEST_CASE(mcp_search_memory_actual) {
    std::string machineId = MCPTest::createTestMachine("c64", "test_search_mem");
    ASSERT(machineId.length() > 0);

    // Write a pattern to search for
    MCPTest::writeMemory(machineId, 0x1000, 0x48);  // PHA
    MCPTest::writeMemory(machineId, 0x1001, 0xEA);  // NOP

    Json args(Json::OBJ);
    args.oVal["machine_id"] = Json(machineId);
    args.oVal["addr"] = Json(0x0000);
    args.oVal["size"] = Json(0x10000);

    Json pattern(Json::ARR);
    pattern.aVal.push_back(Json(0x48));  // PHA
    pattern.aVal.push_back(Json(0xEA));  // NOP
    args.oVal["pattern"] = pattern;

    Json result = MCPTest::invokeTool("search_memory", args);
    ASSERT(isToolSuccess(result));

    MCPTest::destroyTestMachine(machineId);
}

TEST_CASE(mcp_snapshot_operations_actual) {
    std::string machineId = MCPTest::createTestMachine("c64", "test_snapshot");
    ASSERT(machineId.length() > 0);

    // Save a snapshot
    Json saveArgs(Json::OBJ);
    saveArgs.oVal["machine_id"] = Json(machineId);
    saveArgs.oVal["name"] = Json("test_snap_1");

    Json saveResult = MCPTest::invokeTool("snapshot_save", saveArgs);
    ASSERT(isToolSuccess(saveResult));

    // List snapshots
    Json listArgs(Json::OBJ);
    listArgs.oVal["machine_id"] = Json(machineId);

    Json listResult = MCPTest::invokeTool("snapshot_list", listArgs);
    ASSERT(isToolSuccess(listResult));

    // Delete snapshot
    Json deleteArgs(Json::OBJ);
    deleteArgs.oVal["machine_id"] = Json(machineId);
    deleteArgs.oVal["name"] = Json("test_snap_1");

    Json deleteResult = MCPTest::invokeTool("snapshot_delete", deleteArgs);
    ASSERT(isToolSuccess(deleteResult));

    MCPTest::destroyTestMachine(machineId);
}

TEST_CASE(mcp_symbol_operations_actual) {
    std::string machineId = MCPTest::createTestMachine("c64", "test_symbols");
    ASSERT(machineId.length() > 0);

    // Add a symbol
    Json addArgs(Json::OBJ);
    addArgs.oVal["machine_id"] = Json(machineId);
    addArgs.oVal["name"] = Json("START");
    addArgs.oVal["addr"] = Json(0x0800);

    Json addResult = MCPTest::invokeTool("add_symbol", addArgs);
    ASSERT(isToolSuccess(addResult));

    // List symbols
    Json listArgs(Json::OBJ);
    listArgs.oVal["machine_id"] = Json(machineId);

    Json listResult = MCPTest::invokeTool("list_symbols", listArgs);
    ASSERT(isToolSuccess(listResult));

    // Remove symbol
    Json removeArgs(Json::OBJ);
    removeArgs.oVal["machine_id"] = Json(machineId);
    removeArgs.oVal["name"] = Json("START");

    Json removeResult = MCPTest::invokeTool("remove_symbol", removeArgs);
    ASSERT(isToolSuccess(removeResult));

    MCPTest::destroyTestMachine(machineId);
}

TEST_CASE(mcp_device_info_actual) {
    std::string machineId = MCPTest::createTestMachine("c64", "test_devices");
    ASSERT(machineId.length() > 0);

    Json args(Json::OBJ);
    args.oVal["machine_id"] = Json(machineId);
    args.oVal["device_name"] = Json("VIC-II");

    Json result = MCPTest::invokeTool("get_device_info", args);
    ASSERT(isToolSuccess(result));

    MCPTest::destroyTestMachine(machineId);
}

TEST_CASE(mcp_undo_info_actual) {
    std::string machineId = MCPTest::createTestMachine("c64", "test_undo");
    ASSERT(machineId.length() > 0);

    // Step to create trace entries
    Json stepArgs(Json::OBJ);
    stepArgs.oVal["machine_id"] = Json(machineId);
    stepArgs.oVal["count"] = Json(10);
    MCPTest::invokeTool("step_cpu", stepArgs);

    // Get undo info
    Json args(Json::OBJ);
    args.oVal["machine_id"] = Json(machineId);

    Json result = MCPTest::invokeTool("undo_info", args);
    ASSERT(isToolSuccess(result));
    ASSERT(result.contains("text"));

    MCPTest::destroyTestMachine(machineId);
}

TEST_CASE(mcp_trace_operations_actual) {
    std::string machineId = MCPTest::createTestMachine("c64", "test_trace");
    ASSERT(machineId.length() > 0);

    // Set trace filter
    Json filterArgs(Json::OBJ);
    filterArgs.oVal["machine_id"] = Json(machineId);
    filterArgs.oVal["filter"] = Json("all");

    Json filterResult = MCPTest::invokeTool("set_trace_filter", filterArgs);
    ASSERT(isToolSuccess(filterResult));

    // Get trace buffer
    Json bufferArgs(Json::OBJ);
    bufferArgs.oVal["machine_id"] = Json(machineId);
    bufferArgs.oVal["count"] = Json(10);

    Json bufferResult = MCPTest::invokeTool("get_trace_buffer", bufferArgs);
    ASSERT(isToolSuccess(bufferResult));

    MCPTest::destroyTestMachine(machineId);
}

TEST_CASE(mcp_machine_list_cleanup) {
    // Create multiple machines
    std::string m1 = MCPTest::createTestMachine("c64", "m1");
    std::string m2 = MCPTest::createTestMachine("vic20", "m2");
    ASSERT(m1.length() > 0);
    ASSERT(m2.length() > 0);

    // List should have 2 machines
    auto machines = MCPTest::listTestMachines();
    ASSERT_EQ(machines.size(), 2);

    // Clean up all
    MCPTest::cleanupAllTestMachines();
    machines = MCPTest::listTestMachines();
    ASSERT_EQ(machines.size(), 0);
}
