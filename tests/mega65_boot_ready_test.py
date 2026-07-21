#!/usr/bin/env python3
"""
MEGA65 Boot Ready Prompt Test

Tests whether the MEGA65 emulator boots to a Ready prompt.
Uses direct CLI communication via stdin/stdout.

Usage:
    python3 tests/mega65_boot_ready_test.py [--timeout 120]
"""

import subprocess
import time
import sys
import argparse
import re
from typing import Optional

class MMEMUBootTest:
    """MEGA65 Boot test runner."""

    def __init__(self, timeout: int = 120, max_cycles: int = 1000000):
        self.timeout = timeout
        self.max_cycles = max_cycles
        self.process: Optional[subprocess.Popen] = None

    def start(self) -> bool:
        """Start MEGA65 emulator."""
        try:
            self.process = subprocess.Popen(
                ["./bin/mmemu-cli"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                bufsize=1
            )
            return True
        except Exception as e:
            print(f"✗ Failed to start emulator: {e}")
            return False

    def send_command(self, cmd: str) -> str:
        """Send command and get response."""
        if not self.process or not self.process.stdin:
            return ""

        try:
            self.process.stdin.write(cmd + "\n")
            self.process.stdin.flush()

            # Read response until prompt
            response = ""
            while True:
                try:
                    line = self.process.stdout.readline()
                    if not line:
                        break
                    response += line
                    if line.strip() == ">":
                        break
                except:
                    break

            return response
        except Exception as e:
            print(f"✗ Command error: {e}")
            return ""

    def stop(self):
        """Stop emulator."""
        if self.process:
            try:
                self.process.terminate()
                self.process.wait(timeout=2)
            except:
                self.process.kill()
            self.process = None

    def parse_cycles(self, response: str) -> int:
        """Extract cycle count from response."""
        match = re.search(r'Cycles:\s*(\d+)', response)
        if match:
            return int(match.group(1))
        return 0

    def parse_pc(self, response: str) -> str:
        """Extract PC from response."""
        match = re.search(r'PC:\s*(\$?[0-9A-Fa-f]+)', response)
        if match:
            return match.group(1)
        return "?????"

    def parse_memory(self, response: str) -> bytes:
        """Parse memory dump response."""
        data = b""
        lines = response.split('\n')
        for line in lines:
            if ':' in line:
                try:
                    # Format: "XXXX: xx xx xx ..."
                    parts = line.split(':')[1].strip().split()
                    for part in parts:
                        if len(part) == 2 and all(c in '0123456789ABCDEFabcdef' for c in part):
                            data += bytes([int(part, 16)])
                except:
                    pass
        return data

    def screen_contains_ready(self, screen_data: bytes) -> bool:
        """Check if screen contains 'Ready' text."""
        screen_text = ""
        for byte in screen_data:
            if 32 <= byte < 127:
                screen_text += chr(byte)
            elif byte == 0:
                screen_text += " "
            else:
                screen_text += "?"

        ready_text = screen_text.strip().upper()
        return "READY" in ready_text

    def print_screen(self, screen_data: bytes):
        """Print screen contents."""
        print("Screen contents (first 5 lines):")
        for line_num in range(min(5, len(screen_data) // 40)):
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

            print(f"  Line {line_num}: {line_text.rstrip()}")

    def run(self) -> bool:
        """Run the test."""
        print("=" * 40)
        print("MEGA65 Boot Ready Prompt Test")
        print("=" * 40)
        print(f"Max boot time: {self.timeout} seconds")
        print(f"Max cycles: {self.max_cycles}")
        print("")

        # Start emulator
        if not self.start():
            return False

        print("✓ Emulator started")

        # Wait for prompt
        time.sleep(0.5)
        self.send_command("")  # Read initial prompt

        try:
            # Create MEGA65 machine
            print("Creating MEGA65 machine...")
            response = self.send_command("create mega65")
            if "Created machine: MEGA65" not in response:
                print("✗ Failed to create MEGA65 machine")
                return False

            print("✓ MEGA65 machine created")

            # Disable features that might interfere
            self.send_command("brktrap off")
            self.send_command("hle off")

            print("Running boot sequence...")
            print("")

            start_time = time.time()
            check_count = 0
            last_cycles = 0

            # Run boot with periodic checks
            while time.time() - start_time < self.timeout:
                check_count += 1

                # Run next batch of cycles
                print(f"[Check {check_count:2d}] Running 100,000 cycles...", end=" ", flush=True)
                response = self.send_command("run 100000")

                cycles = self.parse_cycles(response)
                pc = self.parse_pc(response)

                print(f"Cycles: {cycles:7d} PC: {pc}")

                # Check screen memory for Ready
                mem_response = self.send_command("m $0400 1000")
                screen_data = self.parse_memory(mem_response)

                if self.screen_contains_ready(screen_data):
                    elapsed = time.time() - start_time
                    print("")
                    print("✓ SUCCESS!")
                    print(f"✓ Ready prompt found!")
                    print(f"✓ Elapsed time: {elapsed:.1f} seconds")
                    print(f"✓ Total cycles: {cycles}")
                    print("")
                    self.print_screen(screen_data)
                    return True

                # Safety check
                if cycles >= self.max_cycles:
                    print("")
                    print(f"Reached max cycles ({self.max_cycles})")
                    break

                last_cycles = cycles

            # Timeout or max cycles reached
            print("")
            print("✗ FAILED!")
            print(f"✗ Ready prompt not found after {time.time() - start_time:.1f} seconds")
            print(f"✗ Final cycle count: {last_cycles}")

            # Print final state
            response = self.send_command("regs")
            print(f"\nFinal CPU state:")
            for line in response.split('\n'):
                if 'PC:' in line or 'Cycles:' in line:
                    print(f"  {line.strip()}")

            mem_response = self.send_command("m $0400 1000")
            screen_data = self.parse_memory(mem_response)
            print("")
            self.print_screen(screen_data)

            return False

        finally:
            self.stop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Test MEGA65 boot to Ready prompt")
    parser.add_argument("--timeout", type=int, default=120,
                        help="Maximum test duration in seconds (default: 120)")
    parser.add_argument("--max-cycles", type=int, default=1000000,
                        help="Maximum emulator cycles (default: 1000000)")

    args = parser.parse_args()

    # Run test
    tester = MMEMUBootTest(timeout=args.timeout, max_cycles=args.max_cycles)
    success = tester.run()

    # Exit with appropriate code
    sys.exit(0 if success else 1)
