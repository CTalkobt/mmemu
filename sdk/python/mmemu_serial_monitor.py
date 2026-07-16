#!/usr/bin/env python3
"""
MMEMU Serial Monitor Python SDK

Provides a Pythonic interface to the MEGA65 Serial Monitor Protocol.
Enables easy integration with IDEs, debuggers, and development tools.

Usage:
    from mmemu_serial_monitor import SerialMonitor

    mm = SerialMonitor('localhost', 2000)
    mm.connect()

    # Read registers
    regs = mm.read_registers()
    print(f"PC: {regs['PC']:06X}")

    # Read memory
    memory = mm.read_memory(0x2000, 256)

    # Set breakpoint
    mm.set_breakpoint(0x2050)

    mm.disconnect()
"""

import socket
from typing import Dict, List, Optional, Tuple
import re


class SerialMonitorException(Exception):
    """Base exception for serial monitor operations."""
    pass


class ConnectionError(SerialMonitorException):
    """Connection-related error."""
    pass


class ProtocolError(SerialMonitorException):
    """Protocol parsing or response error."""
    pass


class Register:
    """CPU register value with metadata."""

    def __init__(self, name: str, value: int, width: int = 8):
        self.name = name
        self.value = value
        self.width = width  # bits

    def __repr__(self) -> str:
        fmt = f"{{:0{self.width // 4}X}}"
        return f"{self.name}={fmt.format(self.value)}"

    def __int__(self) -> int:
        return self.value


class CPUFlags:
    """CPU status flags parser."""

    FLAGS = {
        'N': (0x80, 'Negative'),
        'V': (0x40, 'Overflow'),
        'B': (0x10, 'Break'),
        'D': (0x08, 'Decimal'),
        'I': (0x04, 'Interrupt'),
        'Z': (0x02, 'Zero'),
        'C': (0x01, 'Carry'),
    }

    def __init__(self, p_value: int):
        self.p_value = p_value

    def __getitem__(self, flag: str) -> bool:
        """Check if flag is set."""
        if flag not in self.FLAGS:
            raise ValueError(f"Unknown flag: {flag}")
        mask, _ = self.FLAGS[flag]
        return (self.p_value & mask) != 0

    def get_name(self, flag: str) -> str:
        """Get human-readable flag name."""
        if flag not in self.FLAGS:
            raise ValueError(f"Unknown flag: {flag}")
        return self.FLAGS[flag][1]

    def __repr__(self) -> str:
        result = []
        for flag in ['N', 'V', 'B', 'D', 'I', 'Z', 'C']:
            result.append(flag if self[flag] else '-')
        return ''.join(result)


class Instruction:
    """Parsed instruction."""

    def __init__(self, addr: int, mnemonic: str, operands: str = ''):
        self.addr = addr
        self.mnemonic = mnemonic
        self.operands = operands
        self.complete = f"{mnemonic} {operands}".strip()

    def __repr__(self) -> str:
        return f"{self.addr:06X} {self.complete}"


class SerialMonitor:
    """
    MEGA65 Serial Monitor client.

    Provides a high-level Python interface to the serial monitor protocol.
    """

    def __init__(self, host: str = 'localhost', port: int = 2000, timeout: float = 5.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.socket: Optional[socket.socket] = None
        self._connected = False
        self._last_mem_addr = 0

    def connect(self) -> None:
        """Connect to the serial monitor server."""
        try:
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.socket.settimeout(self.timeout)
            self.socket.connect((self.host, self.port))
            self._connected = True
        except socket.error as e:
            raise ConnectionError(f"Failed to connect to {self.host}:{self.port}: {e}")

    def disconnect(self) -> None:
        """Disconnect from the serial monitor server."""
        if self.socket:
            self.socket.close()
            self.socket = None
            self._connected = False

    def is_connected(self) -> bool:
        """Check if connected."""
        return self._connected

    def _send_command(self, cmd: str) -> str:
        """Send a command and receive response."""
        if not self._connected:
            raise ConnectionError("Not connected")

        try:
            self.socket.sendall((cmd + '\n').encode())

            # Read response
            response = b''
            while True:
                try:
                    chunk = self.socket.recv(4096)
                    if not chunk:
                        break
                    response += chunk
                    if b'\n' in response:
                        break
                except socket.timeout:
                    break

            return response.decode('utf-8', errors='ignore').strip()
        except socket.error as e:
            self._connected = False
            raise ConnectionError(f"Command failed: {e}")

    def read_registers(self) -> Dict[str, int]:
        """Read all CPU registers."""
        response = self._send_command('r')

        if 'ERROR' in response:
            raise ProtocolError(f"Read registers failed: {response}")

        result = {}

        # Parse response: PC=002000 A=42 X=01 Y=02 SP=01F8 P=30
        pairs = response.split()
        for pair in pairs:
            if '=' in pair:
                name, value = pair.split('=')
                try:
                    result[name] = int(value, 16)
                except ValueError:
                    pass

        return result

    def read_memory(self, addr: int = 0, length: int = 256) -> bytes:
        """
        Read memory block.

        Args:
            addr: Start address (default: continue from last address)
            length: Number of bytes to read (default: 256)

        Returns:
            Bytes read from memory
        """
        if addr > 0:
            self._last_mem_addr = addr

        cmd = f"m {addr:X}" if addr > 0 else "m"
        response = self._send_command(cmd)

        if 'ERROR' in response:
            raise ProtocolError(f"Read memory failed: {response}")

        # Parse hex bytes from memory dump
        result = bytearray()
        for line in response.split('\n'):
            # Skip non-data lines
            if not ':' in line:
                continue

            # Extract hex part: "002000: 4C 30 E5 A2..."
            parts = line.split(':')
            if len(parts) < 2:
                continue

            hex_part = parts[1].split()[0:16]  # First 16 bytes
            for hex_byte in hex_part:
                try:
                    result.append(int(hex_byte, 16))
                except ValueError:
                    pass

        self._last_mem_addr = addr + len(result)
        return bytes(result[:length])

    def write_memory(self, addr: int, value: int) -> None:
        """Write single byte to memory."""
        response = self._send_command(f"s {addr:X} {value:02X}")

        if 'ERROR' in response or 'OK' not in response:
            raise ProtocolError(f"Write memory failed: {response}")

    def write_memory_block(self, addr: int, data: bytes) -> None:
        """Write multiple bytes to memory."""
        for i, byte in enumerate(data):
            self.write_memory(addr + i, byte)

    def disassemble(self, addr: int = 0, count: int = 16) -> List[Instruction]:
        """Disassemble instructions."""
        cmd = f"d {addr:X}" if addr > 0 else "d"
        response = self._send_command(cmd)

        if 'ERROR' in response:
            raise ProtocolError(f"Disassemble failed: {response}")

        instructions = []
        for line in response.split('\n'):
            if not line.strip():
                continue

            # Parse: "002000 JMP $E530" or "002000 4C 30 E5    JMP $E530"
            parts = line.split()
            if len(parts) < 2:
                continue

            try:
                addr_hex = parts[0]
                addr = int(addr_hex, 16)
                mnemonic = parts[-2] if len(parts) > 2 else parts[1]
                operands = parts[-1] if len(parts) > 2 else ''

                instructions.append(Instruction(addr, mnemonic, operands))
            except (ValueError, IndexError):
                pass

        return instructions[:count]

    def set_pc(self, addr: int) -> None:
        """Set program counter."""
        response = self._send_command(f"g {addr:X}")

        if 'ERROR' in response or 'OK' not in response:
            raise ProtocolError(f"Set PC failed: {response}")

    def set_breakpoint(self, addr: int) -> None:
        """Set breakpoint at address."""
        response = self._send_command(f"b {addr:X}")

        if 'ERROR' in response or 'OK' not in response:
            raise ProtocolError(f"Set breakpoint failed: {response}")

    def clear_breakpoints(self) -> None:
        """Clear all breakpoints."""
        response = self._send_command("b")

        if 'ERROR' in response or 'OK' not in response:
            raise ProtocolError(f"Clear breakpoints failed: {response}")

    def get_flag(self, flag: str) -> bool:
        """
        Check CPU status flag.

        Args:
            flag: Flag name (N, V, B, D, I, Z, C)

        Returns:
            True if flag is set
        """
        response = self._send_command(f"e {flag}")

        if 'ERROR' in response:
            raise ProtocolError(f"Flag watch failed: {response}")

        # Response: "Negative (N) = SET" or "CLEAR"
        return 'SET' in response

    def enable_interrupts(self) -> None:
        """Enable CPU interrupts."""
        response = self._send_command("i enable")

        if 'ERROR' in response:
            raise ProtocolError(f"Enable interrupts failed: {response}")

    def disable_interrupts(self) -> None:
        """Disable CPU interrupts."""
        response = self._send_command("i disable")

        if 'ERROR' in response:
            raise ProtocolError(f"Disable interrupts failed: {response}")

    def get_interrupt_status(self) -> bool:
        """Check if interrupts are enabled."""
        response = self._send_command("i status")

        if 'ERROR' in response:
            raise ProtocolError(f"Get interrupt status failed: {response}")

        return 'ENABLED' in response

    def enable_trace(self) -> None:
        """Enable instruction tracing."""
        response = self._send_command("t on")

        if 'ERROR' in response:
            raise ProtocolError(f"Enable trace failed: {response}")

    def disable_trace(self) -> None:
        """Disable instruction tracing."""
        response = self._send_command("t off")

        if 'ERROR' in response:
            raise ProtocolError(f"Disable trace failed: {response}")

    def get_trace_dump(self) -> str:
        """Get trace buffer dump."""
        response = self._send_command("t dump")

        if 'ERROR' in response:
            raise ProtocolError(f"Get trace dump failed: {response}")

        return response

    def get_cpu_history(self) -> str:
        """Get CPU execution history (last 32 instructions)."""
        response = self._send_command("z")

        if 'ERROR' in response:
            raise ProtocolError(f"Get CPU history failed: {response}")

        return response

    def get_cpu_view(self) -> str:
        """Get CPU memory view."""
        response = self._send_command("@")

        if 'ERROR' in response:
            raise ProtocolError(f"Get CPU view failed: {response}")

        return response

    def help(self) -> str:
        """Get command help."""
        return self._send_command("?")


if __name__ == '__main__':
    # Example usage
    try:
        mm = SerialMonitor('localhost', 2000)
        mm.connect()

        print("Connected to MMEMU Serial Monitor")
        print(f"Help: {mm.help()}\n")

        # Read registers
        regs = mm.read_registers()
        print(f"Registers: {regs}")

        # Read memory
        memory = mm.read_memory(0x0, 32)
        print(f"Memory[0x0000]: {memory.hex()}")

        # Disassemble
        instrs = mm.disassemble(0x0, 4)
        for instr in instrs:
            print(f"  {instr}")

        mm.disconnect()
        print("\nDisconnected")

    except Exception as e:
        print(f"Error: {e}")
