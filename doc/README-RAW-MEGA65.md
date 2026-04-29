# rawMega65 Machine

The `rawMega65` machine is a minimal, non-cycle-accurate MEGA65 target designed for 45GS02 CPU development and automated opcode validation. It lacks the complex VIC-IV, SID, and CIA components of the full MEGA65, providing only a large flat memory space and basic debugging I/O.

---

## 1. Specifications

- **CPU**: 45GS02 (MEGA65 CPU).
- **RAM**: 256 MB (28-bit address space).
- **ROM**: None (RAM-only execution).
- **I/O Devices**:
  - **HyperSerial** (`$D6C0`): Character logging to `hyper_serial.log`.
  - **ExitTrap** (`$D6CF`): Triggered by writing `$42`.
  - **Math Accelerator** (`$D760`): Fast 32-bit integer multiplication and division. (See [README-MEGA65-MATH.md](README-MEGA65-MATH.md))

---

## 2. Usage

This machine is intended for use with the `mmsim` validation suite:

```bash
mmemu-cli create rawMega65
load my_test.prg
run $0810
```

It is defined in `machines/rawMega65.json`.

---

## 3. Memory Map

| Range | Description |
|-------|-------------|
| `$0000 0000 – $0FFF FFFF` | 256 MB Flat RAM |
| `$0000 D6C0 – $0000 D6CF` | I/O Devices (HyperSerial & ExitTrap) |
| `$0000 D760 – $0000 D77F` | Math Accelerator |
