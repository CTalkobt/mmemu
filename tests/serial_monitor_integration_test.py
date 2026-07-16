#!/usr/bin/env python3
"""
Serial Monitor Protocol Integration Tests

Tests MMEMU's serial monitor server against the protocol specification.
These tests verify compatibility with MEGA65 development tools.

Usage:
    python3 tests/serial_monitor_integration_test.py [--host localhost] [--port 2000]
"""

import socket
import sys
import argparse
import time
from typing import Tuple, Optional

class SerialMonitorClient:
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
            print(f"✓ Connected to {self.host}:{self.port}")
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
        self.socket.sendall((cmd + '\n').encode())

        # Read response until newline
        response = b''
        while True:
            try:
                chunk = self.socket.recv(4096)
                if not chunk:
                    break
                response += chunk
                # Check if we have at least one complete line
                if b'\n' in response:
                    break
            except socket.timeout:
                break

        return response.decode('utf-8', errors='ignore').strip()

def test_basic_commands(client: SerialMonitorClient) -> bool:
    """Test basic Phase 1 commands."""
    tests_passed = 0
    tests_total = 0

    print("\n=== Testing Phase 1: Core Commands ===")

    # Test R (Registers)
    tests_total += 1
    try:
        response = client.send_command('r')
        if 'PC=' in response and 'A=' in response:
            print(f"✓ R (Registers): {response[:60]}...")
            tests_passed += 1
        else:
            print(f"✗ R (Registers): Unexpected format: {response}")
    except Exception as e:
        print(f"✗ R (Registers): {e}")

    # Test M (Memory) - read from address 0
    tests_total += 1
    try:
        response = client.send_command('m 0')
        if ':' in response and any(c in response for c in '0123456789ABCDEF'):
            print(f"✓ M (Memory): {response.split(chr(10))[0][:60]}...")
            tests_passed += 1
        else:
            print(f"✗ M (Memory): Unexpected format")
    except Exception as e:
        print(f"✗ M (Memory): {e}")

    # Test D (Disassemble)
    tests_total += 1
    try:
        response = client.send_command('d 0')
        if any(c.isalpha() for c in response):  # Should have mnemonic
            print(f"✓ D (Disassemble): {response.split(chr(10))[0][:60]}...")
            tests_passed += 1
        else:
            print(f"✗ D (Disassemble): No mnemonics in response")
    except Exception as e:
        print(f"✗ D (Disassemble): {e}")

    # Test ? (Help)
    tests_total += 1
    try:
        response = client.send_command('?')
        if 'Commands' in response or 'command' in response.lower():
            print(f"✓ ? (Help): {response[:60]}...")
            tests_passed += 1
        else:
            print(f"✗ ? (Help): Unexpected format")
    except Exception as e:
        print(f"✗ ? (Help): {e}")

    # Test G (SetPC)
    tests_total += 1
    try:
        response = client.send_command('g 2000')
        if 'OK' in response or 'ok' in response.lower():
            print(f"✓ G (SetPC): {response}")
            tests_passed += 1
        else:
            print(f"✗ G (SetPC): Expected OK, got {response}")
    except Exception as e:
        print(f"✗ G (SetPC): {e}")

    # Test S (SetMemory)
    tests_total += 1
    try:
        response = client.send_command('s 2000 42')
        if 'OK' in response or 'ok' in response.lower():
            print(f"✓ S (SetMemory): {response}")
            tests_passed += 1
        else:
            print(f"✗ S (SetMemory): Expected OK, got {response}")
    except Exception as e:
        print(f"✗ S (SetMemory): {e}")

    # Test B (Breakpoint)
    tests_total += 1
    try:
        response = client.send_command('b 2050')
        if 'OK' in response or 'ok' in response.lower():
            print(f"✓ B (Breakpoint): {response}")
            tests_passed += 1
        else:
            print(f"✗ B (Breakpoint): Expected OK, got {response}")
    except Exception as e:
        print(f"✗ B (Breakpoint): {e}")

    print(f"\nPhase 1: {tests_passed}/{tests_total} tests passed")
    return tests_passed == tests_total

def test_advanced_commands(client: SerialMonitorClient) -> bool:
    """Test advanced Phase 2 commands."""
    tests_passed = 0
    tests_total = 0

    print("\n=== Testing Phase 2: Advanced Commands ===")

    # Test E (FlagWatch)
    tests_total += 1
    try:
        response = client.send_command('e z')
        if 'Zero' in response or 'CLEAR' in response or 'SET' in response:
            print(f"✓ E (FlagWatch Z): {response}")
            tests_passed += 1
        else:
            print(f"✗ E (FlagWatch Z): Unexpected format: {response}")
    except Exception as e:
        print(f"✗ E (FlagWatch Z): {e}")

    # Test E (FlagWatch Carry)
    tests_total += 1
    try:
        response = client.send_command('e c')
        if 'Carry' in response or 'CLEAR' in response or 'SET' in response:
            print(f"✓ E (FlagWatch C): {response}")
            tests_passed += 1
        else:
            print(f"✗ E (FlagWatch C): Unexpected format")
    except Exception as e:
        print(f"✗ E (FlagWatch C): {e}")

    # Test I (Interrupts status)
    tests_total += 1
    try:
        response = client.send_command('i status')
        if 'Interrupt' in response or 'ENABLED' in response or 'DISABLED' in response:
            print(f"✓ I (Interrupts status): {response}")
            tests_passed += 1
        else:
            print(f"✗ I (Interrupts status): Unexpected format")
    except Exception as e:
        print(f"✗ I (Interrupts status): {e}")

    # Test @ (CPU Memory)
    tests_total += 1
    try:
        response = client.send_command('@')
        if 'CPU' in response or 'PC=' in response:
            print(f"✓ @ (CPU Memory): {response.split(chr(10))[0][:60]}...")
            tests_passed += 1
        else:
            print(f"✗ @ (CPU Memory): Unexpected format")
    except Exception as e:
        print(f"✗ @ (CPU Memory): {e}")

    # Test T (Trace)
    tests_total += 1
    try:
        response = client.send_command('t on')
        if 'Trace' in response or 'ON' in response:
            print(f"✓ T (Trace on): {response}")
            tests_passed += 1
        else:
            print(f"✗ T (Trace on): Unexpected format")
    except Exception as e:
        print(f"✗ T (Trace on): {e}")

    # Test Z (History)
    tests_total += 1
    try:
        response = client.send_command('z')
        if 'History' in response or 'instruction' in response.lower() or 'PC=' in response:
            print(f"✓ Z (History): {response.split(chr(10))[0][:60]}...")
            tests_passed += 1
        else:
            print(f"✗ Z (History): Unexpected format")
    except Exception as e:
        print(f"✗ Z (History): {e}")

    print(f"\nPhase 2: {tests_passed}/{tests_total} tests passed")
    return tests_passed == tests_total

def test_error_handling(client: SerialMonitorClient) -> bool:
    """Test error responses."""
    tests_passed = 0
    tests_total = 0

    print("\n=== Testing Error Handling ===")

    # Test invalid command
    tests_total += 1
    try:
        response = client.send_command('x')
        if 'ERROR' in response or 'error' in response.lower():
            print(f"✓ Invalid command error: {response[:60]}")
            tests_passed += 1
        else:
            print(f"✗ Invalid command: Should return ERROR, got {response}")
    except Exception as e:
        print(f"✗ Invalid command: {e}")

    # Test malformed flag watch
    tests_total += 1
    try:
        response = client.send_command('e xyz')
        if 'ERROR' in response or 'error' in response.lower():
            print(f"✓ Malformed flag error: {response[:60]}")
            tests_passed += 1
        else:
            print(f"✗ Malformed flag: Should return ERROR, got {response}")
    except Exception as e:
        print(f"✗ Malformed flag: {e}")

    print(f"\nError handling: {tests_passed}/{tests_total} tests passed")
    return tests_passed == tests_total

def main():
    parser = argparse.ArgumentParser(description='Serial Monitor Integration Tests')
    parser.add_argument('--host', default='localhost', help='Serial monitor server host')
    parser.add_argument('--port', type=int, default=2000, help='Serial monitor server port')
    args = parser.parse_args()

    print(f"Serial Monitor Integration Test Suite")
    print(f"Target: {args.host}:{args.port}")
    print("=" * 60)

    client = SerialMonitorClient(args.host, args.port)

    if not client.connect():
        print("Cannot reach serial monitor server. Is it running?")
        print(f"Try: ./bin/mmemu-cli -m mega65 --serial-monitor-port {args.port}")
        sys.exit(1)

    try:
        phase1_ok = test_basic_commands(client)
        phase2_ok = test_advanced_commands(client)
        error_ok = test_error_handling(client)

        print("\n" + "=" * 60)
        print("SUMMARY")
        print("=" * 60)
        print(f"Phase 1 (Core):      {'✓ PASS' if phase1_ok else '✗ FAIL'}")
        print(f"Phase 2 (Advanced):  {'✓ PASS' if phase2_ok else '✗ FAIL'}")
        print(f"Error Handling:      {'✓ PASS' if error_ok else '✗ FAIL'}")

        if phase1_ok and phase2_ok and error_ok:
            print("\n✓ All tests passed!")
            sys.exit(0)
        else:
            print("\n✗ Some tests failed")
            sys.exit(1)

    finally:
        client.disconnect()

if __name__ == '__main__':
    main()
