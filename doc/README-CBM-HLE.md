# CBM KERNAL HLE Plugin

The `cbm-hle` plugin provides High-Level Emulation (HLE) for Commodore KERNAL disk operations. It intercepts calls to the KERNAL jump table and satisfies them directly using the host's filesystem, bypassing the serial bus protocol.

## Features

- **Instant Load**: Intercepts `LOAD` ($FFD5) and injects file data directly into RAM.
- **Flat Directory Mapping**: Maps a host directory to the virtual disk drive.
- **Bypass Protocol**: No bit-banging required; works even if no disk drive is emulated.

## Installation

The plugin provides the intercept type `cbm.kernal.fileio`; it does nothing until a machine's configuration installs it. `machines/c64.json` and `machines/vic20.json` put it on their KERNAL ROM entry, so it fires only while the KERNAL is banked in:

```json
"intercepts": [
  { "type": "cbm.kernal.fileio", "name": "kernal-fileio",
    "load": "$FFD5", "save": "$FFD8", "path": ".",
    "zeroPage": { "status": "$90", "fnlen": "$B7", "sa": "$B9", "fa": "$BA", "fnadr": "$BB" } }
]
```

The addresses and KERNAL variables are settings, so another KERNAL can be described in its machine file. The PET, the MEGA65 and the Atari do not install it (the PET's KERNAL variables differ and are not configured yet; the MEGA65's drives are its F011).

## Intercepted Vectors

- `load` (`$FFD5`): loads a file from the host folder. Secondary address 0 loads to X/Y, otherwise to the file's own address; the file's first two bytes are its load address either way.
- `save` (`$FFD8`): writes the file to the host folder: the 2-byte start address, then memory from the start pointer (the zero-page address in A) up to X/Y. As on a CBM drive, an existing file is replaced only with an `@:` (or `@0:`) prefix; otherwise it is left alone, SAVE still returns normally (a real drive reports 63 FILE EXISTS on its command channel only) and a warning is logged. A `0:` drive prefix and a `,P,W` suffix are ignored. No name: KERNAL error 8 (MISSING FILE NAME); a host write failure: error 5.
- Units below 8 (tape, printers) and units an emulated drive answers (an I/O device's `getDiskStatus`, e.g. the C64's virtual IEC drive 8 or the MEGA65's F011 units 8/9) are left to the KERNAL.

## Configuration at run time

- `hle on` / `hle off`: enable or disable this machine's KERNAL intercept.
- `hle path <path>`: the host folder.
- `intercept list`, `intercept on|off kernal-fileio`, `intercept set kernal-fileio path <dir>`: the general form.

## Implementation Details

`KernalHLE` is an `IPcIntercept` (`src/libcore/main/intercept.h`). The CPU core tests the machine's intercept table before fetching each opcode; at an armed address the intercept:
1. Reads the KERNAL variables at the configured zero-page addresses.
2. Performs the operation with host file I/O.
3. Updates the CPU registers (A, X, Y), carry and the status byte.
4. Performs an `RTS` by pulling the return address from the stack.
5. Returns `true`, so the CPU does not execute the ROM routine.

## Consistency with Virtual IEC

The KERNAL HLE operates at **Level 1 (API-level)**, while the Virtual IEC device operates at **Level 2 (Protocol-level)**.

- If HLE is **enabled** and no emulated drive answers the unit, the intercept fires first and the operation completes instantly.
- If HLE is **disabled**, or if the device number does not match any host mapping, execution continues into the KERNAL ROM, which will then use the serial bus protocol to communicate with any attached devices (including the `VirtualIECBus` device).
