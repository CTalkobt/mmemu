import subprocess
import json
import sys
import time

class McpClient:
    def __init__(self):
        self.proc = subprocess.Popen(
            ["./bin/mmemu-mcp"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            bufsize=1
        )
        self.msg_id = 1

    def request(self, method, params=None):
        req = {
            "jsonrpc": "2.0",
            "id": self.msg_id,
            "method": method,
            "params": params or {}
        }
        self.msg_id += 1
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        
        while True:
            line = self.proc.stdout.readline()
            if not line:
                return None
            line = line.strip()
            if not line:
                continue
            try:
                res = json.loads(line)
                if res.get("id") == req["id"]:
                    return res
            except json.JSONDecodeError:
                continue

    def call_tool(self, name, args):
        return self.request("tools/call", {"name": name, "arguments": args})

    def close(self):
        self.proc.terminate()
        self.proc.wait()

def run_tests():
    client = McpClient()
    
    print("--- 1. Initialization ---")
    res = client.request("initialize", {
        "protocolVersion": "2024-11-05",
        "capabilities": {},
        "clientInfo": {"name": "test-suite", "version": "1.0"}
    })
    assert "result" in res, "Initialize failed"
    print("Initialize OK")

    print("\n--- 2. Machine Management ---")
    res = client.call_tool("create_machine", {"machine_id": "raw6502"})
    assert "result" in res, "create_machine failed"
    print("Create raw6502 OK")

    print("\n--- 3. Memory Operations ---")
    # Write
    res = client.call_tool("write_memory", {
        "machine_id": "raw6502",
        "addr": 0x1000,
        "bytes": [0x11, 0x22, 0x33, 0x44]
    })
    assert "result" in res and "content" in res["result"]
    print("Write memory response:", res["result"]["content"][0]["text"])

    # Read (Hex dump)
    res = client.call_tool("read_memory", {
        "machine_id": "raw6502",
        "addr": 0x1000,
        "size": 4
    })
    content = res["result"]["content"][0]["text"]
    print("Read memory hex dump:\n", content)
    assert "11 22 33 44" in content, "Memory read mismatch"

    # Fill
    client.call_tool("fill_memory", {
        "machine_id": "raw6502",
        "addr": 0x2000,
        "value": 0xEA,
        "size": 10
    })
    res = client.call_tool("read_memory", {
        "machine_id": "raw6502",
        "addr": 0x2000,
        "size": 10
    })
    assert "ea ea ea ea" in res["result"]["content"][0]["text"].lower()
    print("Fill memory OK")

    # Search
    res = client.call_tool("search_memory", {
        "machine_id": "raw6502",
        "pattern": "11 22 33",
        "is_hex": True
    })
    assert "Found at $1000" in res["result"]["content"][0]["text"], "Search failed"
    print("Search memory OK")

    print("\n--- 4. CPU Operations ---")
    # Set PC
    client.call_tool("set_pc", {"machine_id": "raw6502", "addr": 0x1000})
    # Step
    client.call_tool("write_memory", {"machine_id": "raw6502", "addr": 0x1000, "bytes": [0xEA]}) # NOP
    res = client.call_tool("step_cpu", {"machine_id": "raw6502", "count": 1})
    # Read Regs
    res = client.call_tool("read_registers", {"machine_id": "raw6502"})
    reg_text = res["result"]["content"][0]["text"]
    print("Registers after step:", reg_text)
    assert "PC: $1001" in reg_text, "PC did not advance correctly"

    print("\n--- 5. Breakpoints ---")
    res = client.call_tool("set_breakpoint", {"machine_id": "raw6502", "addr": 0x1005})
    bp_text = res["result"]["content"][0]["text"]
    print("Set breakpoint response:", bp_text)
    # Extract ID from "Breakpoint 1 at $1005"
    bp_id = int(bp_text.split()[1])

    res = client.call_tool("list_breakpoints", {"machine_id": "raw6502"})
    bp_list = res["result"]["content"][0]["text"]
    print("Breakpoint list:\n", bp_list)
    assert f"${0x1005:04x}" in bp_list.lower(), "Breakpoint missing from list"

    # Delete
    client.call_tool("delete_breakpoint", {"machine_id": "raw6502", "id": bp_id})
    res = client.call_tool("list_breakpoints", {"machine_id": "raw6502"})
    assert "No breakpoints set" in res["result"]["content"][0]["text"], "Breakpoint not deleted"
    print("Breakpoint operations OK")


    print("\n--- 6. Error Handling ---")
    # Invalid machine ID
    res = client.call_tool("read_registers", {"machine_id": "nonexistent"})
    assert "Error" in res["result"]["content"][0]["text"]
    print("Invalid machine ID error OK")

    # Unknown tool
    res = client.request("tools/call", {"name": "ghost_tool", "arguments": {}})
    assert "Error" in res["result"]["content"][0]["text"]
    print("Unknown tool error OK")

    print("\n--- 7. Resource Read ---")
    res = client.request("resources/read", {"uri": "machine_state"})
    assert "raw6502" in res["result"]["contents"][0]["text"]
    print("Resource read OK")

    print("\n--- 8. Assembler ---")
    res = client.call_tool("asm", {
        "machine_id": "raw6502",
        "source": "LDA #$42\nNOP",
        "load_addr": 0x1000
    })
    asm_result = json.loads(res["result"]["content"][0]["text"])
    assert asm_result["errors"] == [], f"Unexpected errors: {asm_result['errors']}"
    assert len(asm_result["bytes"]) >= 2, f"Expected assembled bytes, got: {asm_result['bytes']}"
    res2 = client.call_tool("read_memory", {"machine_id": "raw6502", "addr": 0x1000, "size": 2})
    assert "a9 42" in res2["result"]["content"][0]["text"].lower(), "LDA #$42 bytes not in memory"
    print("Assembler OK")

    print("\n--- 9. Search Navigation ---")
    # Plant two identical patterns at known addresses
    client.call_tool("write_memory", {"machine_id": "raw6502", "addr": 0x3000, "bytes": [0xDE, 0xAD]})
    client.call_tool("write_memory", {"machine_id": "raw6502", "addr": 0x3100, "bytes": [0xDE, 0xAD]})

    # Initial search finds first occurrence
    res = client.call_tool("search_memory", {"machine_id": "raw6502", "pattern": "DE AD", "is_hex": True})
    assert "3000" in res["result"]["content"][0]["text"].lower(), "Initial search failed"
    print("Initial search OK")

    # search_next advances to second occurrence
    res = client.call_tool("search_next", {"machine_id": "raw6502"})
    assert "3100" in res["result"]["content"][0]["text"].lower(), f"search_next failed: {res['result']['content'][0]['text']}"
    print("search_next OK")

    # search_prior goes back to first
    res = client.call_tool("search_prior", {"machine_id": "raw6502"})
    assert "3000" in res["result"]["content"][0]["text"].lower(), f"search_prior failed: {res['result']['content'][0]['text']}"
    print("search_prior OK")

    # search_next with no prior search gives clear error
    res2 = client.call_tool("search_next", {"machine_id": "c64"})
    assert "Error" in res2["result"]["content"][0]["text"], "Expected error for no prior search"
    print("Search navigation OK")

    print("\n--- 10. MEGA65-Specific Features ---")
    # Note: MAP features require mega65 machine, which uses SparseMemoryBus + MapMmu
    # Personality features require KEY register, which may not be in all machine setups

    # Test that get_map_state fails gracefully on non-mega65 machines
    res = client.call_tool("get_map_state", {"machine_id": "raw6502"})
    assert "Error" in res["result"]["content"][0]["text"], "Expected error for non-mega65 machine"
    print("get_map_state error handling OK")

    # Test that get_personality fails gracefully if KEY register not available
    res = client.call_tool("get_personality", {"machine_id": "raw6502"})
    assert "Error" in res["result"]["content"][0]["text"], "Expected error when KEY register not available"
    print("get_personality error handling OK")

    # Test set_map_state error handling
    res = client.call_tool("set_map_state", {"machine_id": "raw6502", "offsets": "0,0,0,0,0,0,0,0", "enables": 0})
    assert "Error" in res["result"]["content"][0]["text"], "Expected error for non-mega65 machine"
    print("set_map_state error handling OK")

    # Test set_personality error handling
    res = client.call_tool("set_personality", {"machine_id": "raw6502", "personality": "C64"})
    assert "Error" in res["result"]["content"][0]["text"], "Expected error when KEY register not available"
    print("set_personality error handling OK")

    # Test invalid personality name
    res = client.call_tool("set_personality", {"machine_id": "raw6502", "personality": "INVALID"})
    assert "Error" in res["result"]["content"][0]["text"], "Expected error for invalid personality"
    print("MEGA65-specific features error handling OK")

    print("\n--- 11. Trace Buffer ---")
    # Use raw6502 machine for trace testing
    client.call_tool("create_machine", {"machine_id": "raw6502_trace"})

    # Execute a few instructions to populate trace buffer
    client.call_tool("write_memory", {"machine_id": "raw6502", "addr": 0x1000, "bytes": [0xEA, 0xEA, 0xEA]})  # Three NOPs
    client.call_tool("set_pc", {"machine_id": "raw6502", "addr": 0x1000})
    client.call_tool("step_cpu", {"machine_id": "raw6502", "count": 3})

    # Get trace buffer
    res = client.call_tool("get_trace_buffer", {"machine_id": "raw6502"})
    trace_text = res["result"]["content"][0]["text"]
    assert "Trace buffer" in trace_text, "Trace buffer response missing header"
    assert "entries" in trace_text, "Trace buffer response missing entry count"
    print("get_trace_buffer OK")

    # Test limit parameter
    res = client.call_tool("get_trace_buffer", {"machine_id": "raw6502", "limit": 2})
    trace_text = res["result"]["content"][0]["text"]
    assert "Showing" in trace_text, "Limit parameter response incorrect"
    print("get_trace_buffer with limit OK")

    # Set trace filter
    res = client.call_tool("set_trace_filter", {"machine_id": "raw6502", "filter": "instructions"})
    assert "instructions" in res["result"]["content"][0]["text"], "Filter not set"
    print("set_trace_filter OK")

    # Test invalid filter
    res = client.call_tool("set_trace_filter", {"machine_id": "raw6502", "filter": "INVALID"})
    assert "Error" in res["result"]["content"][0]["text"], "Should error on invalid filter"
    print("set_trace_filter error handling OK")

    # Clear trace
    res = client.call_tool("clear_trace", {"machine_id": "raw6502"})
    assert "cleared" in res["result"]["content"][0]["text"], "Clear trace response incorrect"
    res = client.call_tool("get_trace_buffer", {"machine_id": "raw6502"})
    trace_text = res["result"]["content"][0]["text"]
    assert "0 entries" in trace_text, "Trace buffer should be empty after clear"
    print("clear_trace OK")

    print("Trace buffer operations OK")

    client.close()
    print("\nALL MCP TESTS PASSED")

if __name__ == "__main__":
    try:
        run_tests()
    except Exception as e:
        print(f"TEST FAILED: {e}")
        sys.exit(1)
