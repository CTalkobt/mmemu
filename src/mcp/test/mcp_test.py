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

    # --- 10. analyze_routine Tool ---
    print("\n--- 10. analyze_routine Tool ---")

    # Create a raw6502 machine for analysis tests
    res = client.call_tool("create_machine", {"machine_type": "raw6502"})
    ar_mid = res["result"]["content"][0]["text"].split('"')[1]

    # Write a program with branches, loops, JSR calls, I/O, and RTS
    # $0200: JSR $020A       ; call INIT_SCREEN
    # $0203: JSR $020F       ; call WAIT_RASTER
    # $0206: STA $D020       ; write to BORDER
    # $0209: RTS
    # $020A: LDA #$2A        ; INIT_SCREEN
    # $020C: STA $02
    # $020E: RTS
    # $020F: LDA $D012       ; WAIT_RASTER
    # $0212: CMP #$80
    # $0214: BNE $020F       ; loop until raster = $80
    # $0216: RTS
    program = [
        0x20, 0x0A, 0x02,  # JSR $020A
        0x20, 0x0F, 0x02,  # JSR $020F
        0x8D, 0x20, 0xD0,  # STA $D020
        0x60,              # RTS
        0xA9, 0x2A,        # LDA #$2A  (INIT_SCREEN)
        0x85, 0x02,        # STA $02
        0x60,              # RTS
        0xAD, 0x12, 0xD0,  # LDA $D012 (WAIT_RASTER)
        0xC9, 0x80,        # CMP #$80
        0xD0, 0xF9,        # BNE $020F (-7)
        0x60               # RTS
    ]
    client.call_tool("write_memory", {
        "machine_id": ar_mid, "addr": 0x0200, "bytes": program
    })

    # Add symbols for annotation
    client.call_tool("add_symbol", {"machine_id": ar_mid, "label": "MAIN", "addr": "$0200"})
    client.call_tool("add_symbol", {"machine_id": ar_mid, "label": "INIT_SCREEN", "addr": "$020A"})
    client.call_tool("add_symbol", {"machine_id": ar_mid, "label": "WAIT_RASTER", "addr": "$020F"})

    # Analyze the routine
    res = client.call_tool("analyze_routine", {
        "machine_id": ar_mid, "addr": "$0200"
    })
    text = res["result"]["content"][0]["text"]

    # Verify report structure
    assert "Routine Analysis" in text, f"Missing header: {text[:100]}"
    assert "MAIN" in text, f"Missing entry label: {text[:200]}"
    print("  ✓ analyze_routine header + entry label OK")

    # Verify call detection
    assert "Calls" in text and "JSR $020A" in text, f"Missing call: {text}"
    print("  ✓ Call detection OK")

    # Verify I/O access detection (non-recursive only sees MAIN's STA $D020)
    assert "I/O" in text and "D020" in text.upper(), f"Missing I/O write: {text}"
    print("  ✓ I/O access detection OK")

    # Verify exit detection
    assert "RTS" in text, f"Missing exit: {text}"
    print("  ✓ Exit detection OK")

    # Verify control flow summary
    assert "Branches:" in text and "Calls:" in text, f"Missing control flow: {text}"
    assert "Max call depth: 0" in text, f"Non-recursive should have depth 0: {text}"
    print("  ✓ Control flow summary + depth OK")

    # Test recursive mode — should follow into subroutines
    res = client.call_tool("analyze_routine", {
        "machine_id": ar_mid, "addr": "$0200", "recursive": True
    })
    rtext = res["result"]["content"][0]["text"]
    assert "[recursive]" in rtext, f"Missing recursive indicator: {rtext[:200]}"
    assert "Max call depth: 1" in rtext, f"Recursive depth should be 1: {rtext}"
    # Recursive should find more instructions than non-recursive
    # Extract instruction count from both reports
    import re
    non_rec_insns = int(re.search(r'(\d+) instructions', text).group(1))
    rec_insns = int(re.search(r'(\d+) instructions', rtext).group(1))
    assert rec_insns > non_rec_insns, \
        f"Recursive ({rec_insns}) should have more instructions than non-recursive ({non_rec_insns})"
    # Recursive should find WAIT_RASTER's loop and I/O read
    assert "Loops" in rtext and "020F" in rtext.upper(), \
        f"Recursive should find WAIT_RASTER loop: {rtext}"
    assert "D012" in rtext.upper(), \
        f"Recursive should find I/O read $D012: {rtext}"
    print(f"  ✓ Recursive mode OK ({non_rec_insns} -> {rec_insns} instructions, depth 1)")

    # Test error: invalid machine
    res = client.call_tool("analyze_routine", {
        "machine_id": "nonexistent", "addr": "$0200"
    })
    assert "Error" in res["result"]["content"][0]["text"]
    print("  ✓ Error handling OK")

    client.call_tool("destroy_machine", {"machine_id": ar_mid})

    # --- 11. generate_tests Tool ---
    print("\n--- 11. generate_tests Tool ---")

    res = client.call_tool("create_machine", {"machine_type": "raw6502"})
    gt_mid = res["result"]["content"][0]["text"].split('"')[1]

    # Write "double A" routine: ASL A; RTS
    client.call_tool("write_memory", {
        "machine_id": gt_mid, "addr": 0x0200, "bytes": [0x0A, 0x60]
    })
    client.call_tool("add_symbol", {"machine_id": gt_mid, "label": "DOUBLE", "addr": "$0200"})

    # Test with default values
    res = client.call_tool("generate_tests", {
        "machine_id": gt_mid, "addr": "$0200",
        "input_regs": ["A"], "output_regs": ["A", "P"]
    })
    text = res["result"]["content"][0]["text"]
    assert "Test Vectors" in text and "DOUBLE" in text, f"Missing header: {text[:200]}"
    assert "6 tests" in text, f"Expected 6 default tests: {text[:200]}"
    # Verify $01 -> $02 (ASL doubles it)
    assert "$02" in text, f"Expected $01 -> $02 in output: {text}"
    # All should complete
    assert text.count("| Y") >= 6, f"All tests should complete: {text}"
    print("  ✓ generate_tests basic OK (6 tests, ASL A)")

    # Test with two input registers and custom values
    # Write A+X routine: STA $02; TXA; ADC $02; RTS
    client.call_tool("write_memory", {
        "machine_id": gt_mid, "addr": 0x0300,
        "bytes": [0x85, 0x02, 0x8A, 0x65, 0x02, 0x60]
    })
    res = client.call_tool("generate_tests", {
        "machine_id": gt_mid, "addr": "$0300",
        "input_regs": ["A", "X"], "output_regs": ["A"],
        "values": [0, 1, 255]
    })
    text = res["result"]["content"][0]["text"]
    assert "9 tests" in text, f"Expected 3x3=9 tests: {text[:200]}"
    assert "A(in)" in text and "X(in)" in text, f"Missing input columns: {text[:300]}"
    print("  ✓ generate_tests multi-input OK (9 tests, A+X)")

    # Test with subroutine call: the routine calls a sub and returns
    # Write: JSR $0400; RTS at $0350, and LDA #$42; RTS at $0400
    client.call_tool("write_memory", {
        "machine_id": gt_mid, "addr": 0x0350,
        "bytes": [0x20, 0x00, 0x04, 0x60]  # JSR $0400; RTS
    })
    client.call_tool("write_memory", {
        "machine_id": gt_mid, "addr": 0x0400,
        "bytes": [0xA9, 0x42, 0x60]  # LDA #$42; RTS
    })
    res = client.call_tool("generate_tests", {
        "machine_id": gt_mid, "addr": "$0350",
        "input_regs": ["A"], "output_regs": ["A"],
        "values": [0, 255]
    })
    text = res["result"]["content"][0]["text"]
    # Both tests should output A=$42 regardless of input
    lines = [l for l in text.split('\n') if '| Y' in l]
    assert len(lines) == 2, f"Expected 2 completed tests: {text}"
    for line in lines:
        assert "$42" in line, f"Subroutine should set A=$42: {line}"
    print("  ✓ generate_tests with nested JSR OK")

    # Test error: bad register name
    res = client.call_tool("generate_tests", {
        "machine_id": gt_mid, "addr": "$0200",
        "input_regs": ["NONEXISTENT"]
    })
    assert "Error" in res["result"]["content"][0]["text"]
    print("  ✓ generate_tests bad register error OK")

    client.call_tool("destroy_machine", {"machine_id": gt_mid})

    client.close()
    print("\n" + "="*60)
    print("ALL MCP TESTS PASSED")
    print("="*60)
    print(f"Successfully tested {len(machines)} concurrent machine instances")
    print("with full test suite, diff_file, snapshot, analyze_routine, and generate_tests")

if __name__ == "__main__":
    try:
        run_tests()
    except Exception as e:
        print(f"TEST FAILED: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
