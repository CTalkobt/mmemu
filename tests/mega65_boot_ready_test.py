#!/usr/bin/env python3
"""
MEGA65 Boot Ready Prompt Test

Tests whether the MEGA65 emulator boots to a Ready prompt.
Uses the Serial Monitor protocol to query emulator state.

Usage:
    python3 tests/mega65_boot_ready_test.py [--timeout 60] [--host localhost] [--port 2000]
"""

import socket
import subprocess
import time
import sys
import argparse
from typing import Optional, Tuple

class MMEMUClient:
    """Client for communicating with MMEMU serial monitor server."""

    def __init__(self, host: str = 'localhost', port: int = 2000, timeout: float = 5.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.socket: Optional[socket.socket] = None

    def connect(self) -> bool:
        """Connect to the serial monitor server."""
        try:
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.socket.settimeout(self.timeout)
            self.socket.connect((self.host, self.port))
            return True
        except Exception as e:
            print(f"✗ Connection failed: {e}")
            return False

    def disconnect(self):
        """Disconnect from server."""
        if self.socket:
            self.socket.close()
            self.socket = None

    def send_command(self, cmd: str) -> str:
        """Send a command and receive response."""
        if not self.socket:
            raise RuntimeError("Not connected")

        # Send command with newline
        self.socket.sendall((cmd + "\n").encode('utf-8'))

        # Receive response until prompt or timeout
        response = b""
        try:
            while True:
                chunk = self.socket.recv(4096)
                if not chunk:
                    break
                response += chunk
                # Check if we got a prompt
                if response.endswith(b"> "):
                    break
        except socket.timeout:
            pass

        return response.decode('utf-8', errors='replace')

    def read_memory(self, addr: int, length: int) -> bytes:
        """Read memory at address."""
        cmd = f"m ${addr:04X} {length}"
        response = self.send_command(cmd)
        # Parse memory output format: "XXXX: xx xx xx ..."
        lines = response.split('\n')
        data = b""
        for line in lines:
            if ':' in line:
                parts = line.split(':')[1].strip().split()
                for part in parts:
                    try:
                        data += bytes([int(part, 16)])
                    except ValueError:
                        pass
        return data

    def get_cycles(self) -> int:
        """Get current cycle count."""
        response = self.send_command("regs")
        for line in response.split('\n'):
            if 'Cycles:' in line:
                try:
                    return int(line.split(':')[1].strip())
                except (ValueError, IndexError):
                    return 0
        return 0

    def get_pc(self) -> int:
        """Get current PC."""
        response = self.send_command("regs")
        for line in response.split('\n'):
            if 'PC:' in line:
                try:
                    pc_str = line.split(':')[1].strip().replace('$', '')
                    return int(pc_str, 16)
                except (ValueError, IndexError):
                    return 0
        return 0


def screen_contains_ready(screen_data: bytes) -> bool:
    """Check if screen memory contains 'Ready' text."""
    # Try to find "Ready" in ASCII representation
    screen_text = ""
    for byte in screen_data:
        if 32 <= byte < 127:
            screen_text += chr(byte)
        elif byte == 0:
            screen_text += "."
        else:
            screen_text += "?"

    return "Ready" in screen_text or "READY" in screen_text.upper()


def print_screen(screen_data: bytes, lines: int = 5):
    """Print screen contents."""
    print("Screen contents (first {} lines):".format(lines))
    for line_num in range(min(lines, len(screen_data) // 40)):
        line_start = line_num * 40
        line_end = min(line_start + 40, len(screen_data))
        line_data = screen_data[line_start:line_end]

        line_text = ""
        for byte in line_data:
            if 32 <= byte < 127:
                line_text += chr(byte)
            elif byte == 0:
                line_text += "."
            else:
                line_text += "?"

        print(f"  Line {line_num}: {line_text}")


def run_mega65_boot_test(timeout: int = 60, max_cycles: int = 1000000) -> bool:
    """
    Run MEGA65 boot test.

    Returns True if Ready prompt found, False otherwise.
    """
    print("=" * 40)
    print("MEGA65 Boot Ready Prompt Test")
    print("=" * 40)
    print(f"Max boot time: {timeout} seconds")
    print(f"Max cycles: {max_cycles}")
    print("")

    # Start MMEMU CLI with MEGA65 machine
    print("Starting MEGA65 emulator...")
    cli_process = subprocess.Popen(
        ["./bin/mmemu-cli", "-m", "rawMega65"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )

    # Give the process time to start
    time.sleep(1)

    # Connect to serial monitor
    client = MMEMUClient()
    if not client.connect():
        cli_process.terminate()
        return False

    try:
        # Send initialization commands
        print("Setting up emulator...")
        client.send_command("brktrap off")
        client.send_command("hle off")

        start_time = time.time()
        check_count = 0

        print("Running boot sequence...")
        print("")

        # Run boot sequence with periodic checks
        while time.time() - start_time < timeout:
            check_count += 1

            # Read screen memory (first 40 chars of first line)
            screen_data = client.read_memory(0x0400, 1000)

            # Check for Ready
            if screen_contains_ready(screen_data):
                elapsed = time.time() - start_time
                cycles = client.get_cycles()

                print("✓ SUCCESS!")
                print(f"✓ Ready prompt found!")
                print(f"✓ Elapsed time: {elapsed:.1f} seconds")
                print(f"✓ Cycles: {cycles}")
                print("")
                print_screen(screen_data, 5)

                return True

            # Print progress
            pc = client.get_pc()
            cycles = client.get_cycles()
            print(f"[Check {check_count}] Cycles: {cycles:7d} PC: ${pc:04X}")

            # Run more cycles
            client.send_command("run 100000")

            time.sleep(0.1)

        # Timeout reached
        print("")
        print("✗ FAILED!")
        print(f"✗ Ready prompt not found after {timeout} seconds")

        # Print final state
        screen_data = client.read_memory(0x0400, 1000)
        pc = client.get_pc()
        cycles = client.get_cycles()

        print("")
        print_screen(screen_data, 5)
        print("")
        print(f"Final PC: ${pc:04X}")
        print(f"Final Cycles: {cycles}")

        return False

    finally:
        client.disconnect()
        cli_process.terminate()
        try:
            cli_process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            cli_process.kill()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Test MEGA65 boot to Ready prompt")
    parser.add_argument("--timeout", type=int, default=60,
                        help="Maximum test duration in seconds (default: 60)")
    parser.add_argument("--max-cycles", type=int, default=1000000,
                        help="Maximum emulator cycles (default: 1000000)")
    parser.add_argument("--host", default="localhost",
                        help="Serial monitor host (default: localhost)")
    parser.add_argument("--port", type=int, default=2000,
                        help="Serial monitor port (default: 2000)")

    args = parser.parse_args()

    # Run test
    success = run_mega65_boot_test(timeout=args.timeout, max_cycles=args.max_cycles)

    # Exit with appropriate code
    sys.exit(0 if success else 1)
