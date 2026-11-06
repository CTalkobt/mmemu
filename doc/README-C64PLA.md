# mmemu-plugin-c64-pla: C64 PLA Banking Controller

The C64 PLA (Programmable Logic Array) is the hardware component responsible for memory banking on the Commodore 64. It is implemented as an `IOHandler` plugin within `mmemu-plugin-c64-pla.so`.

---

## 1. Intent

The C64 PLA decodes the three control signals from the MOS 6510 I/O port (LORAM, HIRAM, CHAREN) and the expansion port's EXROM and GAME lines, and determines which combination of RAM, ROM, cartridge ROM and I/O registers is visible on every memory access.

---

## 2. Functionality

### 2.1 Banking Regions

The memory configuration is `LORAM | HIRAM << 1 | CHAREN << 2 | EXROM << 3 | GAME << 4`, where EXROM and GAME are 1 while the cartridge pulls the line low (VICE's `mem_config`). The map of all 32 configurations follows VICE (`c64meminit.c`, `c64mem.c`):

| Region | Visible | When |
|--------|---------|------|
| $8000-$9FFF | ROML | LORAM and HIRAM with an 8 or 16 KB game; always in Ultimax |
| $A000-$BFFF | BASIC ROM | LORAM and HIRAM, no cartridge or an 8 KB game |
| $A000-$BFFF | ROMH | HIRAM with a 16 KB game |
| $D000-$DFFF | I/O (VIC-II, SID, CIAs, colour RAM) | CHAREN and (LORAM or HIRAM); always in Ultimax |
| $D000-$DFFF | Character ROM | not CHAREN and (LORAM or HIRAM); a 16 KB game needs HIRAM |
| $E000-$FFFF | KERNAL ROM | HIRAM, not Ultimax |
| $E000-$FFFF | ROMH | Ultimax |
| $1000-$7FFF, $A000-$CFFF | open | Ultimax: reads give $FF (VICE returns the VIC-II's last fetch), writes are dropped |

Everything else is flat RAM. Without a cartridge the usual `$01` settings come out as: `$37` BASIC, I/O, KERNAL; `$36` RAM, I/O, KERNAL; `$35` RAM and I/O only; `$33`/`$31` the character ROM at $D000; `$34`/`$30` all RAM.

Writes under a ROM go to the RAM behind it. Writes to $D000-$DFFF reach the I/O chips only while I/O is mapped there; otherwise they go to RAM.

### 2.1.1 Cartridges

`setCartridgePort(roml, romlSize, romh, romhSize, exrom, game)` (an `IOHandler` hook) plugs a cartridge in; all null/false takes it out. The `.crt` handler in `mmemu-plugin-cbm-loader.so` calls it on every device, so the VIC-II gets it too: in Ultimax mode the VIC-II sees ROMH's upper 4 KB at $3000-$3FFF of each bank and no character ROM.

### 2.2 Signal Sources

The three signals are `ISignalLine*` outputs from the `MOS6510` I/O port:

```cpp
pla->setSignals(cpu6510->signalLoram(),
                cpu6510->signalHiram(),
                cpu6510->signalCharen());
```

If a signal line is null (not connected), the PLA treats it as high (ROM-visible default).

### 2.3 ROM Injection

ROM images are injected by the machine factory after ROM loading:

```cpp
pla->setBasicRom (basicBuf,  8192);
pla->setKernalRom(kernalBuf, 8192);
pla->setCharRom  (charBuf,   4096);
```

Writes to ROM regions fall through to the flat RAM behind them, as on the real C64.

### 2.4 IORegistry Dispatch Order

The PLA registers with `baseAddr() = 0x1000`, so the bus asks it about every access from $1000 up (ROML and the Ultimax holes are below $A000); it answers RAM reads there itself. The `IORegistry` sorts handlers by base address, so the PLA comes before the I/O devices at $D000. When it returns `false` for $D000-$DFFF (I/O mapped), the IORegistry continues to the VIC-II, SID and CIA handlers.

---

## 3. Dependencies

- **Depends on**: `ISignalLine` (from `mmemu-plugin-6502.so`, via `MOS6510`)
- **Required by**: C64 Machine Plugin (Phase 11.6)

---

## 4. Implementation Details

- **Source Location**: `src/plugins/devices/c64_pla/main/c64_pla.h/cpp`
- **Plugin**: `lib/mmemu-plugin-c64-pla.so`
- **Class**: `C64PLA : public IOHandler`
- **Tests**: `src/plugins/devices/c64_pla/test/test_c64_pla.cpp`
- **Registration Name**: `"c64pla"`
- **Base Address**: `0x1000` (the lowest address it redirects; IORegistry sort ordering)
- **Address Mask**: `0xFFFF` (range checked internally per banking region)
