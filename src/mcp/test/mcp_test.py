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

def run_tests_on_machine(client, machine_id, machine_name):
    """Run a comprehensive test suite on a specific machine instance"""
    print(f"\n  --- Testing {machine_name} ({machine_id}) ---")

    # Memory Operations
    client.call_tool("write_memory", {
        "machine_id": machine_id,
        "addr": 0x1000,
        "bytes": [0x11, 0x22, 0x33, 0x44]
    })
    res = client.call_tool("read_memory", {
        "machine_id": machine_id,
        "addr": 0x1000,
        "size": 4
    })
    assert "11 22 33 44" in res["result"]["content"][0]["text"], f"Memory read failed for {machine_id}"
    print(f"    ✓ Memory read/write OK")

    # Fill memory - use safe RAM address that works on all machines (0x1000 is RAM on both C64 and VIC-20)
    client.call_tool("fill_memory", {
        "machine_id": machine_id,
        "addr": 0x1100,
        "value": 0xCD,
        "size": 10
    })
    res = client.call_tool("read_memory", {
        "machine_id": machine_id,
        "addr": 0x1100,
        "size": 10
    })
    dump_text = res["result"]["content"][0]["text"].lower()
    # Check for pattern of CD bytes
    assert dump_text.count("cd") >= 8, f"Expected at least 8 'cd' bytes in fill test, got: {dump_text}"
    print(f"    ✓ Memory fill OK")

    # Search
    res = client.call_tool("search_memory", {
        "machine_id": machine_id,
        "pattern": "11 22 33",
        "is_hex": True
    })
    assert "Found at $1000" in res["result"]["content"][0]["text"]
    print(f"    ✓ Memory search OK")

    # CPU Operations
    client.call_tool("set_pc", {"machine_id": machine_id, "addr": 0x1000})
    client.call_tool("write_memory", {"machine_id": machine_id, "addr": 0x1000, "bytes": [0xEA]})  # NOP
    client.call_tool("step_cpu", {"machine_id": machine_id, "count": 1})
    res = client.call_tool("read_registers", {"machine_id": machine_id})
    reg_text = res["result"]["content"][0]["text"]
    assert "PC: $1001" in reg_text
    print(f"    ✓ CPU operations OK")

    # Breakpoints
    res = client.call_tool("set_breakpoint", {"machine_id": machine_id, "addr": 0x1005})
    bp_text = res["result"]["content"][0]["text"]
    bp_id = int(bp_text.split()[1])

    res = client.call_tool("list_breakpoints", {"machine_id": machine_id})
    bp_list = res["result"]["content"][0]["text"]
    assert "1005" in bp_list.lower()

    client.call_tool("delete_breakpoint", {"machine_id": machine_id, "id": bp_id})
    res = client.call_tool("list_breakpoints", {"machine_id": machine_id})
    assert "No breakpoints set" in res["result"]["content"][0]["text"]
    print(f"    ✓ Breakpoints OK")

    # Symbols (add, list, remove)
    client.call_tool("add_symbol", {"machine_id": machine_id, "label": "test_label", "addr": "$1234"})
    res = client.call_tool("list_symbols", {"machine_id": machine_id})
    assert "test_label" in res["result"]["content"][0]["text"]
    assert "1234" in res["result"]["content"][0]["text"]

    client.call_tool("remove_symbol", {"machine_id": machine_id, "label": "test_label"})
    res = client.call_tool("list_symbols", {"machine_id": machine_id})
    assert "test_label" not in res["result"]["content"][0]["text"]
    print(f"    ✓ Symbol management OK")

    # Trace Buffer
    client.call_tool("write_memory", {"machine_id": machine_id, "addr": 0x1000, "bytes": [0xEA, 0xEA, 0xEA]})
    client.call_tool("set_pc", {"machine_id": machine_id, "addr": 0x1000})
    client.call_tool("step_cpu", {"machine_id": machine_id, "count": 3})

    res = client.call_tool("get_trace_buffer", {"machine_id": machine_id})
    assert "Trace buffer" in res["result"]["content"][0]["text"]

    client.call_tool("clear_trace", {"machine_id": machine_id})
    res = client.call_tool("get_trace_buffer", {"machine_id": machine_id})
    assert "0 entries" in res["result"]["content"][0]["text"]
    print(f"    ✓ Trace buffer OK")

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

    print("\n--- 2. Multi-Instance Machine Management ---")
    # Create multiple machine instances simultaneously
    print("Creating 3 machine instances...")
    machines = []

    # Create C64 instance
    res = client.call_tool("create_machine", {"machine_type": "c64"})
    c64_id = res["result"]["content"][0]["text"].split('"')[1]
    machines.append(("c64", c64_id, "Commodore 64"))
    print(f"  ✓ Created C64: {c64_id}")

    # Create VIC-20 instance
    res = client.call_tool("create_machine", {"machine_type": "vic20"})
    vic20_id = res["result"]["content"][0]["text"].split('"')[1]
    machines.append(("vic20", vic20_id, "Commodore VIC-20"))
    print(f"  ✓ Created VIC-20: {vic20_id}")

    # Create another C64 instance (to test multiple instances of same type)
    res = client.call_tool("create_machine", {"machine_type": "c64"})
    c64_2_id = res["result"]["content"][0]["text"].split('"')[1]
    machines.append(("c64_2", c64_2_id, "Commodore 64 #2"))
    print(f"  ✓ Created C64 #2: {c64_2_id}")

    print(f"\n  Total machines running concurrently: {len(machines)}")

    print("\n--- 3. List Machine Instances ---")
    res = client.call_tool("list_instances", {})
    instances_text = res["result"]["content"][0]["text"]
    print("Current instances:")
    print(instances_text)

    # Verify all instances are listed
    for _, machine_id, name in machines:
        assert machine_id in instances_text, f"Machine {machine_id} not found in list"
    print(f"  ✓ All {len(machines)} instances listed correctly")

    print("\n--- 4. Parallel Test Suite Execution ---")
    print("Running comprehensive test suite on all instances...")

    # Run the same test suite on each machine
    for machine_type, machine_id, machine_name in machines:
        run_tests_on_machine(client, machine_id, machine_name)

    print("\n  ✓ All machines tested successfully")

    print("\n--- 5. Verify Instance Independence ---")
    # Write different data to each machine at address 0x1000 (which works on both from earlier tests)
    print("Testing instance data isolation...")
    client.call_tool("write_memory", {
        "machine_id": machines[0][1],  # C64
        "addr": 0x1200,
        "bytes": [0xAA, 0xBB, 0xCC]
    })
    client.call_tool("write_memory", {
        "machine_id": machines[1][1],  # VIC-20
        "addr": 0x1200,
        "bytes": [0xDD, 0xEE, 0xFF]
    })

    # Verify C64 still has its data
    res = client.call_tool("read_memory", {
        "machine_id": machines[0][1],
        "addr": 0x1200,
        "size": 3
    })
    c64_memory = res["result"]["content"][0]["text"].lower()
    print(f"    C64 memory at 0x1200: {c64_memory}")
    assert "aa bb cc" in c64_memory, f"C64 isolation failed: got {c64_memory}"

    # Verify VIC-20 has different data
    res = client.call_tool("read_memory", {
        "machine_id": machines[1][1],
        "addr": 0x1200,
        "size": 3
    })
    vic20_memory = res["result"]["content"][0]["text"].lower()
    print(f"    VIC-20 memory at 0x1200: {vic20_memory}")
    assert "dd ee ff" in vic20_memory, f"VIC-20 isolation failed: got {vic20_memory}"
    print("  ✓ Instance data isolation verified")

    print("\n--- 6. Machine Instance Destruction ---")
    print("Destroying machine instances...")

    for idx, (_, machine_id, name) in enumerate(machines, 1):
        res = client.call_tool("destroy_machine", {"machine_id": machine_id})
        assert "Destroyed" in res["result"]["content"][0]["text"]
        print(f"  ✓ Destroyed {name} ({machine_id})")

    # Verify all instances are gone
    res = client.call_tool("list_instances", {})
    instances_text = res["result"]["content"][0]["text"]

    for _, machine_id, name in machines:
        assert machine_id not in instances_text, f"Machine {machine_id} still listed after destruction"

    print(f"  ✓ All {len(machines)} instances destroyed successfully")

    print("\n--- 7. Error Handling with Destroyed Instances ---")
    # Verify operations fail on destroyed instances
    res = client.call_tool("read_registers", {"machine_id": machines[0][1]})
    assert "Error" in res["result"]["content"][0]["text"]
    print("  ✓ Operations correctly fail on destroyed instances")

    # --- 8. diff_file Tool ---
    print("\n--- 8. diff_file Tool ---")
    import tempfile, os

    # Create two test files
    file_a = tempfile.NamedTemporaryFile(delete=False, suffix=".bin")
    file_b = tempfile.NamedTemporaryFile(delete=False, suffix=".bin")
    try:
        data_a = bytearray(256)
        data_b = bytearray(256)
        # Make them differ at offset 0x10-0x13
        for i in range(4):
            data_a[0x10 + i] = 0xAA
            data_b[0x10 + i] = 0xBB
        # Add vectors at end (NMI=$E000, RESET=$E100, IRQ=$E200)
        import struct
        data_a[0xFA:] = struct.pack('<HHH', 0xE000, 0xE100, 0xE200)
        data_b[0xFA:] = struct.pack('<HHH', 0xE000, 0xE150, 0xE200)
        file_a.write(data_a)
        file_b.write(data_b)
        file_a.close()
        file_b.close()

        # Test diff with differences
        res = client.call_tool("diff_file", {
            "file_a": file_a.name,
            "file_b": file_b.name,
            "base_addr": "$FF00"
        })
        diff_text = res["result"]["content"][0]["text"]
        assert "Changed: 5 / 256 bytes" in diff_text, f"Diff summary wrong: {diff_text[:200]}"
        assert "RESET" in diff_text and "CHANGED" in diff_text, f"Vector diff missing: {diff_text[:400]}"
        print("  ✓ diff_file with differences OK")

        # Test diff identical files
        res = client.call_tool("diff_file", {
            "file_a": file_a.name,
            "file_b": file_a.name
        })
        diff_text = res["result"]["content"][0]["text"]
        assert "Files are identical" in diff_text, f"Identical diff failed: {diff_text[:200]}"
        print("  ✓ diff_file identical files OK")

        # Test diff missing file
        res = client.call_tool("diff_file", {
            "file_a": "/nonexistent/path.bin",
            "file_b": file_b.name
        })
        err_text = res["result"]["content"][0]["text"]
        assert "Error" in err_text, f"Missing file should error: {err_text[:200]}"
        print("  ✓ diff_file missing file error OK")
    finally:
        os.unlink(file_a.name)
        os.unlink(file_b.name)

    # --- 9. Snapshot Tools ---
    print("\n--- 9. Snapshot Tools ---")

    # Create a fresh machine for snapshot tests
    res = client.call_tool("create_machine", {"machine_type": "raw6502"})
    snap_mid = res["result"]["content"][0]["text"].split('"')[1]
    print(f"  Created {snap_mid} for snapshot tests")

    # Set up: write a small program at $0200 and set PC there
    client.call_tool("fill_memory", {
        "machine_id": snap_mid, "addr": "$0200", "size": "16", "value": "$EA"  # NOP sled
    })
    client.call_tool("set_pc", {"machine_id": snap_mid, "addr": "$0200"})

    # snapshot_save — take "before" snapshot
    res = client.call_tool("snapshot_save", {
        "machine_id": snap_mid, "name": "before", "range": "$0000-$02FF"
    })
    save_text = res["result"]["content"][0]["text"]
    assert "before" in save_text and "registers" in save_text, f"Save failed: {save_text}"
    print("  ✓ snapshot_save OK")

    # Modify state: fill memory and step CPU
    client.call_tool("fill_memory", {
        "machine_id": snap_mid, "addr": "$0080", "size": "4", "value": "$42"
    })
    client.call_tool("step_cpu", {"machine_id": snap_mid, "count": 3})

    # snapshot_save — take "after" snapshot
    res = client.call_tool("snapshot_save", {
        "machine_id": snap_mid, "name": "after", "range": "$0000-$02FF"
    })
    save_text = res["result"]["content"][0]["text"]
    assert "after" in save_text, f"Save after failed: {save_text}"
    print("  ✓ snapshot_save (after changes) OK")

    # snapshot_list
    res = client.call_tool("snapshot_list", {"machine_id": snap_mid})
    list_text = res["result"]["content"][0]["text"]
    assert "before" in list_text and "after" in list_text, f"List failed: {list_text}"
    print("  ✓ snapshot_list OK")

    # snapshot_diff — should show register and memory changes
    res = client.call_tool("snapshot_diff", {
        "machine_id": snap_mid, "snapshot_a": "before", "snapshot_b": "after"
    })
    diff_text = res["result"]["content"][0]["text"]
    assert "Registers" in diff_text, f"Diff missing registers section: {diff_text[:200]}"
    assert "Memory" in diff_text, f"Diff missing memory section: {diff_text[:200]}"
    assert "PC:" in diff_text, f"Diff should show PC change: {diff_text[:400]}"
    assert "$0080" in diff_text, f"Diff should show memory change at $0080: {diff_text[:400]}"
    print("  ✓ snapshot_diff OK (registers + memory)")

    # snapshot_diff — error on missing snapshot
    res = client.call_tool("snapshot_diff", {
        "machine_id": snap_mid, "snapshot_a": "before", "snapshot_b": "nonexistent"
    })
    err_text = res["result"]["content"][0]["text"]
    assert "Error" in err_text, f"Missing snapshot should error: {err_text}"
    print("  ✓ snapshot_diff missing snapshot error OK")

    # snapshot_delete — delete one
    res = client.call_tool("snapshot_delete", {"machine_id": snap_mid, "name": "before"})
    assert "Deleted" in res["result"]["content"][0]["text"]
    res = client.call_tool("snapshot_list", {"machine_id": snap_mid})
    list_text = res["result"]["content"][0]["text"]
    assert "before" not in list_text and "after" in list_text
    print("  ✓ snapshot_delete (single) OK")

    # snapshot_delete — delete all
    res = client.call_tool("snapshot_delete", {"machine_id": snap_mid, "name": "*"})
    assert "Deleted" in res["result"]["content"][0]["text"]
    res = client.call_tool("snapshot_list", {"machine_id": snap_mid})
    assert "No snapshots" in res["result"]["content"][0]["text"]
    print("  ✓ snapshot_delete (all) OK")

    # Cleanup
    client.call_tool("destroy_machine", {"machine_id": snap_mid})

    client.close()
    print("\n" + "="*60)
    print("ALL MCP TESTS PASSED")
    print("="*60)
    print(f"Successfully tested {len(machines)} concurrent machine instances")
    print("with full test suite, diff_file, and snapshot tools")

if __name__ == "__main__":
    try:
        run_tests()
    except Exception as e:
        print(f"TEST FAILED: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
