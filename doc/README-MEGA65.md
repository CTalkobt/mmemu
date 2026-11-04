# MEGA65 Machine

The MEGA65 is a modern, high-end 8-bit computer that is highly compatible with the Commodore 64 and Commodore 65. It features the powerful 45GS02 CPU, enhanced video (VIC-IV), and a wide range of advanced peripherals.

---

## 1. Specifications

- **CPU**: 45GS02 (running at 40 MHz in Fast mode).
- **Video**: VIC-IV (compatible with VIC-II and VIC-III, supports Full Colour Mode and 80-column text).
- **Audio**: Dual SID (6581/8580 compatible).
- **RAM**: 384 KB Chip RAM, expandable to 128 MB Attic RAM.
- **I/O**:
  - Dual CIA 6526 for legacy C64 compatibility.
  - F018B DMA Controller.
  - Math Acceleration Registers (Hardware Multiply/Divide).
- **Bus**: 28-bit physical address space (256 MB).

---

## 2. Memory Map (MEGA65 Mode)

| Range | Description |
|-------|-------------|
| `$0000 0000 – $000F FFFF` | 1 MB Chip RAM |
| `$0002 0000 – $000D FFFF` | ROM area (MEGA65.ROM) |
| `$0000 D000 – $0000 DFFF` | I/O Area (VIC-IV, SIDs, CIAs, DMA) |
| `$0FF8 0000 – $0FF8 7FFF` | 32 KB Colour RAM |

---

## 3. CPU Core (45GS02)

The MEGA65 uses the 45GS02 CPU, which features:
- **MAP**: Dynamic 28-bit memory mapping.
- **32-bit Ops**: Quad registers and arithmetic.
- **Base Page**: Relocatable zero-page.

See [README-45GS02.md](README-45GS02.md) for details.

---

## 4. Peripherals

### 4.1 VIC-IV Video
The VIC-IV supports traditional C64 modes plus advanced features:
- **H640**: 80-column text mode.
- **FCM**: Full Colour Mode (8x8 glyphs with unique colours per pixel).
- **Palette**: 256 entries from a 4096-colour space.

### 4.2 F018B DMA Controller
The F018B handles high-speed memory transfers within the 28-bit address space:
- **Copy**, **Fill**, **Swap**, and **Mix** operations.
- Fractional step sizes for non-contiguous transfers.
- Overlap-safe copying (automatic backward copy when source and destination overlap).
- Enhanced DMA mode with extended option lists.

### 4.3 Math Accelerator
Hardware registers at `$D760–$D77F` provide fast 32-bit integer multiplication and division.

See [README-MEGA65-MATH.md](README-MEGA65-MATH.md) for detailed register maps and usage.

### 4.4 Hypervisor (HYPPO)
The MEGA65 includes a privileged hypervisor mode implemented in the 45GS02 CPU:
- **HYPPO ROM** (`HICKUP.M65`, 16KB) is loaded at `$8000-$BFFF` in hypervisor mode.
- On reset, the CPU enters hypervisor at `$8100` (rather than reading the standard reset vector).
- **SYSCALL traps** ($D640-$D67F): user-mode writes trigger hypervisor entry at computed trap addresses.
- **Virtualisation control registers** ($D640-$D67F): in hypervisor mode, these provide read/write access to saved CPU state.
- Writing `$D67F` exits hypervisor mode and restores user-mode state.

The machine factory searches for `HICKUP.M65` in the `hyppo` paths of `machines/mega65.json` (`roms/mega65/`, the current directory, `~/.local/share/mmsim/`); `importmega65` extracts it from an installed xemu. If not found, the machine falls back to the standard C64 reset vector.

#### What booting to `READY.` needs

The boot follows the hardware: reset traps to HYPPO at `$8100`; HYPPO mounts the SD card, loads `CHARROM.M65` into the character WOM (`$FF7E000`) and `MEGA65.ROM` into `$20000-$3FFFF`, and leaves through `$D67F` to the ROM's reset vector. The emulator does not patch HYPPO or the ROM, and nothing depends on addresses inside them. You need:

- **`HICKUP.M65`** (above).
- **An SD card image** with `MEGA65.ROM` on it: the `sdcard` paths in `machines/mega65.json`, e.g. xemu's `~/.local/share/xemu-lgb/mega65/mega65.img`. Without one, HYPPO stops at its ROM menu.
- **The repository's `machines/` directory**: `machines/mega65.json` and the `roms/` paths are looked up relative to the current directory first, then `~/.local/share/mmsim/` and `/usr/local/share/mmsim/`. Run from elsewhere and an old copy there may be used; plugins likewise come from `./lib` first, then `~/.local/lib/mmsim/plugins`.

A host ROM file (`mega65rom` paths, e.g. `roms/mega65/MEGA65.ROM`) is optional: it is preloaded at `$20000` for the no-HYPPO fallback, and HYPPO replaces it with the SD card's copy. ROM banking (`$01`, `$D030`) and the C64-mode character ROM read the RAM at `$20000-$3FFFF`; the VIC-IV draws characters from the character WOM.

### 4.5 Floppy drives (F011)

The F011 controller (`$D080-$D09F`, `src/plugins/devices/sdcard/main/f011.cpp`) runs drives 0 and 1 (units 8 and 9) for BASIC 65's DOS, in MEGA65 and C64 mode. It follows xemu's `f011_core.c`: a command completes at once and moves a whole 512-byte sector between the disk and the sector buffer at `$FFD6C00`.

- **The SD card's D81**: HYPPO mounts `MEGA65.D81` from the SD card at boot, through `$D68B` (bits 0-2 / 3-5: image enabled, present, writable) and the start sector in `$D68C-$D68F` / `$D690-$D693`. These registers can only be changed in the hypervisor. Bit 2 / 5 is set when the image may be written: the VHDL (`sdcardio.vhdl`) returns "not write protected" there, although the book labels it D0WP.
- **A host `.d81`**: CLI `disk mount 8 <file.d81>` (`disk eject 8`), the GUI's Drive Status pane, or MCP attaches an 800 KB image file to unit 8 or 9; it replaces the SD card's image until ejected, and writes go straight to the file. Swapping disks sets the disk-changed signal, so the DOS rereads the directory.
- **Disk buffers**: the 4 KB at `$FFD6000` hold the F011 buffer (`+$C00`) and the SD card buffer (`+$E00`). `$DE00` (after SD command `$81`) shows the one `$D689` bit 7 selects, except in the hypervisor, which always sees the SD card buffer.
- With no disk, `DIR` ends with `74,DRIVE NOT READY`.

The MEGA65 installs no KERNAL intercept (`cbm.kernal.fileio`), so `LOAD`/`SAVE` always go through the ROM's DOS.

### 4.6 HYPPO call intercepts

Before HYPPO sees a user-mode trap, the machine's call intercepts may serve it (`machines/mega65.json`, `"intercepts"`; format in `machines/README-machines.md`). The default entry, `mega65.hdos` (`src/plugins/devices/mega65_hypervisor/main/hdos_handler.cpp`), serves these DOS calls (trap `$00`) from a host folder, the first of its `paths` that exists: get/select drive, disk size, chdir, opendir/readdir/closedir, openfile/readfile/closefile, closeall, findfile, loadfile, get last error, cdrootdir and loadfile-to-attic.

- **setname** (`$2E`) and **set transfer area** (`$3A`) are noted and still passed to HYPPO: the name is read from user memory at page Y, as HYPPO reads it, so findfile, openfile and loadfile see it. Keep both listed.
- `intercept set hdos filename <NAME>` overrides the name (empty: off); `intercept set hdos path <dir>` changes the folder; `intercept off hdos` hands every call to HYPPO.
- Remove a function from `functions` to let HYPPO serve it from the SD card; `"intercepts": []` for none. Without an `"intercepts"` key the machine uses this default list.

Not emulated: real floppy drives (`$D6A0`), 64 MB MEGA disk images (`$D68B` bits 6-7), D64/D71 image modes, the FDC interrupt, and the delays of a real drive.

See [README-45GS02.md](README-45GS02.md) for detailed hypervisor register maps.

---

## 5. Development Status in mmsim

The MEGA65 implementation includes the **45GS02 CPU** with hypervisor support, **VIC-IV** video (VIC-II/III/IV modes, FCM, NCM), **F018B DMA**, **MAP MMU**, **Math Accelerator**, dual SID, dual CIA, and **HYPPO hypervisor ROM** loading. Integration testing and further peripheral work is ongoing.
