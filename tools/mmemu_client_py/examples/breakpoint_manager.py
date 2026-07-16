#!/usr/bin/env python3
"""
Interactive breakpoint manager for mmemu.

Usage:
    python3 breakpoint_manager.py [--host localhost] [--port 6502]
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from mmemu_client import MMemuClient, parse_addr, format_addr


def print_header():
    """Print help header."""
    print(
        """
Breakpoint Manager
Commands:
  list            - List all breakpoints
  set <addr>      - Set breakpoint at address
  clear <addr>    - Clear breakpoint
  clearall        - Clear all breakpoints
  status          - Show current CPU status
  quit/exit       - Exit

Address formats: $2000, 0x2000, 8192
"""
    )


def print_breakpoints(client):
    """Display current breakpoints."""
    bps = client.list_breakpoints()
    if not bps:
        print("No breakpoints set")
        return

    print(f"\n{'Address':<10} {'Status':<12}")
    print("-" * 22)
    for bp in bps:
        status = "enabled" if bp.get("enabled", True) else "disabled"
        print(f"{format_addr(bp['addr']):<10} {status:<12}")


def show_status(client):
    """Display current CPU status."""
    try:
        regs = client.read_registers()
        print(f"\nCPU Status:")
        print(f"  PC = {format_addr(regs.get('PC', 0), 4)}")
        print(f"  A  = ${regs.get('A', 0):02X}")
        print(f"  X  = ${regs.get('X', 0):02X}")
        print(f"  Y  = ${regs.get('Y', 0):02X}")
        print(f"  SP = {format_addr(regs.get('SP', 0), 4)}")
        print(f"  P  = ${regs.get('P', 0):02X}")
    except Exception as e:
        print(f"Error reading status: {e}")


def main():
    parser = argparse.ArgumentParser(description="Interactive breakpoint manager")
    parser.add_argument(
        "--host", default="localhost", help="Server hostname (default: localhost)"
    )
    parser.add_argument(
        "--port", type=int, default=6502, help="Server port (default: 6502)"
    )

    args = parser.parse_args()

    try:
        client = MMemuClient(args.host, args.port)
    except ConnectionError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    print_header()

    try:
        while True:
            try:
                cmd = input("> ").strip()

                if not cmd:
                    continue

                parts = cmd.split()
                command = parts[0].lower()

                if command in ("quit", "exit"):
                    print("Goodbye!")
                    break

                elif command == "list":
                    print_breakpoints(client)

                elif command == "set":
                    if len(parts) < 2:
                        print("Usage: set <address>")
                        continue
                    try:
                        addr = parse_addr(parts[1])
                        if client.set_breakpoint(addr):
                            print(f"Breakpoint set at {format_addr(addr, 4)}")
                        else:
                            print("Failed to set breakpoint")
                    except ValueError:
                        print(f"Invalid address: {parts[1]}")

                elif command == "clear":
                    if len(parts) < 2:
                        print("Usage: clear <address>")
                        continue
                    try:
                        addr = parse_addr(parts[1])
                        # Note: clear not implemented in serial monitor yet
                        print(f"Clear would remove breakpoint at {format_addr(addr, 4)}")
                        print("(Clear breakpoint command not yet implemented)")
                    except ValueError:
                        print(f"Invalid address: {parts[1]}")

                elif command == "clearall":
                    print("Clearall not yet implemented in serial monitor")

                elif command == "status":
                    show_status(client)

                elif command == "help":
                    print_header()

                else:
                    print(f"Unknown command: {command}")

            except KeyboardInterrupt:
                print("\nInterrupted")
                break
            except Exception as e:
                print(f"Error: {e}")

    finally:
        client.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
