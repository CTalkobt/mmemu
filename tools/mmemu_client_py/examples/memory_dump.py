#!/usr/bin/env python3
"""
Memory dump tool - Extract memory regions from mmemu.

Usage:
    python3 memory_dump.py [--host localhost] [--port 6502] [--addr $2000] [--size 256] [--output dump.bin]
"""

import argparse
import sys
from pathlib import Path

# Add parent directory to path for development
sys.path.insert(0, str(Path(__file__).parent.parent))

from mmemu_client import MMemuClient, parse_addr, format_memory


def main():
    parser = argparse.ArgumentParser(
        description="Dump memory from mmemu emulator"
    )
    parser.add_argument(
        "--host", default="localhost", help="Server hostname (default: localhost)"
    )
    parser.add_argument(
        "--port", type=int, default=6502, help="Server port (default: 6502)"
    )
    parser.add_argument(
        "--addr",
        default="$0000",
        help="Start address (default: $0000, supports $XXXX, 0xXXXX, decimal)",
    )
    parser.add_argument(
        "--size",
        type=int,
        default=256,
        help="Number of bytes to dump (default: 256)",
    )
    parser.add_argument(
        "--output", help="Output file (if not specified, dumps to stdout)"
    )
    parser.add_argument("--hex", action="store_true", help="Output as hex dump")
    parser.add_argument("--binary", action="store_true", help="Output as binary")

    args = parser.parse_args()

    try:
        addr = parse_addr(args.addr)
    except ValueError:
        print(f"Error: Invalid address '{args.addr}'", file=sys.stderr)
        return 1

    try:
        with MMemuClient(args.host, args.port) as client:
            print(f"Reading ${addr:04X}..${addr + args.size - 1:04X} ({args.size} bytes)...")
            data = client.read_memory(addr, args.size)
            print(f"Read {len(data)} bytes")

            if args.binary:
                # Write binary
                if args.output:
                    with open(args.output, "wb") as f:
                        f.write(data)
                    print(f"Wrote to {args.output}")
                else:
                    sys.stdout.buffer.write(data)
            elif args.hex:
                # Output as hex string
                hex_str = " ".join(f"{b:02X}" for b in data)
                if args.output:
                    with open(args.output, "w") as f:
                        f.write(hex_str + "\n")
                    print(f"Wrote to {args.output}")
                else:
                    print(hex_str)
            else:
                # Default hex dump format
                dump = format_memory(data, addr)
                if args.output:
                    with open(args.output, "w") as f:
                        f.write(dump + "\n")
                    print(f"Wrote to {args.output}")
                else:
                    print(dump)

            return 0

    except ConnectionError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
