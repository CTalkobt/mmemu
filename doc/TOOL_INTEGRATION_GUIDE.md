# MEGA65 Tool Integration Guide

This guide explains how to use MMEMU's serial monitor server with existing MEGA65 development tools like m65 CLI.

## Quick Start

### 1. Start MMEMU with Serial Monitor Server

```bash
./bin/mmemu-cli -m mega65 --serial-monitor-port 2000
```

### 2. Connect with netcat (Quick Test)

```bash
# In another terminal
nc localhost 2000

# Try commands:
r              # Read registers
m 2000         # Read memory at $2000
d 4000         # Disassemble from $4000
?              # Help
```

Press Ctrl-D to disconnect.

## Tool Integration Patterns

### Pattern 1: Direct TCP Connection (Simple)

Tools that support TCP socket connections can connect directly:

```bash
# Python client
python3 -c "
import socket
s = socket.socket()
s.connect(('localhost', 2000))
s.sendall(b'r\n')
print(s.recv(1024).decode())
s.close()
"
```

### Pattern 2: UART Bridge with socat (Real Hardware)

Use socat to bridge MMEMU to real MEGA65 over UART:

```bash
# Terminal 1: Start MMEMU
./bin/mmemu-cli -m mega65 --serial-monitor-port 2000

# Terminal 2: Create socat bridge
socat TCP:localhost:2000 /dev/ttyUSB0,b2000000

# Terminal 3: Use m65 CLI tool (connects to real MEGA65 via socat)
m65 --serial-port /dev/pts/N  # (or configure m65 to use socat pipe)
```

### Pattern 3: TCP to TCP Bridge

Bridge two TCP connections (e.g., MMEMU to a remote tool):

```bash
socat TCP:localhost:2000 TCP:remote-tool.example.com:2000
```

## Running Integration Tests

MMEMU includes Python-based integration tests for the serial monitor protocol:

```bash
# Start MMEMU in one terminal
./bin/mmemu-cli -m mega65 --serial-monitor-port 2000

# Run tests in another terminal
python3 tests/serial_monitor_integration_test.py

# Custom host/port
python3 tests/serial_monitor_integration_test.py --host 192.168.1.100 --port 3000
```

### Test Coverage

The integration tests verify:

**Phase 1 (Core Commands)**
- `R` - Register read
- `M` - Memory read
- `D` - Disassemble
- `?` - Help
- `G` - Set PC
- `S` - Set memory
- `B` - Breakpoint

**Phase 2 (Advanced Commands)**
- `E` - Flag watch (N, V, C, Z flags)
- `I` - Interrupt control and status
- `@` - CPU memory view
- `T` - Trace control
- `Z` - CPU history

**Error Handling**
- Invalid commands
- Malformed arguments
- Error response format

## Workflow Examples

### Debugging a Program

```bash
# Terminal 1: Start MMEMU with serial monitor
./bin/mmemu-cli -m mega65 --serial-monitor-port 2000
c64_program.prg

# Terminal 2: Debug via serial monitor
nc localhost 2000

# Get current state
r

# Set breakpoint at program start
b 2000

# Continue execution
(in MMEMU, press 'run')

# When breakpoint hits, examine memory
m 2000

# Disassemble around PC
d

# Check CPU flags
e z
e c
e n
```

### Memory Inspection Workflow

```bash
# Read 256 bytes from address $2000
m 2000

# Continue reading next page
m

# Read specific address
m 4000

# Write single byte
s 2050 42

# Verify write
m 2050
```

### Trace and History Analysis

```bash
# Enable instruction tracing
t on

# Run program...

# Dump last 16 instructions
t dump

# Get full CPU history (last 32 instructions)
z

# Check interrupt status
i status

# Disable interrupts for testing
i off
```

## Advanced: Creating Custom Tool Bridges

### Python Bridge Example

```python
#!/usr/bin/env python3
"""
Simple serial monitor bridge that can be used by m65 CLI or other tools.
"""

import socket
import sys
from threading import Thread

def bridge(mmemu_host='localhost', mmemu_port=2000, local_port=2001):
    """Bridge local socket to MMEMU serial monitor."""
    
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('127.0.0.1', local_port))
    server.listen(1)
    
    print(f"Bridge listening on port {local_port}, forwarding to {mmemu_host}:{mmemu_port}")
    
    def handle_client():
        client, addr = server.accept()
        print(f"Client connected: {addr}")
        
        mmemu = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        mmemu.connect((mmemu_host, mmemu_port))
        
        # Forward client → MMEMU
        def client_to_mmemu():
            try:
                while True:
                    data = client.recv(4096)
                    if not data:
                        break
                    mmemu.sendall(data)
            finally:
                client.close()
        
        # Forward MMEMU → client
        def mmemu_to_client():
            try:
                while True:
                    data = mmemu.recv(4096)
                    if not data:
                        break
                    client.sendall(data)
            finally:
                mmemu.close()
        
        Thread(target=client_to_mmemu, daemon=True).start()
        Thread(target=mmemu_to_client, daemon=True).start()
    
    while True:
        handle_client()

if __name__ == '__main__':
    bridge()
```

Save as `bridge.py` and run:
```bash
python3 bridge.py
# Now tools can connect to localhost:2001 instead of 2000
```

## Performance Notes

### Baud Rate Characteristics

Default rate: **2,000,000 bps** (200 KB/sec theoretical)

- Single byte: ~5 µs
- 256-byte block: ~1.2 ms
- 64 KB page: ~320 ms
- Full 256 KB dump: ~1.3 sec

Adjust with the `+` command:
```bash
# Slow down to 19,200 bps for compatibility
+ 83c

# Reset to default 2 Mbps
+ 14
```

### Connection Persistence

- Commands are stateless (each command independent)
- Breakpoints, watchpoints persist across commands
- Trace buffer maintains history within session
- Session ends on disconnect

## Troubleshooting

### "Connection refused"
MMEMU is not running or port is different:
```bash
# Check if running
netstat -an | grep 2000

# Start with verbose logging
./bin/mmemu-cli -m mega65 --serial-monitor-port 2000 -v
```

### Slow Response Times
Check baud rate and network:
```bash
# Query current divisor
nc localhost 2000
echo "+0" # Shows current setting
```

### Tools Not Finding Serial Port
If using socat bridge, ensure:
1. `/dev/ttyUSB0` exists (USB adapter connected)
2. Permissions allow access: `chmod 666 /dev/ttyUSB0`
3. Correct baud rate: `socat ... /dev/ttyUSB0,b2000000`

## See Also

- [Serial Monitor Protocol Specification](SERIAL_MONITOR_PROTOCOL.md)
- `tests/serial_monitor_integration_test.py` - Protocol compliance tests
- MEGA65 Book: Chapter 12 (Remote Debugging and Development)
- m65 CLI Tool: https://github.com/mega65/mega65-tools
