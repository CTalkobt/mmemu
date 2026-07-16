#!/usr/bin/env python3
"""
Breakpoint Manager Tool

Manages breakpoints and watches in MMEMU.

Usage:
    python3 breakpoint_manager.py [--host localhost] [--port 2000] <action> [args]

Actions:
    list                      - List active breakpoints
    add <addr> [addr] ...     - Add breakpoints at addresses
    clear                     - Clear all breakpoints
    check <addr>              - Check if breakpoint exists at address
    trace <on|off>            - Enable/disable instruction trace
    dump-trace                - Show trace buffer
"""

import sys
import argparse
from pathlib import Path

# Add SDK to path
sdk_path = Path(__file__).parent.parent
sys.path.insert(0, str(sdk_path))

from mmemu_serial_monitor import SerialMonitor, SerialMonitorException


class BreakpointManager:
    """Manage breakpoints and trace."""

    def __init__(self, host: str, port: int):
        self.mm = SerialMonitor(host, port)
        self.breakpoints = []

    def connect(self):
        """Connect to MMEMU."""
        self.mm.connect()

    def disconnect(self):
        """Disconnect from MMEMU."""
        self.mm.disconnect()

    def add_breakpoint(self, addr: int) -> bool:
        """Add breakpoint at address."""
        try:
            self.mm.set_breakpoint(addr)
            self.breakpoints.append(addr)
            print(f"✓ Breakpoint added at ${addr:06X}")
            return True
        except SerialMonitorException as e:
            print(f"✗ Error: {e}")
            return False

    def clear_breakpoints(self) -> bool:
        """Clear all breakpoints."""
        try:
            self.mm.clear_breakpoints()
            self.breakpoints.clear()
            print("✓ All breakpoints cleared")
            return True
        except SerialMonitorException as e:
            print(f"✗ Error: {e}")
            return False

    def list_breakpoints(self) -> bool:
        """List active breakpoints."""
        if not self.breakpoints:
            print("No active breakpoints")
            return True

        print(f"Active breakpoints ({len(self.breakpoints)}):")
        for i, addr in enumerate(self.breakpoints, 1):
            print(f"  {i}. ${addr:06X}")
        return True

    def check_breakpoint(self, addr: int) -> bool:
        """Check if breakpoint exists at address."""
        if addr in self.breakpoints:
            print(f"✓ Breakpoint exists at ${addr:06X}")
            return True
        else:
            print(f"✗ No breakpoint at ${addr:06X}")
            return False

    def enable_trace(self) -> bool:
        """Enable instruction trace."""
        try:
            self.mm.enable_trace()
            print("✓ Trace enabled")
            return True
        except SerialMonitorException as e:
            print(f"✗ Error: {e}")
            return False

    def disable_trace(self) -> bool:
        """Disable instruction trace."""
        try:
            self.mm.disable_trace()
            print("✓ Trace disabled")
            return True
        except SerialMonitorException as e:
            print(f"✗ Error: {e}")
            return False

    def dump_trace(self) -> bool:
        """Dump trace buffer."""
        try:
            dump = self.mm.get_trace_dump()
            print(dump)
            return True
        except SerialMonitorException as e:
            print(f"✗ Error: {e}")
            return False

    def show_cpu_state(self) -> bool:
        """Show current CPU state."""
        try:
            regs = self.mm.read_registers()
            print("Current CPU state:")
            for key in ['PC', 'A', 'X', 'Y', 'SP', 'P']:
                if key in regs:
                    val = regs[key]
                    if key == 'PC':
                        print(f"  {key} = ${val:06X}")
                    else:
                        print(f"  {key} = ${val:02X}")
            return True
        except SerialMonitorException as e:
            print(f"✗ Error: {e}")
            return False


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument('--host', default='localhost', help='MMEMU host')
    parser.add_argument('--port', type=int, default=2000, help='MMEMU port')
    parser.add_argument('action', nargs='?', default='list', help='Action to perform')
    parser.add_argument('args', nargs='*', help='Arguments for action')

    args = parser.parse_args()

    try:
        mgr = BreakpointManager(args.host, args.port)
        mgr.connect()
        print(f"✓ Connected to {args.host}:{args.port}\n")

        # Show current CPU state
        mgr.show_cpu_state()
        print()

        # Dispatch action
        action = args.action.lower()

        if action == 'list':
            mgr.list_breakpoints()

        elif action == 'add':
            if not args.args:
                print("Usage: add <addr> [addr] ...")
                sys.exit(1)
            for addr_str in args.args:
                try:
                    addr = int(addr_str, 16)
                    mgr.add_breakpoint(addr)
                except ValueError:
                    print(f"✗ Invalid address: {addr_str}")

        elif action == 'clear':
            mgr.clear_breakpoints()

        elif action == 'check':
            if not args.args:
                print("Usage: check <addr>")
                sys.exit(1)
            try:
                addr = int(args.args[0], 16)
                mgr.check_breakpoint(addr)
            except ValueError:
                print(f"✗ Invalid address: {args.args[0]}")

        elif action == 'trace':
            if not args.args:
                print("Usage: trace <on|off>")
                sys.exit(1)
            if args.args[0].lower() == 'on':
                mgr.enable_trace()
            elif args.args[0].lower() == 'off':
                mgr.disable_trace()
            else:
                print(f"✗ Unknown trace action: {args.args[0]}")

        elif action == 'dump-trace':
            mgr.dump_trace()

        else:
            print(f"✗ Unknown action: {action}")
            sys.exit(1)

        mgr.disconnect()

    except Exception as e:
        print(f"Fatal error: {e}")
        sys.exit(1)


if __name__ == '__main__':
    main()
