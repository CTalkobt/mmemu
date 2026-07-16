#!/usr/bin/env python3
"""
Memory Inspector Tool

Interactive memory viewer using MMEMU Serial Monitor SDK.

Usage:
    python3 memory_inspector.py [--host localhost] [--port 2000]

Commands in REPL:
    read <addr> [len]    - Read memory from address
    write <addr> <val>   - Write byte to address
    disasm <addr> [n]    - Disassemble N instructions
    pc <addr>            - Set program counter
    regs                 - Show all registers
    break <addr>         - Set breakpoint
    clear                - Clear breakpoints
    help                 - Show this help
    quit                 - Exit
"""

import sys
import argparse
from pathlib import Path

# Add SDK to path
sdk_path = Path(__file__).parent.parent
sys.path.insert(0, str(sdk_path))

from mmemu_serial_monitor import SerialMonitor, SerialMonitorException


class MemoryInspector:
    """Interactive memory inspection tool."""

    def __init__(self, host: str, port: int):
        self.mm = SerialMonitor(host, port)
        self.current_addr = 0

    def connect(self):
        """Connect to MMEMU."""
        self.mm.connect()
        print(f"✓ Connected to {self.mm.host}:{self.mm.port}")
        print("Type 'help' for commands")

    def disconnect(self):
        """Disconnect from MMEMU."""
        self.mm.disconnect()

    def cmd_read(self, args):
        """read <addr> [len] - Read memory."""
        if not args:
            print("Usage: read <addr> [len]")
            return

        try:
            addr = int(args[0], 16)
            length = int(args[1]) if len(args) > 1 else 256
            self.current_addr = addr

            data = self.mm.read_memory(addr, length)

            # Display in hex dump format
            for offset in range(0, len(data), 16):
                hex_part = ' '.join(f'{b:02X}' for b in data[offset:offset+16])
                ascii_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in data[offset:offset+16])
                print(f"{addr + offset:06X}: {hex_part:<48} {ascii_part}")

        except (ValueError, SerialMonitorException) as e:
            print(f"Error: {e}")

    def cmd_write(self, args):
        """write <addr> <val> - Write byte to address."""
        if len(args) < 2:
            print("Usage: write <addr> <val>")
            return

        try:
            addr = int(args[0], 16)
            value = int(args[1], 16)

            self.mm.write_memory(addr, value & 0xFF)
            print(f"✓ Wrote ${value:02X} to ${addr:06X}")

        except (ValueError, SerialMonitorException) as e:
            print(f"Error: {e}")

    def cmd_disasm(self, args):
        """disasm <addr> [n] - Disassemble instructions."""
        if not args:
            print("Usage: disasm <addr> [n]")
            return

        try:
            addr = int(args[0], 16)
            count = int(args[1]) if len(args) > 1 else 16

            instructions = self.mm.disassemble(addr, count)
            for instr in instructions:
                print(f"  {instr}")

        except (ValueError, SerialMonitorException) as e:
            print(f"Error: {e}")

    def cmd_pc(self, args):
        """pc <addr> - Set program counter."""
        if not args:
            print("Usage: pc <addr>")
            return

        try:
            addr = int(args[0], 16)
            self.mm.set_pc(addr)
            print(f"✓ PC set to ${addr:06X}")

        except (ValueError, SerialMonitorException) as e:
            print(f"Error: {e}")

    def cmd_regs(self, args):
        """regs - Show CPU registers."""
        try:
            regs = self.mm.read_registers()

            # Display registers nicely
            for reg_name in ['PC', 'A', 'X', 'Y', 'SP', 'P']:
                if reg_name in regs:
                    val = regs[reg_name]
                    print(f"  {reg_name:3s} = ${val:06X}" if reg_name == 'PC' else f"  {reg_name:3s} = ${val:02X}")

        except SerialMonitorException as e:
            print(f"Error: {e}")

    def cmd_break(self, args):
        """break <addr> - Set breakpoint."""
        if not args:
            print("Usage: break <addr>")
            return

        try:
            addr = int(args[0], 16)
            self.mm.set_breakpoint(addr)
            print(f"✓ Breakpoint set at ${addr:06X}")

        except (ValueError, SerialMonitorException) as e:
            print(f"Error: {e}")

    def cmd_clear(self, args):
        """clear - Clear all breakpoints."""
        try:
            self.mm.clear_breakpoints()
            print("✓ Breakpoints cleared")

        except SerialMonitorException as e:
            print(f"Error: {e}")

    def cmd_help(self, args):
        """help - Show help."""
        print(__doc__)

    def cmd_quit(self, args):
        """quit - Exit."""
        return False

    def run(self):
        """Run REPL."""
        try:
            while True:
                try:
                    cmd_line = input("> ").strip()
                    if not cmd_line:
                        continue

                    parts = cmd_line.split()
                    cmd = parts[0].lower()
                    args = parts[1:]

                    method = getattr(self, f'cmd_{cmd}', None)
                    if method:
                        result = method(args)
                        if result is False:
                            break
                    else:
                        print(f"Unknown command: {cmd}")

                except KeyboardInterrupt:
                    print("\nInterrupted")
                    break
                except EOFError:
                    break

        finally:
            self.disconnect()
            print("Disconnected")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='localhost', help='MMEMU host')
    parser.add_argument('--port', type=int, default=2000, help='MMEMU port')
    args = parser.parse_args()

    try:
        inspector = MemoryInspector(args.host, args.port)
        inspector.connect()
        inspector.run()
    except Exception as e:
        print(f"Fatal error: {e}")
        sys.exit(1)


if __name__ == '__main__':
    main()
