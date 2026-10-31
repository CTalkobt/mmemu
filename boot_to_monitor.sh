#!/bin/bash
# Boot MEGA65 into the monitor by holding Run/Stop during startup

cat << 'EOF' | ./bin/mmemu-cli -m mega65 2>&1 | grep -v "^\[MapMmu\]\|^Warning:" | tail -100
key RUNSTOP down
step 100
key RUNSTOP up
run 100000
regs
disasm
EOF
