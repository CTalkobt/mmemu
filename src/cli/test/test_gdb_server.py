#!/usr/bin/env python3
"""Integration tests for the GDB Remote Serial Protocol server."""

import subprocess
import socket
import time
import sys
import struct

GDB_PORT = 12345

class GdbRspClient:
    """Minimal GDB RSP client for testing."""

    def __init__(self, host='127.0.0.1', port=GDB_PORT):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(5)
        self.sock.connect((host, port))

    def send(self, data):
        """Send a GDB RSP packet: $data#checksum"""
        cksum = sum(ord(c) for c in data) & 0xFF
        pkt = f'${data}#{cksum:02x}'
        self.sock.sendall(pkt.encode())

    def recv(self):
        """Receive a GDB RSP packet, return the payload string."""
        buf = b''
        # Read until '$'
        while True:
            c = self.sock.recv(1)
            if not c:
                raise ConnectionError("Connection closed")
            if c == b'$':
                break

        # Read until '#'
        while True:
            c = self.sock.recv(1)
            if not c:
                raise ConnectionError("Connection closed")
            if c == b'#':
                break
            buf += c

        # Read 2-char checksum
        self.sock.recv(2)

        # Send ACK
        self.sock.sendall(b'+')

        return buf.decode()

    def command(self, data):
        """Send a command and return the response."""
        self.send(data)
        return self.recv()

    def close(self):
        self.sock.close()


def wait_for_port(port, timeout=5):
    """Wait until a TCP port is accepting connections."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(0.5)
            s.connect(('127.0.0.1', port))
            s.close()
            return True
        except (ConnectionRefusedError, OSError):
            time.sleep(0.1)
    return False


def run_tests():
    # Start CLI with a raw6502 machine and GDB server
    proc = subprocess.Popen(
        ['./bin/mmemu-cli', '-m', 'raw6502', '--gdb-port', str(GDB_PORT)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1
    )

    try:
        # Wait for GDB server to be ready
        if not wait_for_port(GDB_PORT):
            raise RuntimeError("GDB server did not start in time")

        client = GdbRspClient()

        # --- Test 1: Halt reason ---
        print("--- GDB RSP Tests ---")
        reply = client.command('?')
        assert reply == 'S05', f"Expected S05 (halted), got: {reply}"
        print("  ✓ Halt reason OK (S05)")

        # --- Test 2: qSupported ---
        reply = client.command('qSupported')
        assert 'PacketSize' in reply, f"Expected PacketSize in qSupported, got: {reply}"
        print("  ✓ qSupported OK")

        # --- Test 3: Read registers ---
        reply = client.command('g')
        # 6502 regs: A(2) X(2) Y(2) SP(2) PC(4) P(2) = 14 hex chars
        assert len(reply) == 14, f"Expected 14 hex chars for regs, got {len(reply)}: {reply}"
        print(f"  ✓ Read registers OK (g -> {reply})")

        # --- Test 4: Write memory ---
        # Write NOP; NOP; NOP; LDA #$42; RTS at $0200
        # EA EA EA A9 42 60
        reply = client.command('M0200,6:eaeaeaa94260')
        assert reply == 'OK', f"Memory write failed: {reply}"
        print("  ✓ Write memory OK")

        # --- Test 5: Read memory ---
        reply = client.command('m0200,6')
        assert reply == 'eaeaeaa94260', f"Memory read mismatch: {reply}"
        print("  ✓ Read memory OK")

        # --- Test 6: Write registers (set PC to $0200) ---
        # Layout: A(2) X(2) Y(2) SP(2) PC(4 LE) P(2) = 14 hex chars
        # A=00 X=00 Y=00 SP=FD PC=$0200(LE=0002) P=$24
        reply = client.command('G000000fd000224')
        assert reply == 'OK', f"Register write failed: {reply}"
        # Verify PC was set
        reply = client.command('g')
        pc_hex = reply[8:12]  # Offset 8 = after A,X,Y,SP (2+2+2+2=8)
        assert pc_hex == '0002', f"Expected PC=0002, got: {pc_hex} (full: {reply})"
        print("  ✓ Write registers OK")

        # --- Test 7: Single step ---
        reply = client.command('s')
        assert reply == 'S05', f"Step should return S05, got: {reply}"
        # PC should have advanced by 1 (NOP = 1 byte): $0201 LE = 0102
        reply = client.command('g')
        pc_hex = reply[8:12]
        assert pc_hex == '0102', f"After step, expected PC=$0201 (LE=0102), got: {pc_hex} (full: {reply})"
        print("  ✓ Single step OK")

        # --- Test 8: Set breakpoint and continue ---
        # Set breakpoint at $0203 (LDA #$42)
        reply = client.command('Z0,0203,1')
        assert reply == 'OK', f"Set breakpoint failed: {reply}"
        print("  ✓ Set breakpoint OK")

        # Continue — should hit breakpoint at $0203
        reply = client.command('c')
        assert reply == 'S05', f"Continue should hit breakpoint (S05), got: {reply}"
        reply = client.command('g')
        pc_hex = reply[8:12]
        # $0203 LE = 0302
        assert pc_hex == '0302', f"Expected breakpoint at PC=$0203 (LE=0302), got: {pc_hex} (full: {reply})"
        print("  ✓ Continue + breakpoint OK")

        # --- Test 9: Remove breakpoint ---
        reply = client.command('z0,0203,1')
        assert reply == 'OK', f"Remove breakpoint failed: {reply}"
        print("  ✓ Remove breakpoint OK")

        # --- Test 10: Step through LDA #$42, verify A register ---
        reply = client.command('s')  # Execute LDA #$42
        assert reply == 'S05'
        reply = client.command('g')
        a_hex = reply[0:2]
        assert a_hex == '42', f"After LDA #$42, expected A=$42, got: {a_hex} (full: {reply})"
        print("  ✓ Register verification OK (A=$42 after LDA #$42)")

        # --- Test 11: Detach ---
        reply = client.command('D')
        assert reply == 'OK', f"Detach failed: {reply}"
        print("  ✓ Detach OK")

        client.close()

        print("\n" + "=" * 50)
        print("ALL GDB RSP TESTS PASSED")
        print("=" * 50)

    finally:
        proc.stdin.write('quit\n')
        proc.stdin.flush()
        proc.terminate()
        proc.wait(timeout=3)


if __name__ == '__main__':
    try:
        run_tests()
    except Exception as e:
        print(f"TEST FAILED: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
