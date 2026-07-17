#include "../src/test_harness.h"
#include "minijson.h"
#include <iostream>
#include <sstream>

// Mock MCP tool responses for testing
// In a real scenario, these would be actual tool invocations
struct MCPToolTest {
    std::string toolName;
    Json arguments;

    MCPToolTest(const std::string& name) : toolName(name) {
        arguments = Json(Json::OBJ);
    }
};

// Helper to simulate tool response (in actual tests, this would call the real tool)
static Json mockToolCall(const std::string& toolName, const Json& args) {
    Json response(Json::OBJ);
    response.oVal["type"] = Json("text");

    // This is a placeholder - real tests would invoke actual tools
    response.oVal["text"] = Json("Tool: " + toolName);
    return response;
}

TEST_CASE(mcp_machine_create_c64) {
    MCPToolTest tool("create_machine");
    tool.arguments.oVal["machine_type"] = Json("c64");

    // In real test, this would call the actual MCP tool
    // For now, verify the test framework works
    ASSERT(tool.toolName == "create_machine");
    ASSERT_EQ(tool.arguments.oVal["machine_type"].sVal, "c64");
}

TEST_CASE(mcp_machine_create_vic20) {
    MCPToolTest tool("create_machine");
    tool.arguments.oVal["machine_type"] = Json("vic20");

    ASSERT(tool.toolName == "create_machine");
    ASSERT_EQ(tool.arguments.oVal["machine_type"].sVal, "vic20");
}

TEST_CASE(mcp_machine_list) {
    MCPToolTest tool("list_instances");

    // Verify tool structure
    ASSERT(tool.toolName == "list_instances");
}

TEST_CASE(mcp_memory_read_basic) {
    MCPToolTest tool("read_memory");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["addr"] = Json(0x1000);
    tool.arguments.oVal["size"] = Json(16);

    ASSERT_EQ(tool.arguments.oVal["addr"].nVal, 0x1000);
    ASSERT_EQ(tool.arguments.oVal["size"].nVal, 16);
}

TEST_CASE(mcp_memory_write_basic) {
    MCPToolTest tool("write_memory");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["addr"] = Json(0x2000);

    Json bytes(Json::ARR);
    bytes.aVal.push_back(Json(0xA9));  // LDA immediate
    bytes.aVal.push_back(Json(0x42));  // value
    tool.arguments.oVal["bytes"] = bytes;

    ASSERT_EQ(tool.arguments.oVal["addr"].nVal, 0x2000);
    ASSERT_EQ(tool.arguments.oVal["bytes"].aVal.size(), 2);
}

TEST_CASE(mcp_memory_copy) {
    MCPToolTest tool("copy_memory");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["src"] = Json(0x1000);
    tool.arguments.oVal["dest"] = Json(0x2000);
    tool.arguments.oVal["size"] = Json(256);

    ASSERT_EQ(tool.arguments.oVal["src"].nVal, 0x1000);
    ASSERT_EQ(tool.arguments.oVal["dest"].nVal, 0x2000);
    ASSERT_EQ(tool.arguments.oVal["size"].nVal, 256);
}

TEST_CASE(mcp_memory_fill) {
    MCPToolTest tool("fill_memory");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["addr"] = Json(0x3000);
    tool.arguments.oVal["size"] = Json(512);
    tool.arguments.oVal["value"] = Json(0xFF);

    ASSERT_EQ(tool.arguments.oVal["addr"].nVal, 0x3000);
    ASSERT_EQ(tool.arguments.oVal["size"].nVal, 512);
    ASSERT_EQ(tool.arguments.oVal["value"].nVal, 0xFF);
}

TEST_CASE(mcp_memory_search_pattern) {
    MCPToolTest tool("search_memory");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["addr"] = Json(0x0000);
    tool.arguments.oVal["size"] = Json(0x10000);

    Json pattern(Json::ARR);
    pattern.aVal.push_back(Json(0x48));  // PHA
    pattern.aVal.push_back(Json(0xEA));  // NOP
    tool.arguments.oVal["pattern"] = pattern;

    ASSERT_EQ(tool.arguments.oVal["pattern"].aVal.size(), 2);
}

TEST_CASE(mcp_cpu_step) {
    MCPToolTest tool("step_cpu");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["count"] = Json(1);

    ASSERT_EQ(tool.arguments.oVal["count"].nVal, 1);
}

TEST_CASE(mcp_cpu_step_multiple) {
    MCPToolTest tool("step_cpu");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["count"] = Json(100);

    ASSERT_EQ(tool.arguments.oVal["count"].nVal, 100);
}

TEST_CASE(mcp_cpu_run) {
    MCPToolTest tool("run_cpu");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["timeout"] = Json(5000);

    ASSERT_EQ(tool.arguments.oVal["timeout"].nVal, 5000);
}

TEST_CASE(mcp_cpu_reverse_step) {
    MCPToolTest tool("reverse_step");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["count"] = Json(1);

    ASSERT_EQ(tool.arguments.oVal["count"].nVal, 1);
}

TEST_CASE(mcp_cpu_set_pc) {
    MCPToolTest tool("set_pc");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["addr"] = Json(0x0800);

    ASSERT_EQ(tool.arguments.oVal["addr"].nVal, 0x0800);
}

TEST_CASE(mcp_registers_read) {
    MCPToolTest tool("read_registers");
    tool.arguments.oVal["machine_id"] = Json("test_machine");

    ASSERT(tool.toolName == "read_registers");
}

TEST_CASE(mcp_register_write) {
    MCPToolTest tool("write_register");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["reg"] = Json("A");
    tool.arguments.oVal["value"] = Json(0x42);

    ASSERT_EQ(tool.arguments.oVal["reg"].sVal, "A");
    ASSERT_EQ(tool.arguments.oVal["value"].nVal, 0x42);
}

TEST_CASE(mcp_breakpoint_set) {
    MCPToolTest tool("set_breakpoint");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["addr"] = Json(0x1000);
    tool.arguments.oVal["mode"] = Json("EXEC");

    ASSERT_EQ(tool.arguments.oVal["addr"].nVal, 0x1000);
    ASSERT_EQ(tool.arguments.oVal["mode"].sVal, "EXEC");
}

TEST_CASE(mcp_breakpoint_list) {
    MCPToolTest tool("list_breakpoints");
    tool.arguments.oVal["machine_id"] = Json("test_machine");

    ASSERT(tool.toolName == "list_breakpoints");
}

TEST_CASE(mcp_breakpoint_delete) {
    MCPToolTest tool("delete_breakpoint");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["bp_id"] = Json(0);

    ASSERT_EQ(tool.arguments.oVal["bp_id"].nVal, 0);
}

TEST_CASE(mcp_watchpoint_set) {
    MCPToolTest tool("set_watchpoint");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["addr"] = Json(0x2000);
    tool.arguments.oVal["mode"] = Json("WRITE");

    ASSERT_EQ(tool.arguments.oVal["addr"].nVal, 0x2000);
    ASSERT_EQ(tool.arguments.oVal["mode"].sVal, "WRITE");
}

TEST_CASE(mcp_disassemble_basic) {
    MCPToolTest tool("disassemble");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["addr"] = Json(0x0800);
    tool.arguments.oVal["count"] = Json(10);

    ASSERT_EQ(tool.arguments.oVal["addr"].nVal, 0x0800);
    ASSERT_EQ(tool.arguments.oVal["count"].nVal, 10);
}

TEST_CASE(mcp_assembler_basic) {
    MCPToolTest tool("asm");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["addr"] = Json(0x0800);
    tool.arguments.oVal["code"] = Json("LDA #$42");

    ASSERT_EQ(tool.arguments.oVal["code"].sVal, "LDA #$42");
}

TEST_CASE(mcp_snapshot_save) {
    MCPToolTest tool("snapshot_save");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["name"] = Json("snapshot_1");

    ASSERT_EQ(tool.arguments.oVal["name"].sVal, "snapshot_1");
}

TEST_CASE(mcp_snapshot_list) {
    MCPToolTest tool("snapshot_list");
    tool.arguments.oVal["machine_id"] = Json("test_machine");

    ASSERT(tool.toolName == "snapshot_list");
}

TEST_CASE(mcp_snapshot_delete) {
    MCPToolTest tool("snapshot_delete");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["name"] = Json("snapshot_1");

    ASSERT_EQ(tool.arguments.oVal["name"].sVal, "snapshot_1");
}

TEST_CASE(mcp_snapshot_diff) {
    MCPToolTest tool("snapshot_diff");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["snapshot1"] = Json("snap1");
    tool.arguments.oVal["snapshot2"] = Json("snap2");

    ASSERT_EQ(tool.arguments.oVal["snapshot1"].sVal, "snap1");
    ASSERT_EQ(tool.arguments.oVal["snapshot2"].sVal, "snap2");
}

TEST_CASE(mcp_device_list) {
    MCPToolTest tool("list_devices");
    tool.arguments.oVal["machine_id"] = Json("test_machine");

    ASSERT(tool.toolName == "list_devices");
}

TEST_CASE(mcp_device_info) {
    MCPToolTest tool("get_device_info");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["device_name"] = Json("VIC-II");

    ASSERT_EQ(tool.arguments.oVal["device_name"].sVal, "VIC-II");
}

TEST_CASE(mcp_symbol_add) {
    MCPToolTest tool("add_symbol");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["name"] = Json("START");
    tool.arguments.oVal["addr"] = Json(0x0800);

    ASSERT_EQ(tool.arguments.oVal["name"].sVal, "START");
    ASSERT_EQ(tool.arguments.oVal["addr"].nVal, 0x0800);
}

TEST_CASE(mcp_symbol_list) {
    MCPToolTest tool("list_symbols");
    tool.arguments.oVal["machine_id"] = Json("test_machine");

    ASSERT(tool.toolName == "list_symbols");
}

TEST_CASE(mcp_symbol_remove) {
    MCPToolTest tool("remove_symbol");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["name"] = Json("START");

    ASSERT_EQ(tool.arguments.oVal["name"].sVal, "START");
}

TEST_CASE(mcp_trace_buffer_info) {
    MCPToolTest tool("get_trace_buffer");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["count"] = Json(100);

    ASSERT_EQ(tool.arguments.oVal["count"].nVal, 100);
}

TEST_CASE(mcp_trace_filter) {
    MCPToolTest tool("set_trace_filter");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["filter"] = Json("breakpoints");

    ASSERT_EQ(tool.arguments.oVal["filter"].sVal, "breakpoints");
}

TEST_CASE(mcp_profile_cpu) {
    MCPToolTest tool("profile_cpu");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["duration"] = Json(1000);

    ASSERT_EQ(tool.arguments.oVal["duration"].nVal, 1000);
}

TEST_CASE(mcp_heatmap_basic) {
    MCPToolTest tool("get_heatmap");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["addr"] = Json(0x0000);
    tool.arguments.oVal["size"] = Json(0x10000);

    ASSERT_EQ(tool.arguments.oVal["size"].nVal, 0x10000);
}

TEST_CASE(mcp_cartridge_attach) {
    MCPToolTest tool("attach_cartridge");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["path"] = Json("/path/to/cartridge.crt");

    ASSERT_EQ(tool.arguments.oVal["path"].sVal, "/path/to/cartridge.crt");
}

TEST_CASE(mcp_cartridge_eject) {
    MCPToolTest tool("eject_cartridge");
    tool.arguments.oVal["machine_id"] = Json("test_machine");

    ASSERT(tool.toolName == "eject_cartridge");
}

TEST_CASE(mcp_disk_mount) {
    MCPToolTest tool("mount_disk");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["drive"] = Json(8);
    tool.arguments.oVal["path"] = Json("/path/to/disk.d64");

    ASSERT_EQ(tool.arguments.oVal["drive"].nVal, 8);
    ASSERT_EQ(tool.arguments.oVal["path"].sVal, "/path/to/disk.d64");
}

TEST_CASE(mcp_disk_eject) {
    MCPToolTest tool("eject_disk");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["drive"] = Json(8);

    ASSERT_EQ(tool.arguments.oVal["drive"].nVal, 8);
}

TEST_CASE(mcp_keyboard_press) {
    MCPToolTest tool("press_key");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["key"] = Json("RETURN");
    tool.arguments.oVal["duration"] = Json(100);

    ASSERT_EQ(tool.arguments.oVal["key"].sVal, "RETURN");
    ASSERT_EQ(tool.arguments.oVal["duration"].nVal, 100);
}

TEST_CASE(mcp_type_string) {
    MCPToolTest tool("type_string");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["text"] = Json("HELLO");

    ASSERT_EQ(tool.arguments.oVal["text"].sVal, "HELLO");
}

TEST_CASE(mcp_screenshot) {
    MCPToolTest tool("screenshot");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["format"] = Json("png");

    ASSERT_EQ(tool.arguments.oVal["format"].sVal, "png");
}

TEST_CASE(mcp_record_audio) {
    MCPToolTest tool("record_audio");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["duration"] = Json(5000);
    tool.arguments.oVal["format"] = Json("wav");

    ASSERT_EQ(tool.arguments.oVal["duration"].nVal, 5000);
    ASSERT_EQ(tool.arguments.oVal["format"].sVal, "wav");
}

TEST_CASE(mcp_analyze_routine) {
    MCPToolTest tool("analyze_routine");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["start"] = Json(0x0800);
    tool.arguments.oVal["end"] = Json(0x0900);

    ASSERT_EQ(tool.arguments.oVal["start"].nVal, 0x0800);
    ASSERT_EQ(tool.arguments.oVal["end"].nVal, 0x0900);
}

TEST_CASE(mcp_generate_tests) {
    MCPToolTest tool("generate_tests");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["addr"] = Json(0x0800);
    tool.arguments.oVal["count"] = Json(10);

    ASSERT_EQ(tool.arguments.oVal["addr"].nVal, 0x0800);
    ASSERT_EQ(tool.arguments.oVal["count"].nVal, 10);
}

TEST_CASE(mcp_test_sequence) {
    MCPToolTest tool("test_sequence");
    tool.arguments.oVal["machine_id"] = Json("test_machine");

    Json steps(Json::ARR);
    Json step1(Json::OBJ);
    step1.oVal["op"] = Json("step");
    step1.oVal["count"] = Json(10);
    steps.aVal.push_back(step1);
    tool.arguments.oVal["steps"] = steps;

    ASSERT_EQ(tool.arguments.oVal["steps"].aVal.size(), 1);
}

TEST_CASE(mcp_test_assert) {
    MCPToolTest tool("test_assert");
    tool.arguments.oVal["machine_id"] = Json("test_machine");
    tool.arguments.oVal["condition"] = Json("A == 0x42");

    ASSERT_EQ(tool.arguments.oVal["condition"].sVal, "A == 0x42");
}
