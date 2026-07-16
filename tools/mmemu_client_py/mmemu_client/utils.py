"""
Utility functions for mmemu client.
"""

import re
from typing import Dict


def parse_addr(s: str) -> int:
    """
    Parse address in various formats.

    Supports:
    - Hex: $2000, 0x2000, 2000h
    - Decimal: 8192
    - Binary: 0b0010000000000000

    Args:
        s: Address string

    Returns:
        Integer address value
    """
    s = s.strip()

    # Hex formats
    if s.startswith("$"):
        return int(s[1:], 16)
    if s.startswith("0x") or s.startswith("0X"):
        return int(s[2:], 16)
    if s.endswith("h") or s.endswith("H"):
        return int(s[:-1], 16)

    # Binary format
    if s.startswith("0b") or s.startswith("0B"):
        return int(s[2:], 2)

    # Try hex if all hex digits
    if all(c in "0123456789ABCDEFabcdef" for c in s):
        try:
            # Assume hex if >= 4 digits
            if len(s) >= 4:
                return int(s, 16)
        except ValueError:
            pass

    # Default to decimal
    return int(s)


def format_addr(addr: int, width: int = 4) -> str:
    """Format address as hex string."""
    return f"${addr:0{width}X}"


def format_byte(b: int) -> str:
    """Format single byte as hex string."""
    return f"${b:02X}"


def format_word(w: int, little_endian: bool = True) -> str:
    """Format 16-bit word as hex."""
    if little_endian:
        return f"${w & 0xFF:02X}${w >> 8:02X}"
    return f"${w >> 8:02X}${w & 0xFF:02X}"


def format_regs(regs: Dict[str, int], compact: bool = False) -> str:
    """
    Format register dictionary for display.

    Args:
        regs: Register dict with keys A, X, Y, SP, PC, P
        compact: If True, single line format

    Returns:
        Formatted string
    """
    if not regs:
        return "(no registers)"

    if compact:
        parts = []
        if "A" in regs:
            parts.append(f"A=${regs['A']:02X}")
        if "X" in regs:
            parts.append(f"X=${regs['X']:02X}")
        if "Y" in regs:
            parts.append(f"Y=${regs['Y']:02X}")
        if "SP" in regs:
            parts.append(f"SP=${regs['SP']:04X}")
        if "PC" in regs:
            parts.append(f"PC=${regs['PC']:04X}")
        if "P" in regs:
            parts.append(f"P=${regs['P']:02X}")
        return " ".join(parts)

    lines = []
    if "A" in regs:
        lines.append(f"  A  = ${regs['A']:02X}")
    if "X" in regs:
        lines.append(f"  X  = ${regs['X']:02X}")
    if "Y" in regs:
        lines.append(f"  Y  = ${regs['Y']:02X}")
    if "SP" in regs:
        lines.append(f"  SP = ${regs['SP']:04X}")
    if "PC" in regs:
        lines.append(f"  PC = ${regs['PC']:04X}")
    if "P" in regs:
        lines.append(f"  P  = ${regs['P']:02X}")

    return "\n".join(lines)


def format_registers_verbose(regs: Dict[str, int]) -> str:
    """Format registers with flag interpretation."""
    lines = []

    for key in ["A", "X", "Y", "SP", "PC"]:
        if key in regs:
            lines.append(f"{key:2} = ${regs[key]:04X}")

    if "P" in regs:
        p = regs["P"]
        lines.append(f"P  = ${p:02X}  (", end="")
        flags = []
        if p & 0x80:
            flags.append("N")
        if p & 0x40:
            flags.append("V")
        if p & 0x20:
            flags.append("-")
        if p & 0x10:
            flags.append("B")
        if p & 0x08:
            flags.append("D")
        if p & 0x04:
            flags.append("I")
        if p & 0x02:
            flags.append("Z")
        if p & 0x01:
            flags.append("C")
        lines[-1] += " ".join(flags) + ")"

    return "\n".join(lines)


def format_memory(data: bytes, addr: int = 0, width: int = 16) -> str:
    """
    Format memory dump with hex and ASCII representation.

    Args:
        data: Bytes to format
        addr: Starting address for display
        width: Bytes per line

    Returns:
        Formatted memory dump string
    """
    lines = []
    for i in range(0, len(data), width):
        chunk = data[i : i + width]
        hex_part = " ".join(f"{b:02X}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        line = f"${addr + i:04X}: {hex_part:<{width * 3 - 1}}  {ascii_part}"
        lines.append(line)

    return "\n".join(lines)


def format_disassembly(
    disasm: list, addr_width: int = 4, show_bytes: bool = True
) -> str:
    """
    Format disassembly output.

    Args:
        disasm: List of (addr, instr) tuples
        addr_width: Width for address display
        show_bytes: Include instruction bytes

    Returns:
        Formatted disassembly string
    """
    lines = []
    for addr, instr in disasm:
        addr_str = f"${addr:0{addr_width}X}"
        lines.append(f"{addr_str}  {instr}")

    return "\n".join(lines)


def parse_registers(response: str) -> Dict[str, int]:
    """
    Parse register response from serial monitor.

    Expected format: A=42 X=00 Y=FF SP=01FE PC=2000 P=30
    """
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
