#!/usr/bin/env python3
"""
Variable inspector tool - Inspect variables in functions with debug metadata.

Usage:
    python3 variable_inspector.py [--host localhost] [--port 6502] [--function main] [--read]
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from mmemu_client import MMemuClient, format_addr


def format_value(data: bytes, type_name: str) -> str:
    """Format bytes as typed value."""
    if not data:
        return "(empty)"

    if type_name in ("int8", "I8", "char"):
        val = data[0] if data[0] < 128 else data[0] - 256
        return f"{val} (0x{data[0]:02X})"

    elif type_name in ("int16", "I16", "int"):
        if len(data) >= 2:
            val = data[0] | (data[1] << 8)  # Little-endian
            if val > 32767:
                val -= 65536
            return f"{val} (0x{val:04X})"

    elif type_name in ("int32", "I32", "long"):
        if len(data) >= 4:
            val = (
                data[0]
                | (data[1] << 8)
                | (data[2] << 16)
                | (data[3] << 24)
            )
            if val > 2147483647:
                val -= 4294967296
            return f"{val} (0x{val:08X})"

    else:
        # Default hex dump
        return " ".join(f"{b:02X}" for b in data)


def list_variables(client, function_name=None):
    """List variables for a function."""
    try:
        vars_list = client.list_variables(function_name)

        if not vars_list:
            if function_name:
                print(f"No variables found in function '{function_name}'")
            else:
                print("No global variables found")
            return

        # Sort by offset
        vars_list.sort(key=lambda v: v.get("offset", 0))

        print(f"\n{'Name':<20} {'Offset':<10} {'Size':<6} {'Type':<12} {'Scope':<10}")
        print("-" * 60)

        for var in vars_list:
            name = var.get("name", "?")[:19]
            offset = format_addr(var.get("offset", 0), 4)
            size = var.get("size", 0)
            type_name = var.get("type", "?")[:11]
            scope = var.get("scope", "?")[:9]

            print(f"{name:<20} {offset:<10} {size:<6} {type_name:<12} {scope:<10}")

    except Exception as e:
        print(f"Error listing variables: {e}")


def inspect_variable(client, function_name, var_name, read_value=False):
    """Inspect a specific variable."""
    try:
        vars_list = client.list_variables(function_name)

        # Find the variable
        target = None
        for var in vars_list:
            if var.get("name") == var_name:
                target = var
                break

        if not target:
            print(f"Variable '{var_name}' not found in function '{function_name}'")
            return

        print(f"\nVariable: {var_name}")
        print(f"  Offset: {format_addr(target['offset'], 4)}")
        print(f"  Size:   {target['size']} bytes")
        print(f"  Type:   {target['type']}")
        print(f"  Scope:  {target['scope']}")

        if read_value:
            try:
                data = client.read_memory(target["offset"], target["size"])
                value = format_value(data, target["type"])
                print(f"  Value:  {value}")
            except Exception as e:
                print(f"  (Error reading value: {e})")

    except Exception as e:
        print(f"Error: {e}")


def main():
    parser = argparse.ArgumentParser(description="Variable inspector tool")
    parser.add_argument(
        "--host", default="localhost", help="Server hostname (default: localhost)"
    )
    parser.add_argument(
        "--port", type=int, default=6502, help="Server port (default: 6502)"
    )
    parser.add_argument(
        "--function", help="Function to inspect (default: list all functions)"
    )
    parser.add_argument(
        "--variable", help="Specific variable to inspect"
    )
    parser.add_argument(
        "--read",
        action="store_true",
        help="Read variable values from memory",
    )
    parser.add_argument(
        "--interactive",
        action="store_true",
        help="Interactive mode",
    )

    args = parser.parse_args()

    try:
        client = MMemuClient(args.host, args.port)
    except ConnectionError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    try:
        if args.interactive:
            print("Variable Inspector - Interactive Mode")
            print("Type 'help' for commands\n")

            while True:
                try:
                    cmd = input("> ").strip()
                    if not cmd:
                        continue

                    if cmd == "help":
                        print(
                            """
Commands:
  list [function]      - List variables in function (or all)
  inspect <func> <var> - Inspect specific variable
  read <func> <var>    - Read and display variable value
  quit/exit            - Exit
"""
                        )
                    elif cmd.startswith("list"):
                        parts = cmd.split()
                        func = parts[1] if len(parts) > 1 else None
                        list_variables(client, func)

                    elif cmd.startswith("inspect"):
                        parts = cmd.split()
                        if len(parts) >= 3:
                            inspect_variable(client, parts[1], parts[2], read_value=False)
                        else:
                            print("Usage: inspect <function> <variable>")

                    elif cmd.startswith("read"):
                        parts = cmd.split()
                        if len(parts) >= 3:
                            inspect_variable(client, parts[1], parts[2], read_value=True)
                        else:
                            print("Usage: read <function> <variable>")

                    elif cmd in ("quit", "exit"):
                        print("Goodbye!")
                        break

                    else:
                        print(f"Unknown command: {cmd}")

                except KeyboardInterrupt:
                    print("\nInterrupted")
                    break
                except Exception as e:
                    print(f"Error: {e}")

        else:
            if args.variable:
                # Inspect specific variable
                if not args.function:
                    print("Error: --function required when inspecting a variable")
                    return 1
                inspect_variable(client, args.function, args.variable, args.read)
            else:
                # List variables
                list_variables(client, args.function)

        return 0

    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())
