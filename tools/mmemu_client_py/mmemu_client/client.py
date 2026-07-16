"""
MMemuClient: Main client class for mmemu serial monitor communication.
"""

import socket
import re
from typing import Dict, List, Tuple, Optional, Union
from .utils import parse_addr


class MMemuClient:
    """Client for communicating with mmemu serial monitor server."""

    def __init__(self, host: str = "localhost", port: int = 6502, timeout: float = 5.0):
        """
        Initialize client connection to mmemu serial monitor.

        Args:
            host: Server hostname/IP (default: localhost)
            port: Server port (default: 6502)
            timeout: Socket timeout in seconds
        """
        self.host = host
        self.port = port
        self.timeout = timeout
        self._socket: Optional[socket.socket] = None
        self._connect()

    def _connect(self) -> None:
        """Establish socket connection to server."""
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._socket.settimeout(self.timeout)
        try:
            self._socket.connect((self.host, self.port))
        except ConnectionRefusedError:
            raise ConnectionError(
                f"Could not connect to mmemu at {self.host}:{self.port}. "
                "Is the server running with --serial-monitor-port?"
            )

    def _send_command(self, cmd: str) -> str:
        """
        Send command and get response.

        Args:
            cmd: Command string (without newline)

        Returns:
            Response from server (without trailing newline)
        """
        if not self._socket:
            self._connect()

        try:
            self._socket.sendall((cmd + "\n").encode())
            response = b""
            while True:
                chunk = self._socket.recv(4096)
                if not chunk:
                    break
                response += chunk
                # Check if we have a complete response (ends with newline)
                if response.endswith(b"\n"):
                    break
        except socket.timeout:
            raise TimeoutError(f"No response from server after {self.timeout}s")
        except (BrokenPipeError, ConnectionResetError):
            self._socket = None
            raise ConnectionError("Connection lost to mmemu server")

        return response.decode().rstrip("\n\r")

    def close(self) -> None:
        """Close connection to server."""
        if self._socket:
            self._socket.close()
            self._socket = None

    def __enter__(self):
        """Context manager entry."""
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        """Context manager exit."""
        self.close()

    # ==================== Register Operations ====================

    def read_registers(self) -> Dict[str, int]:
        """
        Read all CPU registers.

        Returns:
            Dict with keys: A, X, Y, SP, PC, P (all int values)
        """
        response = self._send_command("R")
        return self._parse_registers(response)

    def _parse_registers(self, response: str) -> Dict[str, int]:
        """Parse register response string."""
        # Expected format: A=42 X=00 Y=FF SP=01FE PC=2000 P=30
        regs = {}
        patterns = {
            "A": r"A=([0-9A-Fa-f]+)",
            "X": r"X=([0-9A-Fa-f]+)",
            "Y": r"Y=([0-9A-Fa-f]+)",
            "SP": r"SP=([0-9A-Fa-f]+)",
            "PC": r"PC=([0-9A-Fa-f]+)",
            "P": r"P=([0-9A-Fa-f]+)",
        }
        for key, pattern in patterns.items():
            match = re.search(pattern, response)
            if match:
                regs[key] = int(match.group(1), 16)
        return regs

    # ==================== Memory Operations ====================

    def read_memory(self, addr: Union[int, str], length: int = 256) -> bytes:
        """
        Read memory from emulator.

        Args:
            addr: Start address (int or hex string like "$2000" or "0x2000")
            length: Number of bytes to read (max 65536)

        Returns:
            Bytes read from memory
        """
        if isinstance(addr, str):
            addr = parse_addr(addr)

        # Request memory dump in hex format
        response = self._send_command(f"M {addr:X}")

        # Parse hex dump response
        # Expected format: "2000: AD LDA 00 20 ..."
        data = bytearray()
        lines = response.split("\n")

        for line in lines:
            # Skip empty lines and headers
            if not line.strip() or ":" not in line:
                continue

            # Extract hex bytes from line
            parts = line.split(":")
            if len(parts) >= 2:
                hex_part = parts[1].strip()
                # Extract individual hex bytes
                hex_bytes = hex_part.split()
                for h in hex_bytes:
                    # Skip non-hex data (like instruction mnemonics)
                    if len(h) <= 2 and all(c in "0123456789ABCDEFabcdef" for c in h):
                        data.append(int(h, 16))
                        if len(data) >= length:
                            return bytes(data[:length])

        return bytes(data)

    def write_memory(self, addr: Union[int, str], data: Union[bytes, bytearray]) -> bool:
        """
        Write memory to emulator.

        Args:
            addr: Start address
            data: Bytes to write

        Returns:
            True if successful
        """
        if isinstance(addr, str):
            addr = parse_addr(addr)

        # Write byte by byte (limitation of serial protocol)
        for i, byte in enumerate(data):
            response = self._send_command(f"S {addr + i:X} {byte:02X}")
            if "ERROR" in response.upper():
                return False

        return True

    # ==================== Program Control ====================

    def set_pc(self, addr: Union[int, str]) -> bool:
        """
        Set program counter.

        Args:
            addr: Target address

        Returns:
            True if successful
        """
        if isinstance(addr, str):
            addr = parse_addr(addr)

        response = self._send_command(f"G {addr:X}")
        return "ERROR" not in response.upper()

    # ==================== Disassembly ====================

    def disassemble(
        self, addr: Union[int, str], count: int = 16
    ) -> List[Tuple[int, str]]:
        """
        Disassemble instructions.

        Args:
            addr: Start address
            count: Number of instructions to disassemble

        Returns:
            List of (address, instruction_string) tuples
        """
        if isinstance(addr, str):
            addr = parse_addr(addr)

        response = self._send_command(f"D {addr:X} {count}")
        return self._parse_disassembly(response)

    def _parse_disassembly(self, response: str) -> List[Tuple[int, str]]:
        """Parse disassembly response."""
        # Expected format:
        # 2000 AD LDA 00 20
        # 2003 29 AND #$FF
        instrs = []
        for line in response.split("\n"):
            line = line.strip()
            if not line:
                continue

            parts = line.split()
            if len(parts) < 2:
                continue

            try:
                addr = int(parts[0], 16)
                # Rest is the instruction
                instr = " ".join(parts[1:])
                instrs.append((addr, instr))
            except ValueError:
                continue

        return instrs

    # ==================== Breakpoints ====================

    def set_breakpoint(self, addr: Union[int, str]) -> bool:
        """
        Set breakpoint at address.

        Args:
            addr: Breakpoint address

        Returns:
            True if successful
        """
        if isinstance(addr, str):
            addr = parse_addr(addr)

        response = self._send_command(f"B {addr:X}")
        return "ERROR" not in response.upper()

    def list_breakpoints(self) -> List[Dict]:
        """
        List all breakpoints.

        Returns:
            List of breakpoint dicts with keys: addr, enabled
        """
        response = self._send_command("B")
        return self._parse_breakpoints(response)

    def _parse_breakpoints(self, response: str) -> List[Dict]:
        """Parse breakpoint list response."""
        breakpoints = []
        for line in response.split("\n"):
            line = line.strip()
            if not line or line.startswith("Breakpoint"):
                continue

            # Expected format: $2000 (enabled/disabled)
            parts = line.split()
            if parts:
                try:
                    addr = parse_addr(parts[0])
                    enabled = "disabled" not in line.lower()
                    breakpoints.append({"addr": addr, "enabled": enabled})
                except (ValueError, IndexError):
                    continue

        return breakpoints

    # ==================== Watchpoints ====================

    def set_watchpoint(self, addr: Union[int, str]) -> bool:
        """
        Set watchpoint at address.

        Args:
            addr: Watchpoint address

        Returns:
            True if successful
        """
        if isinstance(addr, str):
            addr = parse_addr(addr)

        response = self._send_command(f"W {addr:X}")
        return "ERROR" not in response.upper()

    def list_watchpoints(self) -> List[Dict]:
        """
        List all watchpoints.

        Returns:
            List of watchpoint dicts
        """
        response = self._send_command("W")
        return self._parse_watchpoints(response)

    def _parse_watchpoints(self, response: str) -> List[Dict]:
        """Parse watchpoint list response."""
        # Similar to breakpoints parsing
        watchpoints = []
        for line in response.split("\n"):
            line = line.strip()
            if not line or line.startswith("Watchpoint"):
                continue

            parts = line.split()
            if parts:
                try:
                    addr = parse_addr(parts[0])
                    watchpoints.append({"addr": addr})
                except (ValueError, IndexError):
                    continue

        return watchpoints

    # ==================== Trace & History ====================

    def get_trace(self, mode: str = "all") -> List[Dict]:
        """
        Get execution trace.

        Args:
            mode: Trace mode - 'all', 'memory', 'calls'

        Returns:
            List of trace entries
        """
        response = self._send_command(f"T {mode}")
        return self._parse_trace(response)

    def _parse_trace(self, response: str) -> List[Dict]:
        """Parse trace response."""
        # Response format depends on trace mode
        trace = []
        # This is a simplified version - actual format may vary
        for line in response.split("\n"):
            if line.strip():
                trace.append({"raw": line.strip()})
        return trace

    def get_history(self) -> List[Dict]:
        """
        Get execution history.

        Returns:
            List of history entries
        """
        response = self._send_command("Z")
        return self._parse_history(response)

    def _parse_history(self, response: str) -> List[Dict]:
        """Parse history response."""
        history = []
        for line in response.split("\n"):
            if line.strip():
                history.append({"raw": line.strip()})
        return history

    # ==================== Debug Metadata ====================

    def list_variables(self, function_name: Optional[str] = None) -> List[Dict]:
        """
        List variables for a function (requires debug metadata).

        Args:
            function_name: Function to inspect (None for all globals)

        Returns:
            List of variable dicts with: name, offset, type, scope
        """
        if function_name:
            response = self._send_command(f"V {function_name}")
        else:
            response = self._send_command("V")

        return self._parse_variables(response)

    def _parse_variables(self, response: str) -> List[Dict]:
        """Parse variable listing response."""
        variables = []
        for line in response.split("\n"):
            line = line.strip()
            if not line or line.startswith("Variable"):
                continue

            # Expected format: var_name @offset size=N type=T scope=S
            match = re.search(
                r"(\w+)\s+@([0-9A-Fa-f]+)\s+size=(\d+)\s+type=(\w+)\s+scope=(\w+)",
                line,
            )
            if match:
                variables.append(
                    {
                        "name": match.group(1),
                        "offset": int(match.group(2), 16),
                        "size": int(match.group(3)),
                        "type": match.group(4),
                        "scope": match.group(5),
                    }
                )

        return variables

    # ==================== Utility ====================

    def get_help(self) -> str:
        """Get command help from server."""
        response = self._send_command("?")
        return response

    def set_uart_divisor(self, divisor: int) -> bool:
        """
        Set UART divisor (affects simulated baud rate).

        Args:
            divisor: UART divisor value

        Returns:
            True if successful
        """
        response = self._send_command(f"+ {divisor}")
        return "ERROR" not in response.upper()
