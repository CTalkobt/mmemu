"""
mmemu-client: Python client library for MEGA65 emulator (mmemu) serial monitor.
"""

from .client import MMemuClient
from .utils import parse_addr, format_regs, format_memory, format_registers_verbose

__version__ = "0.1.0"
__all__ = [
    "MMemuClient",
    "parse_addr",
    "format_regs",
    "format_memory",
    "format_registers_verbose",
]
