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


def run_sm83_tests():
    """The Game Boy's SM83: AF BC DE HL SP PC (16-bit each, GDB's Z80 order), a target description,
    p/P, and `c` not stopping on $00 (NOP here, BRK only on the 6502)."""
    port = GDB_PORT + 1
    proc = subprocess.Popen(
        ['./bin/mmemu-cli', '-m', 'gameboy', '--speed', 'max', '--gdb-port', str(port)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
    try:
        if not wait_for_port(port):
            raise RuntimeError("GDB server did not start in time")
        client = GdbRspClient(port=port)
        print("--- GDB RSP Tests (Game Boy, SM83) ---")

        reply = client.command('qSupported')
        assert 'qXfer:features:read+' in reply, f"Expected qXfer in qSupported, got: {reply}"
        xml = client.command('qXfer:features:read:target.xml:0,1000')
        assert xml.startswith('l<?xml') and '<reg name="af" bitsize="16"/>' in xml and 'name="pc" bitsize="16" type="code_ptr"' in xml, xml
        part = client.command('qXfer:features:read:target.xml:0,10')
        assert part == 'm' + xml[1:17], f"Expected the first $10 bytes, got: {part}"
        print("  ✓ Target description OK")

        # After the boot ROM (none: its hand-off state): AF=$01B0 BC=$0013 DE=$00D8 HL=$014D SP=$FFFE PC=$0100
        reply = client.command('g')
        assert reply == 'b0011300d8004d01feff0001', f"Unexpected SM83 registers: {reply}"
        assert client.command('p5') == '0001', "p5 (PC) should be $0100"
        print(f"  ✓ Read registers OK (g -> {reply})")

        # In work RAM: NOP x4; LD A,$42; JR -2
        assert client.command('Mc000,8:000000003e4218fe') == 'OK'
        assert client.command('mc000,8') == '000000003e4218fe'
        assert client.command('P5=00c0') == 'OK'
        assert client.command('p5') == '00c0'
        print("  ✓ Write memory and PC OK")

        assert client.command('s') == 'S05'
        assert client.command('p5') == '01c0', "After a NOP, PC should be $C001"
        assert client.command('Z0,c004,1') == 'OK'
        assert client.command('c') == 'S05'
        assert client.command('p5') == '04c0', "Continue should run past the NOPs to the breakpoint at $C004"
        assert client.command('z0,c004,1') == 'OK'
        print("  ✓ Step, breakpoint and continue past $00 OK")

        assert client.command('s') == 'S05'
        af = client.command('p0')
        assert af[2:4] == '42', f"After LD A,$42, expected A=$42, got AF (LE) {af}"
        reply = client.command('G' + 'b0ff' + '3412' + '7856' + 'bc9a' + 'f0df' + '00c0')
        assert reply == 'OK'
        assert client.command('g') == 'b0ff34127856bc9af0df00c0'
        print("  ✓ Register verification and G OK")

        assert client.command('D') == 'OK'
        client.close()
        print("ALL SM83 GDB RSP TESTS PASSED")
    finally:
        proc.stdin.write('quit\n')
        proc.stdin.flush()
        proc.terminate()
        proc.wait(timeout=3)


if __name__ == '__main__':
    try:
        run_tests()
        run_sm83_tests()
    except Exception as e:
        print(f"TEST FAILED: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
