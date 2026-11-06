# CHANGELOG.md - mmsim (Multi-Machine Simulator)

All notable changes to this project will be documented in this file.

Version format: `MAJOR.MINOR.PATCH.GITHASH` (e.g., `0.4.0.abc1234`).
The canonical version is defined in the `VERSION` file at the repository root.

## [0.4.0] - Unreleased

### Repository
- `mmsim.dump` and `xmega65.dump`, written by `tests/45gs02/validate.py` during `make test`, are no longer tracked and are now in `.gitignore` (they had been committed by accident again in #205).

### Commodore 128
- **C128 internal function ROM socket** (#224): new ROM sockets on the machine (`MachineDescriptor::romSockets`): CLI `rom [socket] [file|remove]`, GUI Control > ROM Sockets..., MCP `rom_socket`. The C128's `function` socket takes the internal function ROM (a raw 1-32 KB image or a C128 `.crt`), and the machine resets. Tests: `c128_function_roms` (now fits it through the socket and empties it).

### Atari 2600
- **Atari 2600** (#11): profiles `atari2600` and `atari2600-pal` (new plugin `mmemu-plugin-atari2600.so`), after Stella (GPL-2.0-or-later). The 6507 (the 6502 core with 13 address lines); a port of Stella's TIA (playfield, players, missiles, ball, the delay queue, HMOVE, collisions, fire buttons and paddle readout, sound, NTSC/PAL/SECAM palettes, the VSYNC-driven frame) and RIOT 6532 (`M6532.cxx`: RAM, ports A/B, the interval timer, the PA7 edge flag). The TIA and RIOT are clocked lazily, up to the cycle of each read, with writes applied on the instruction's last cycles; WSYNC holds the CPU to the end of the line. Cartridges `.a26`/`.bin` with Stella's type detection: 2K, 4K, F8/F6/F4, their Superchip variants, E0, 3F, FA. Joysticks on SWCHA/INPT4-5; console switches on F1-F8 as in Stella, GUI buttons RESET and SELECT. See `doc/README-A2600.md`. Tests: `riot6532_*`, `tia_*` (15), `a2600_boot_ram`, `a2600_wsync`, `a2600_f8_banks`, `a2600_inputs`.
- **GUI: `-i` attaches cartridges**: an image given with `-i` (or File > Load Image) that no image loader takes is now tried as a cartridge for the current machine.

### Commodore 128
- **C128 1571 fast serial** (#22): the C128's drive is a 1571 on fast serial: the KERNAL's announce byte before ATN and the drive's answer mark it fast (`$0A1C`); data bytes then go through CIA1's shift register in the usual handshake; LOAD uses the burst fastload (`U0` `$1F`); the burst commands inquire disk, read and write (several sectors 5 on round the track, as the C128's CP/M expects) on disk images. CP/M boots about four times faster and reads and writes on fast serial; C64 mode stays slow. Worked out from the C128 KERNAL (`$E33B-$E5FA`, `$F3A1-$F502`), CP/M's sources and the burst command set. New: CIA 6526 serial data register (stored, an output hook, `receiveSerialByte`), `IecDrive::setFastSerial`, burst replies in the DOSes (a binary `U0` command runs at UNLISTEN, not at a CR). Tests: `image_dos_burst_commands`, `c128_fast_serial_load`.
- **C128 function ROMs and cartridges** (#22): the internal and external (cartridge) 32 KB function ROMs where CR picks them, after VICE (`functionrom.c`, `c128cart.c`, `c128/cart/generic.c`); C128 cartridges from raw 1-32 KB `.bin` images (mirrored) or `.crt` files (`C128 CARTRIDGE  `, generic, CHIPs at `$8000`/`$C000`); the KERNAL's reset poll starts one marked `$01` at `$xx06`. CLI `cart`/`eject`, GUI Attach Cartridge and the Cartridge pane, MCP `attach_cartridge`. Tests: `c128_function_roms`.
- **Cartridges: one extension, several machines**: new `ImageLoaderRegistry::attachCartridge` tries every handler registered for the file's extension on the current machine and keeps the first that attaches (a `.bin` is a Plus/4 or a C128 cartridge); the CLI, GUI, MCP and snapshot restore use it. Tests: `cartridge_attach_tries_each_handler`.
- **Disk images: writing files** (#22): SAVE, OPEN `,S/P/U,W` (written on CLOSE), `@0:` replace, `,A` append, `S0:` scratch and `R0:` rename on D64/D71/D81/D80/D82 images, written back to the image file. Blocks come from the BAM as in VICE (`vdrive-bam.c`): the first next to the directory track, then the drive's interleave; a full directory sector chain grows on the directory track; DOS errors 63 FILE EXISTS, 72 DISK FULL, 01 FILES SCRATCHED. For every machine that mounts images (PET, Plus/4/C16, C128). Checked from C128 BASIC on a blank D64 (`SAVE`, `DLOAD`, `DIRECTORY`; the file at 17/0 as a 1541 puts it, BAM counts consistent). Tests: `ieee488_disk_image_writes_files`; the write-protection test now uses a read-only file.
- **Disk images: block writes** (#22): `U2`/`UB` and `B-W` write a `#` buffer to a track and sector, straight back to the image file (as VICE does); read-only image files answer `26,WRITE PROTECT ON`. CP/M on the C128 can now change its disks (checked: `REN` on a copy of the 28 May 87 boot disk). Saving files (SAVE, `,W`) on images is still write protected. Tests: `image_dos_block_write`.
- **C128 Z80 and CP/M 3.0** (#22): the C128 now powers on through its Z80 boot BIOS and hands over to the 8502 through MCR bit 0, which switches the CPUs at any time; the Z80's view of memory and its IN/OUT ports as in VICE (`z80mem.c`); /IRQ to both CPUs; the Z80 at an effective 2 MHz. A C128 CP/M boot disk boots to `A>` in 40 or 80 columns and runs DIR (checked with Commodore's 28 May 87 disk; no disk is included). New CLI `cpu [n|name]` lists the CPUs and picks the one `regs`/`disasm`/`m` show; the CLI debugger now observes every CPU. Typed keys on the C128 are held ~100 ms (CP/M's scan missed shorter presses). Tests: `c128_z80_view`.
- **Disk images: direct access** (#22): `#` buffer channels with `U1`/`UA` and `B-R` block reads and `B-P` (what the C128's boot sector and CP/M use); block writes still report WRITE PROTECT ON. Tests: `image_dos_direct_access`.
- **Zilog Z80 core** (#22): new plugin `mmemu-plugin-z80.so` with the `Z80` core (`z80`), its disassembler and a `rawz80` machine. All documented and undocumented instructions, the undocumented X/Y flags, MEMPTR, the SCF/CCF Q latch, T-state timing, interrupt modes 0-2, NMI, HALT and the EI delay, 16-bit I/O ports. Frank Cringle's ZEXDOC and ZEXALL (GPL, now in `tests/z80/`) pass every group (`MMEMU_ZEX=1 bin/mmemu-test z80_zex`). See `doc/README-Z80.md`. Not yet wired into the C128.
- **C128 C64 mode** (#22): `GO64`, or C= held at reset, switches to C64 mode (MCR bit 6) as in VICE: the C64 memory map from the 8502 port with BASIC/KERNAL 64 and the C64 character set, RAM in the bank CR held, RCR's shared areas from bank 0, no MMU (open `$D500`, plain `$FF00`), the VDC and `$D02F`/`$D030` still there, the first colour RAM half; a reset returns to C128 mode. The C64-mode ROM is now the US 251913-01 (`roms/c128/c64.251913-01.bin`, the first half of 318023-02, identical to the C64's ROMs) instead of 325182-01, a national version whose keyboard tables turned `;` into `.` on the US keyboard. Keys held through a reset stay down (only typed text is released). Tests: `c128_c64_mode`, `c128_c64_mode_memory`.
- **C128 80-column screen** (#22): the VDC draws its display (after VICE `vdc-draw.c`, `vdc.c`): text and bitmap modes, attributes (colour, alternate set, reverse, underline, blink), monochrome, 40-column double-pixel mode, cursor and its blink, reverse screen, semigraphics, the R22 displayed width, smooth scroll, the RGBI palette, frame timing from the registers (and the status vertical-blank bit from it). The frontends show the VIC-II's 40 columns or the VDC's 80 by the 40/80 key, as VICE's single-window builds do; the screen editor starts in 80 columns with the key down at reset. GUI screen buttons: 40/80, RUN/STOP + RESTORE, RESTORE. Tests: `vdc_render_text`, `vdc_render_bitmap`, `c128_boots_80_columns`.
- **Commodore 128, C128 mode** (#22): profiles `c128` and `c128-ntsc` (new plugin `mmemu-plugin-c128.so`), booting to BASIC 7.0 READY on the 40-column screen. After VICE (`c128mem.c`, `c128meminit.c`, `c128mmu.c`): the 8722 MMU with CR at `$FF00`/`$D500`, the preconfiguration registers, 128 KB RAM in two banks, shared RAM, page 0/1 relocation; the 8502 (the 6510 core with a seventh port pin, CAPS LOCK); the VIC-IIe (`$D02F` keyboard lines, `$D030` 2 MHz, with BASIC `FAST` running the CPU at 2 MHz), 2 KB colour RAM in two halves, the C128 character set; SID, two CIAs, the C128 keyboard (HELP, ESC, TAB, ALT, LINE FEED, NO SCROLL, keypad, cursor keys, CAPS LOCK, 40/80), joysticks, RESTORE, a serial drive on unit 8 and the datasette. See `doc/README-C128.md`.
- **MOS 8563 VDC, registers and memory** (#22): new `Vdc8563` (after VICE `vdc-mem.c`): the `$D600`/`$D601` interface, 16 or 64 KB RAM with R28 address translation, auto-increment, block fill and copy, status. The 80-column display is not emulated yet.
- **VIC-II**: with no character ROM of its own the VIC-II now fetches characters through its DMA bus (it read `$FF`), so a machine can map the character ROM itself (the C128 does).

### Commodore 64
- **C64 PLA: GAME/EXROM and all 32 memory configurations** (#213): the PLA now maps from VICE's tables (`c64meminit.c`, `c64mem.c`), with the cartridge's EXROM and GAME lines as configuration bits 3 and 4. 16 KB games (ROML at $8000, ROMH at $A000) and Ultimax cartridges (ROMH at $E000, open $1000-$7FFF and $A000-$CFFF, $01 ignored) now map; ROML follows LORAM/HIRAM. The `.crt` handler lays the CHIP packets out as VICE's generic cartridge does (8 KB, 16 KB, Ultimax) and hands them to the PLA and the VIC-II through a new `IOHandler::setCartridgePort`; the VIC-II sees ROMH at $3000-$3FFF of each bank in Ultimax mode. Cartridge ROM is no longer a bus overlay, so the VIC-II no longer sees it in its bank at $8000.
- **C64 PLA: $D000 with LORAM but not HIRAM**: `$01 = $35` now shows I/O and `$31` the character ROM, as on the real C64 (they showed RAM), and writes to $D000-$DFFF go to RAM instead of the I/O chips while RAM or the character ROM is mapped there. New tests: `test_c64_pla.cpp` (5).
- **C64 cartridges were never mapped**: `cart` reported "Attached" but the ROM never appeared, because the 6510's port wrapper (`MOS6510::PortBus`, the CPU's data bus, which the CLI, GUI and MCP hand to cartridge handlers) did not pass `addRomOverlay`/`removeRomOverlay` (or `isHaltRequested`) on to the system bus. An 8 KB cartridge at $8000 now shows and autostarts through CBM80. (Now mapped by the PLA: see below.)

### Commodore Plus/4 and C16
- **TED (MOS 7360/8360)** (#9), the Plus/4 and C16 video, sound, timer and keyboard-latch chip, ported from VICE (`ted*.c`, palette from `ted-color.c`/`video-color.c`): new plugin `mmemu-plugin-ted.so`, device type `ted`, for the Plus/4/C16 machine to come.
- **Plus/4 and C16 machines** (#9): profiles `plus4`, `plus4-ntsc`, `c16` and `c16-ntsc` (new plugin `mmemu-plugin-plus4.so`). The 7501 CPU runs on the TED clock, single clock during the display and stalled on bad lines; ROM banking through $FDD0 and $FF3E/$FF3F as in VICE (BASIC 3.5, KERNAL, 3-plus-1); keyboard through the $FD30 latch and $FF08; both joysticks. Both machines boot to READY and run BASIC; F1 starts 3-plus-1 on the Plus/4. See `doc/README-PLUS4.md`.
- **Plus/4 and C16 serial-bus drive** (#9): unit 8 is a line-level IEC drive (new `IecDrive`) on the 7501 port's ATN/CLK/DATA lines, driven by the KERNAL's own serial code, serving the current folder or `disk mount 8 <folder or D64/D71/D80/D81/D82>` through the PET's DOS (`CbmHostDos`/`CbmImageDos`). LOAD, SAVE, `DIRECTORY`, OPEN/PRINT#/INPUT# and the error channel work; a missing file gives FILE NOT FOUND.
- **MOS 6551 ACIA** (#9): the Plus/4's RS-232 at $FD00 (new `Acia6551`, after VICE's `aciacore.c`): registers, programmed reset, 50-19200 bps character timing, receive/overrun/transmit IRQs. New CLI `serial loopback|tcp:PORT|none` (and `IOHandler::attachSerial`) sets the far end; a TCP client becomes the modem. BASIC `OPEN 2,2,0,CHR$(8)+CHR$(5)` / `PRINT#2` / `GET#2` work through the KERNAL.
- **Plus/4 and C16 tape** (#9): a 1531 datasette on the cassette port ($01 bits 1, 3 and 4, sense on $FD10 bit 2), `tape mount|play|record|stoprecord|save`. New Plus/4 behaviour in the Datasette (VICE's C16 tape behaviour): the read line is a level flipping each half-wave; `C16-TAPE-RAW` version 2 TAPs play as half-waves, version 0/1 as full waves; recordings are saved as C16 version 2 TAPs. SAVE to tape and LOAD back work through the KERNAL.
- **Plus/4 and C16: $FF20-$FF3F reach RAM/ROM** (#9): the machine now passes on what the TED leaves to memory, as in VICE (`plus4mem.c`): reads of $FF20-$FF3D (and $FF3E/$FF3F while ROM is selected) give the ROM or RAM underneath instead of $FF, and writes to $FF20-$FF3D land in RAM. New machine tests for the ROM/RAM banking ($FF3E/$FF3F, $FDD0-$FDDF, $FC00-$FCFF always the KERNAL) and the C16's 16 KB mirroring.
- **TED graphics viewer** (#208): View > Graphics (TED) (pane `graphics-ted`), `gfx` and `render_graphics` on the Plus/4, C16 and C116. New formats `ted-char` and `ted-char-mc` (characters, or 8x8 bitmap cells); the TED implements `IVideoInspect` (121-colour palette, video-chip reads with ROM or RAM per $FF12 bit 2, and the character set or bitmap it is showing with its colours), so "chip-charset" follows $FF12/$FF13 and the text or bitmap mode.
- **Commodore 116** (#9): profiles `c116` and `c116-ntsc`, the C16 in its smaller case (16 KB, no 3-plus-1 or ACIA; VICE's "C16/116").
- **Plus/4, C16 and C116 cartridges** (#209): the expansion port's C1 low/high ROMs (bank 2 at $FDD0-$FDDF) and C2 (bank 3), from raw 8/16/32 KB `.bin` images or VICE `.crt` files (`PLUS4 CARTRIDGE `, generic), loaded as VICE does (`plus4cart.c`, `plus4-generic.c`). The KERNAL's reset poll finds and starts an autostart cartridge. CLI `cart`/`eject`, the GUI's Attach Cartridge (now also `*.bin`) and Cartridge pane, MCP `attach_cartridge`. See `doc/README-PLUS4.md`.
- **Cartridge factories can share an extension**: `ImageLoaderRegistry` keeps every factory registered for an extension and uses the first that accepts the file; the C64 `.crt` factory now turns down files without the `C64 CARTRIDGE` header, so the Plus/4's `.crt` files reach the Plus/4 handler.
- **KERNAL HLE reads RAM under ROM**: new `MachineDescriptor::readRam`, used by the `kernal-fileio` intercept for file names and SAVE data, because BASIC 3.5 keeps its strings and programs where the ROM is banked in.
  - Registers `$FF00-$FF3F` with VICE's read-back of unused bits; `$FF20-$FF3D` (and `$FF3E/$FF3F` while ROM is in) fall through to RAM/ROM as in VICE.
  - Three 16-bit timers at the single clock (timer 1 reloads from its latch, timers 2/3 run on; a low-byte write stops, a high-byte write starts), IRQ flags/enables/summary in `$FF09/$FF0A`, raster compare IRQ (9 bits; immediate when the current line is written), raster line `$FF1C/$FF1D` (writable) and horizontal position `$FF1E`, PAL 312 / NTSC 262 lines of 114 double-clock cycles.
  - Video a line at a time into a 384x288 (PAL) / 384x242 (NTSC) frame: standard text with 128/256 characters, flashing and the hardware cursor, multicolour and extended colour text, hires and multicolour bitmaps, illegal modes, idle state, X/Y scroll, 24/25 rows, 38/40 columns, display enable; VICE's attribute/character fetch sequence and row counters; character/bitmap data from ROM or RAM (`$FF12` bit 2) through the machine's fetch hook. The 121-colour palette as VICE computes it (PAL or NTSC).
  - Sound: two voices with 10-bit frequencies, noise on voice 2, volume, DA mode, as VICE generates it, at 44100 Hz.
  - Machine hooks: video fetch, `$FF08` keyboard/joystick latch, `$FF3E/$FF3F` ROM select; single-clock windows (`cpuSingleClock()`, also forced by `$FF13` bit 1) and the 86-cycle DMA of bad and attribute lines (`cpuBlocked()`), for the scheduler.
  - Not emulated: mid-line splits (changes apply per line), the colour-write grey dot, open side borders, light pen. See `doc/README-TED.md`.
  - Tests: `test_ted.cpp` (23): unused bits, `$FF20-$FF3F`, ROM select, keyboard latch, timers 1-3 and IRQ acknowledge, raster compare IRQ (incl. bit 8 and the current line), PAL/NTSC lines, horizontal position, palette entries against VICE's formula, text (reverse, scroll, 38 columns, 24 rows, display off, flash, cursor), multicolour/extended text, hires and multicolour bitmaps, character ROM fetches, single clock and DMA, voice/volume/noise/DA sound, device info.

### Debugger
- **RUN/STOP + RESTORE and RESTORE buttons** on the GUI screen pane, under Capture Keyboard, for the C64, VIC-20 and MEGA65 (the PET keeps its **Trigger NMI** button). Each machine lists its buttons (`"ui": {"screenButtons": [{"label", "keys", "tooltip"}]}`, `MachineDescriptor::screenButtons`, replacing `nmiButtonKey`); a button holds its keys while pressed, and at least 150 ms per click, since a quick click could press and release between two emulation slices and be missed. RUN/STOP + RESTORE warm-starts BASIC on all three (stops `10 GOTO 10`, clears the screen; checked in the CLI on the MEGA65, by tests on the C64 and VIC-20, and the buttons seen in the GUI). Tests: `c64_run_stop_restore_button`, `vic20_run_stop_restore_button`, `pet_nmi_button` (updated).
- **Graphics (VERA)** for the Commander X16 (pane id `graphics-vera`): sprites 8-64 pixels at 4 or 8 bpp (`vera-spr<W>-<B>`) and tiles 8/16 pixels at 1/2/4/8 bpp (`vera-tile<W>-<B>`), read from VRAM through the chip, with VERA's colour rules (index 0 transparent, palette offset on indices 1-15 only). It follows any of the 128 sprites (a number box in the pane: address, depth, size and palette offset from the attributes at $1FC00) and either tile layer (tile base, size and depth from the layer registers; 1 bpp tiles in the map's most common colours). VERA now implements `IVideoInspect`, which gains `layerCount()`/`layerCharsetInfo()` and VERA sprite/tile fields. CLI `gfx chip-sprite 0-127` and new `gfx chip-layer <0|1>`; MCP `render_graphics` takes the VERA formats and a `layer`. Tests: `x16_vera_video_inspect`, `x16_ui_panes`, `gfx_vera_palette_offset`; checked on an X16 at READY with `MOUSE 1`: the KERNAL character set in white on blue and the mouse pointer sprite (16x16, 8 bpp at $13000), in the CLI and GUI.
- **Graphics (ANTIC/GTIA)** for the Atari 8-bit (pane id `graphics-antic`): the graphics viewer for ANTIC and GTIA. Formats `antic2` (character modes 2/3, 2 colours), `antic4` (modes 4/5, 4 colours), `player` and `player-2line` (players and missiles at single- and double-line resolution). It follows players 0-3 and the missiles (PMBASE and DMACTL, as ANTIC fetches them, in COLPMn) and the character set (CHBASE, in the text mode the display list uses most, in GTIA's COLPF/COLBK colours and NTSC/PAL palette): ANTIC now implements `IVideoInspect`. The pane hides the VIC-IV-only palette offset and nibble swap, and starts at the ROM character set ($E000). CLI `gfx chip-sprite <n>` / `gfx chip-charset` (VIC sprites 0-7, Atari players 0-3 and 4 = missiles; `vic-sprite`/`vic-charset` remain) and MCP `render_graphics` take the Atari's formats. Formats now carry a chip family; a machine that lists none gets the VIC ones. Tests: `atari_antic_video_inspect`, `atari_ui_panes`; checked on an 800XL at BASIC's READY (the ROM character set in the screen's colours) in the CLI and GUI.
- **Panes per machine**: each machine lists the debugger panes its hardware supports (`"ui": {"panes": [...]}`, `MachineDescriptor::uiPanes`), and the GUI hides the rest: `graphics-vic`, `sidscope`, `cartridge`, `tape`, `drive`, `mega65`. Panes for any machine (console, CPU, memory, ...) always show. Control > Attach/Eject Cartridge are disabled on machines without a cartridge port. A machine that lists nothing gets every pane.
  - PET: Tape. VIC-20: Tape, Cartridge, Graphics (VIC). C64: Tape, Drive, Cartridge, SID Scope, Graphics (VIC). MEGA65: Drive, Cartridge, SID Scope, MEGA65 Status, Graphics (VIC). Atari: Drive, Cartridge. X16: Drive.
  - The PET and VIC-20 have no Drive pane: their disk access goes through the KERNAL intercept, with no drive device to show.
- **Graphics (VIC)**: the sprite/character viewer is now named for what it shows (pane id `graphics-vic`; a saved layout that had "Graphics" open shows it closed once). It offers only the formats of the machine's video chip (`"ui": {"graphicsFormats": [...]}`): VIC-20 characters, C64 VIC-II sprites and characters, every format on the MEGA65; none on the PET, Atari or X16, which do not have a VIC. CLI `gfx formats` lists the machine's formats and `gfx`/MCP `render_graphics` reject others (MCP defaults to the machine's first format). New VIC-I formats `char16` and `char16-mc` (8x16 double-height characters). Atari (player/missile, ANTIC modes) and VERA (sprites, tiles) viewers are to follow as their own panes.
- Tests: `machine_ui_capabilities`, `atari_ui_panes`, `x16_ui_panes`, `gfx_vic1_double_height_characters`.

### Commodore Fixes (PET)
- **PET tape** now saves and loads (2001 and 4032, checked from BASIC: `PRESS PLAY & RECORD ON TAPE #1`, `WRITING T`; then `FOUND T`, `LOADING`, RUN). Its lines were wrong: the sense line named a PIA pin (`pb6`) the 6520 does not offer, so the tape had none. Now as VICE wires cassette #1: sense on PIA1 PA4, motor on PIA1 CB2, write on VIA PB3 (was VIA CA2 and PB7). `tapeWiring` accepts declared signals (moved after the signals in the loader). Test `pet_tape_lines`.
- **PET 8032 keyboard**: the business keyboard used the graphics keyboard's matrix (a comment said they were the same), so most keys typed the wrong characters or moved the cursor. Its matrix now comes from the 8032 editor ROM's decode table ($E6D1), cross-checked against VICE's business keymap; characters without a key of their own (`"`, `!`, `(`, `*`, `=`, `+`, ...) are typed with SHIFT, as their legends are. Typing every symbol on an 8032 gives the right screen codes; `DLOAD`/`DSAVE`/`DIRECTORY`/`LOAD"$",8` on its drive 8 work. Test `pet_business_keyboard_positions`.
- **Disk images on the PET's drive 8** (#21): `disk mount 8 <image>` serves a D64, D71, D80, D81 or D82 image, read-only, through the IEEE-488 drive (`CbmImageDos`): LOAD with wildcards, the directory as Commodore's drives send it (name, ID, DOS type, types and flags, blocks free, BASIC lines linked at $0401), PRG/SEQ/USR files, the error channel; writes answer `26,WRITE PROTECT ON`. Checked from BASIC 4 on a 4032 with a D64 and a D80. The GUI's drive pane offers D71/D80/D82 too.
- **Disk image fixes** (shared with the C64's drive): the D80/D82 header (disk name, ID, DOS type) is at 39/0, not 38/0 (VICE `vdrive-bam.c`: "the first BAM block with the disk name is at 39/0"); blocks free follow CBM DOS for every format, leaving out the directory track (an empty 1541 disk: 664, was 683), with the D71's second side, the D81's two BAM sectors and the D80/D82 bitmap blocks chained from 38/0 (they read the first one only). New `getDosType()`. Tests: `d64_free_blocks_leave_out_the_directory_track`, `d80_open_dir_read` (free blocks, DOS type), the IEEE drive's image tests.
- **PET IEEE-488 bus and drive 8** (#21): the bus is wired to PIA1, PIA2 and the VIA as on the real board (after VICE), and drive 8 on it serves a host folder: `LOAD`, `SAVE`, `LOAD"$",8`, BASIC 4's `DLOAD`/`DSAVE`/`DIRECTORY`/`DS$`, sequential files, the error channel. The drive answers the three-wire handshake (DAV/NRFD/NDAC) and the commands under ATN, sends again a byte the PET cuts off before UNTALK (as Commodore's drives do), and stays silent for a missing file (BASIC 4: `?FILE NOT FOUND`; BASIC 2 retries until STOP, as its ROM does). The Drive Status pane is now on the PET; `disk mount 8 <folder>` picks the folder. Checked from BASIC on the 2001 and 4032. The 8032 could not be checked: its keyboard input is wrong, also on master (a separate problem).
  - The X16 HostFS's DOS is now `CbmHostDos` (`src/plugins/devices/ieee488/`), shared by both; the `$` listing's load address is a setting ($0801 on the X16, $0401 on the PET).
  - The 2001 needed the VIA's vertical retrace on PB5 (its screen editor waits for it); it had read the output latch by chance. New loader options: `frameAccumulator.retraceSignal`/`retraceCycles`, `portWiring`, `pinWiring`, and `"DEV.line"` in a device's `lines`; `IOHandler::namedPort`, port getters on the 6520 PIA.
  - New plugin `mmemu-plugin-ieee488.so` (device `ieee488_bus`). Tests: `test_ieee488_bus.cpp` (5).
- **PET NMI button**: the PET has no RESTORE key and nothing drives its NMI line, but its ROMs send an NMI to BASIC's warm start through `$0094`, and owners often added a pushbutton for it. mmemu models that button, not a RESTORE key: the GUI's **Trigger NMI** button below **Capture Keyboard** on the screen pane (PET only), or the key name `nmi` in the CLI and MCP. It breaks a running `10 GOTO 10` into `READY.` on the 2001 and 4032 (checked in the CLI). New `kbdWiring.nmiKey` and `MachineDescriptor::nmiButtonKey`. Test `pet_nmi_button`.
- **PET typing**: the CLI's `type` and MCP's `type_string` sent capitals as SHIFT+letter, which is a graphics character on a PET, so typed BASIC came out as graphics. Capitals are now plain key presses. Unknown key names (such as `restore`) are now rejected instead of silently accepted.

### Commodore Fixes (NMI and RESTORE)
- **VIC-20 NMI**: VIA1 now drives the CPU's NMI and VIA2 alone drives IRQ, as on the real machine; both were on IRQ.
- **C64 NMI**: CIA2's interrupt output now reaches NMI. The machine file named it as an NMI source, but the loader attached it to a device NMI output the CIA does not have, so it was not connected.
- **RESTORE key** on the VIC-20 (pulls VIA1 CA1 low) and C64 (straight to NMI): key name `restore`, GUI Page Up as in VICE. RUN/STOP+RESTORE warm-starts BASIC on both, checked in the CLI.
- Loader: new `sharedNmi` signal type (wired-OR NMI, chip interrupt outputs as sources, extra named `lines`) and `kbdWiring.restore`. A plain `nmi` line now fires only on a rising edge.
- Tests: `vic20_restore_is_nmi`, `c64_restore_is_nmi`.

### Commodore Fixes (tape)
- **VIC-20 tape** now saves and loads: `SAVE` to the datasette, then `LOAD` and `RUN` from the recorded `.tap`, checked from BASIC. The lines were wired to the wrong pins; now they match VICE and the KERNAL: sense on VIA1 PA6 (was PB6, which the KERNAL never reads), motor on VIA1 CA2 (was VIA2 PB3), write on VIA2 PB3 (was PB7), read pulses on VIA2 CA1 (was VIA1 CA1, the RESTORE line). New VIA input-pin lines `pa0`-`pa7` let a peripheral hold a port A pin.
- **Datasette recording** stored every edge of the write line as a pulse, so a recorded `.tap` held half-cycles at twice the count. It now stores one pulse per cycle, from one 0→1 transition to the next, as VICE does. A C64 `SAVE` to tape loads back (checked from BASIC; not tried with the old recorder); the PET's recording uses the same code but was not tried.
- Tests: `vic20_tape_lines`, `via6522_port_a_input_pin_lines`; `datasette_recording` now checks the stored pulse lengths.

### Input
- **VIC-20 joystick** now on the right lines, as in VICE: up, down, left and fire on VIA1 PA2-PA5 (`$911F`), right on VIA2 PB7 (`$9120`), shared with keyboard column 7. Before, it was on VIA1 port B (the user port). The machine JSON's `joystick` can put each switch on a chosen bit of several ports (`lines`, new `JoystickPins`). The keyboard's column port now drives only the bits set as outputs, so a program can read PB7. Test `vic20_joystick_on_via1_and_via2`; checked from BASIC.
- **VIC-20 keyboard matrix** fixed against VICE: `:` is its own key (was typed as SHIFT+`;`, which gives `]`, so typed BASIC lines with `:` failed), `-` and `↑` (arrow) moved to their real positions, and cursor up/left are SHIFT+down/right (the matrix has no keys for them; left did nothing before). `]` can be typed.
- **C64 joysticks**: both control ports, on CIA1 as on the real machine: control port 1 on Port B with the keyboard rows, control port 2 on Port A with the columns. CIA1 reads its output bits back from the pins (new `IOHandler::setOutputsReadPins`), so `PEEK(56320)` sees port 2 while the KERNAL drives the columns. `onJoystick` port 1 is control port 2. The machine JSON's `joystick` now takes an array of ports and `joystickPrimary`; a port shared with the keyboard reads both. The MEGA65 gets the same wiring: before, its control port 1 was not connected and `onJoystick` port 1 drove Port B.
- **Host USB controllers in the GUI**: game controllers and joysticks, read through SDL2's game-controller API (optional at build time), drive the machine's joystick ports. The C64, MEGA65 and Atari get a 5-bit joystick: D-pad, hat or left stick, and any face button for fire. The Commander X16 gets SNES pads with the buttons matched by position. The first controller goes to the port most software reads (`MachineDescriptor::primaryJoystickPort`: the C64's control port 2); **Control > Swap Joystick Ports** swaps the first two. Controllers can be plugged in and out while it runs, and the status bar names each one. New `MachineDescriptor::joystickPorts` and `cbmJoystickBits()`. The CLI `joy` and MCP `set_joystick` check the port number.
- Tests: `c64_joysticks_on_cia1`, `cia_outputs_read_pins_when_enabled`, `host_pad_buttons_map_by_position`, `host_pad_ports_follow_the_machine`, MCP (C64 `$DC00`). The SDL code was checked with SDL virtual controllers: plugging and unplugging, D-pad, stick, buttons, and a plain joystick's hat. No physical controller was available.

### Commander X16
- **Host folder as unit 8 (HostFS)** (#16): the `x16.hostfs` intercept, ported from x16-emulator's `ieee.c`, serves the current directory as IEEE unit 8 by intercepting the KERNAL's IEEE API (LISTEN/SECOND/CIOUT/MCIOUT/UNLSN/TALK/TKSA/ACPTR/MACPTR/UNTLK): `LOAD`, `SAVE`, `DOS"$"` (with CMDR-DOS's filters), `OPEN`/`GET#`/`PRINT#`, the error channel and the DOS commands `CD`, `MD`, `RD`, `S:`, `R:`, `P`, `T`, `U0>`. While an SD card is mounted it steps aside on unit 8, as x16-emulator does. CLI: `intercept set x16.hostfs path|startin|unit <value>`. See `doc/README-X16.md`. Tests: `test_x16_hostfs.cpp`.
- **SNES pads and mouse** (#16), after x16-emulator's `joystick.c` and the SMC mouse in `i2c.c`/`smc.c`: four pads on VIA1 port A (BASIC `JOY(1..4)`; -1 until a pad is used), and the PS/2 mouse through the SMC with the wheel (`MOUSE 1`, `MX`/`MY`/`MB`). New generic `MachineDescriptor::onGamepad` (SNES-style buttons) and `onMouse` hooks; CLI `joy <port> [buttons]` and `mouse <dx> <dy> [buttons] [wheel]` (`joy` also drives CBM joysticks through `onJoystick`), MCP `set_joystick` and `mouse_input`; the GUI screen pane sends the host mouse to machines that have one. Tests: `x16_snes_pads_shift_buttons_and_presence`, `x16_smc_mouse_packets`, `x16_basic_reads_gamepad_and_mouse`, MCP. The GUI mouse could not be driven from the test desktop (XWayland), so it is checked by build only.
- **SD card** (#16): VERA's SPI port now has an SD card in SPI mode (`SdCardSpi`, after x16-emulator's `sdcard.c`) backed by a raw image file, read and written block by block. Mount it as unit 8 (CLI `disk mount 8`, the GUI drive pane, which now lists `*.img`, or MCP), or put it at `roms/x16/sdcard.img`. On a FAT32 image, BASIC's `SAVE`, `DOS"$"`, `LOAD` and `RUN` work. See `doc/README-X16.md`. Tests: `test_sdcard_spi.cpp`.
- **YM2151 FM sound** (#16): `src/plugins/devices/ym2151/` wraps ymfm's OPM (Aaron Giles, BSD-3-Clause, vendored in `src/third_party/ymfm`) with x16-emulator's glue: busy flag and timers in chip clocks, writes while busy dropped, timer IRQs on the X16's shared line. VERA mixes it into its output as `(psg + pcm) * 2 + ym` (new `Vera::setExternalAudio`), resampled from 55,930 to 48,828 Hz. BASIC's FM commands play: `FMNOTE 0,$4A` gives 440 Hz. Tests: `test_ym2151.cpp`.
- **Commander X16 machine (`x16`, #16)**: boots the R49 ROM (`roms/x16/rom.bin`, not in the repository) to BASIC's READY in the CLI, GUI and MCP, and runs BASIC including VERA graphics (`SCREEN`, `LINE`) and PSG sound (`PSGNOTE`/`PSGFREQ`). See `doc/README-X16.md`.
  - **Machine** (`src/plugins/machines/x16/`, after x16-emulator's `memory.c`/`via.c`): WDC 65C02 at 8 MHz; RAM, bank registers `$00`/`$01`, 512 KB banked RAM, 32 ROM banks; VIA1 (I2C, NES lines, IEC port) and VIA2; slow-I/O wait states.
  - **I2C** (`x16_i2c.cpp`, after `i2c.c`/`smc.c`/`rtc.c`): the SMC (keyboard buffer, version, reset/NMI requests) and the MCP7940N RTC (BCD time from the host clock, NVRAM), bit-banged through VIA1 port A as open-collector lines.
  - **Keyboard**: key names to IBM key numbers through the SMC; CLI `type`/MCP `type_string`; the GUI sends keys positionally (and now names the backslash and backtick keys).
  - **Interrupt vectors**: the 6502 core gains a vector-pull hook (the 65C02's VPB pin); the X16 uses it to select ROM bank 0 for the vector fetch while `$01` keeps the interrupted bank — without it the first IRQ in another bank jumped to `$AAAA`.
  - **VERA** (`src/plugins/devices/vera/`, `mmemu-plugin-vera.so`, ported from x16-emulator's `video.c`, `vera_psg.c`, `vera_pcm.c`, `vera_spi.c`): registers, VRAM and address ports, layers (text/tile/bitmap), 128 sprites with collisions, composer and display window, VGA/NTSC/240p timing, interrupts, the full FX set, 16-voice PSG and PCM audio at 48,828 Hz, SPI. Side-effect-free peeks.
  - Tests: `test_vera.cpp` (16), `test_x16_i2c.cpp` (3), `test_x16_machine.cpp` (boot to READY and a typed line; the vector-pull hook).

### Project
- **Licence: AGPL-3.0-or-later**: mmemu is now licensed under the GNU Affero General Public License v3 or later (`LICENSE`, the unmodified text from gnu.org). Anyone distributing it, or running a modified version as a network service, must share the source under the same terms. `CONTRIBUTING.md` asks for changes to be offered back via https://github.com/CTalkobt/mmemu and describes the workflow, including DCO sign-off. `NOTICE` credits reSID, reSIDfp and atari800 (all GPL-2.0-or-later, so compatible) and states that the ROM images in `roms/` are not covered. The README gains a License section.

### Debugger
- **65C02 assembler**: the CLI `asm`/`.` commands (and MCP assembly) on 65C02 machines such as the Commander X16 accept the CMOS instruction set: `STZ`, `TRB`/`TSB`, `BRA`, `PHX`/`PHY`/`PLX`/`PLY`, `INC A`/`DEC A` (`INA`/`DEA`), the new `BIT` modes, `(zp)`, `JMP (abs,X)`, `WAI`, `STP`, `RMBn`/`SMBn` and `BBRn`/`BBSn zp,target`; the NMOS illegal opcodes are rejected. Test `assembler65c02_round_trips_every_opcode` disassembles every documented 65C02 opcode and assembles it back.
- **WDC 65C02 core** (#16, for the Commander X16): `MOS65C02` (core and disassembler ISA `65C02`) adds the CMOS instruction set to the 6502 plugin: STZ, TRB/TSB, BRA, PHX/PHY/PLX/PLY, INC/DEC A, BIT #/zp,X/abs,X, `(zp)`, `JMP (abs,X)`, BBR/BBS/RMB/SMB, WAI, STP; CMOS decimal flags and timing; BRK/IRQ/NMI clear D; no `JMP ($xxFF)` bug. The opcode table is generated from x16-emulator's tables with timings corrected to recorded WDC behaviour. See `doc/README-6502.md` §7.
- **Whole-instruction-set tests for the 6502 and 65C02**: `test_cpu_opcode_suites.cpp` runs Klaus Dormann's functional tests and SingleStepTests/65x02 (per opcode: registers, RAM, cycles) when they are present under `../refs`. All pass: Dormann 6502 (NMOS and 65C02) and 65C02 extended; SingleStepTests 239,736 NMOS tests over 244 opcodes (all but the 12 JAM opcodes) and 263,804 65C02 tests over 256 opcodes. They found these NMOS core bugs, now fixed:
  - Indexed stores and read-modify-write instructions took an extra cycle on a page crossing (only reads do).
  - ANC, ALR, ARR, XAA, SBX (immediate illegal opcodes) read their operand from the byte after it and skipped a byte; LAS fetched its address twice.
  - ARR set its flags like ROR (C is bit 6, V is bit 6 ^ bit 5; decimal mode adjusts the nibbles); LXA used LAX; XAA and LXA lacked the $EE "magic" constant.
  - SHA/AHX, TAS, SHY and SHX were stubs that stored nothing.
- **`tests/45gs02/validate.py`** finds `ca45` and `xemu-xmega65` on the `PATH`.
- **KERNAL SAVE to the host folder**: the `cbm.kernal.fileio` intercept (C64, VIC-20) answered SAVE with "device not present". It now writes the file to its host folder (the start address, then the data from the start pointer in A's zero-page address up to X/Y). An existing file is replaced only with an `@:`/`@0:` prefix, as on CBM DOS; without it the file is kept, SAVE returns normally and a warning is logged. A missing name gives KERNAL error 8. Test `kernal_hle_save_writes_host_file`; checked with `SAVE"MYPROG",9` and `LOAD` back on the C64.
- **Peeks have no side effects**: memory dumps, debugger views, `iomap` and the "before" value of a write (taken whenever a debugger is attached) read I/O registers the way the CPU does, so merely looking could clear a CIA's interrupt flags, release a PIA/VIA handshake, clear VIC-II collisions, advance the MEGA65 floppy's data pointer or random number generator, or run a pending floppy command. Devices now have `IOHandler::ioPeek` (default `ioRead`, for devices whose reads change nothing), `IORegistry::dispatchPeek` serves `peek8` on `FlatMemoryBus`, `MapMmu` and the Atari bus, and these devices peek without side effects:
  - **CIA 6526**: ICR not cleared and IRQ not released; TODHR does not latch, TODTEN does not release.
  - **VIA 6522**: ORA/ORB do not clear CA1/CA2/CB1/CB2 or run the handshake; T1CL/T2CL do not clear their flags.
  - **PIA 6520**: ORA/ORB do not clear IRQ flags or drive CA2 (the Atari bus no longer returns `$FF` for the PIA on a peek).
  - **VIC-II, VIC-III, VIC-IV**: `$D01E`/`$D01F` collisions not cleared. **VIC 6560**: no bus-pointer update.
  - **F011**: the registers as they are: no media refresh, no pending command run, `$D087` does not advance, `$D083` keeps WTREQ.
  - **MEGA65 maths unit**: `$D7EF` does not advance the random number generator.
  - **Port devices**: `IPortDevice::peekPort` (the IEC bus device does not advance its handshake).
  - `FlatMemoryBus::write8` took a write's "before" value with a side-effecting I/O read, like `MapMmu` did before #183. CLI `iomap`, the CLI and GUI KEY-register displays and the MEGA65's `$D030` queries now peek too.
  - Tests: `src/libdevices/test/test_io_peek.cpp` (the CIA, VIC-II, F011 and bus tests fail without the change).
- **Intercepts installed by the machine's configuration**: host-side handlers for firmware routines and calls are now listed in the machine JSON, on the machine or on one of its ROMs, instead of being wired in code or attached to every machine (`src/libcore/main/intercept.h`, `machines/README-machines.md`):
  - **PC intercepts** run instead of the instruction at their addresses; the CPU core (6502 and 45GS02) tests a per-machine table before each opcode, so they no longer depend on a debugger observer. On a ROM entry (`cpuAddr` or `overlayAddr`) they fire only while that ROM is visible.
  - **Call intercepts** see a machine's firmware calls first: the MEGA65 asks them about every user-mode hypervisor trap, with `trap` and `functions` selecting which calls they serve.
  - **CBM KERNAL host-folder LOAD** (`cbm.kernal.fileio`, cbm-hle plugin) was a global observer that trapped `$FFD5`/`$FFD8` on every machine (Atari included) with C64 zero-page addresses. It is now installed on the KERNAL ROM in `machines/c64.json` and `machines/vic20.json`, with its addresses and KERNAL variables as settings; the PET, MEGA65 and Atari do not install it. It now leaves units below 8 to the KERNAL, and `LOAD"X",n` (secondary address 0) no longer loads the file's 2-byte header into memory.
  - **MEGA65 HYPPO DOS calls** (`mega65.hdos`) are listed in `machines/mega65.json` (replacing `hdos.paths`), defaulting to the calls the machine has always served from a host folder. setname (`$2E`) and the transfer area (`$3A`) are now actually read (from user memory at page Y, as HYPPO does) and still passed to HYPPO: before, the code to capture them was never called, so findfile, openfile and loadfile never saw a name and readdir wrote no entries. A `filename` setting overrides the name; results now also set Y (it was never written).
  - CLI: `intercept [list]`, `intercept on|off <name>`, `intercept set <name> <setting> <value>`, `intercept types`; `hle on|off|path` now controls the current machine's `cbm.kernal.fileio`.
  - Tests: `src/libcore/test/test_intercept.cpp` (ROM visibility, banked-out ROM, disabled and unhandled intercepts, warnings, call selection, HYPPO setname and the filename override), `kernal_hle_load_sa0_skips_header`, `kernal_hle_leaves_tape_to_kernal`, `mega65_installs_hdos_intercept_from_config`.
- **45GS02 dot-assembly through libca45**: the ca45 backend's `assembleLine()` returned -1, so the CLI's `.<instruction>` (and MCP's line assembly) fell back to the 6502 assembler and refused the 45GS02's own instructions (`MAP`, `EOM`, `NEG`, `ASR`, `LDZ`, quad and `[zp],z` modes, 16-bit branches). It now assembles with libca45 (m65compiler #117), linked into the 45GS02 and MEGA65 plugins when the build finds it (`CA45_DIR`, default `../m65compiler` or `/usr/local`); `lastError()` gives the reason for a -1. Test `src/plugins/45gs02/test/test_ca45_assemble_line.cpp`.
- **`.sid` tunes played far too fast**: CLI `sid load` and MCP `load_sid` called the tune's play routine back to back as fast as the CPU ran, instead of once a frame. That alone made tunes sound horrible: Rob Hubbard's *Commando* matched a sidplayfp rendering with an onset correlation of 0.09. Their loop also sat in zero page (`$0002`), which tunes use. Both now share one loader (`src/libdebug/main/sid_file.cpp`) with a player driver in free RAM:
  - `init` gets the subtune, then `play` is called once per CIA 1 timer period: 50 Hz for VBI songs, or the tune's rate for CIA-speed songs.
  - `$01` is banked for each as sidplayfp does, and tunes without a play routine run from their own IRQ.
  - Against sidplayfp, *Commando* and *Monty on the Run* now match in tempo (1.000 / 0.995) and pitch, with a spectrum correlation of 0.98.
  - Tests cover 50 Hz, CIA-rate timing, banking and driver placement.
- **`recordaudio` / MCP `record_audio` dropped samples**: Both counted CPU instructions as clock cycles, so each batch ran 3-5 times longer than intended. A batch could produce more samples than the sound chip's 8192-sample buffer holds, and the excess was dropped, giving audible skips in longer recordings. They now count the cycles each step reports.
- **Issue #169 - Real-time speed**: The GUI and CLI `run` ran machines as fast as the host allowed (a C64 at about 15× real time). They now keep the machine's device clock in step with the wall clock: the C64 (985,248 Hz), VIC-20 (1,022,727 Hz) and PET (1 MHz) run at real time, so the jiffy clock advances 60 times a second. Control > Speed is now shown for these machines too, with **Real time** and **Max (unbounded)**; CLI `speed [real|max]` and MCP `set_speed` select the same setting. On the MEGA65 the 1/2/3.5/40.5 MHz settings are paced against the ~1 MHz phi clock the devices run on, so the CPU clock changes throughput while devices stay real time; Max (warp) is unpaced. The GUI and CLI start every machine at real time (the MEGA65 at its current clock with warp off); `--speed real|max|<MHz>` on either overrides this. Machines created elsewhere (unit tests, the MCP server) start at Max, so tests run flat out unless they need real time; `tests/mega65_boot_ready_test.py` passes `--speed max`. When emulation falls more than 100 ms behind (slow host, a pause) it resyncs instead of catching up in a burst. The MCP server stays unpaced. Machine JSON gains an optional top-level `clockHz`.
- **Issue #30 - SID oscilloscope**: New SID Scope pane (View > SID Scope) draws a trace for each SID voice and for the mixed output, triggered on a rising zero crossing so steady tones stand still, or free-running, over 2-50 ms. Each voice shows its waveform, frequency and nearest note (with cents), pulse width, ADSR settings, envelope phase and level, filter routing and voice-3-off, with an ADSR diagram highlighting the current phase. On the MEGA65 either SID of the pair can be shown. A new MCP `sid_scope` tool and CLI `sid scope` command draw the same scope to a PNG, or to an animated GIF captured while the machine runs (frames spaced in emulated time); CLI `sid voices` and the MCP tool's reply list each voice's state as text. The SID records the scope history and exposes voice state through a new `IAudioInspect` interface. See `doc/README-SID.md` section 2.6.
- **Graphics pane GTK warnings (#161)**: the Count, Columns, Stride and Palette + spin controls had fixed 70-80 px widths, narrower than a GTK3 spin button's entry plus its - and + buttons, so showing the pane logged `gtk_box_gadget_distribute: assertion 'size >= 0' failed in GtkSpinButton` (8 times per start). Each control is now sized to fit its widest value.
- **Issue #28 - Sprite and character viewer**: New Graphics pane (View > Graphics) shows sprites and characters at any address in hires, multicolour and the VIC-IV 64-wide, 16-colour, FCM and NCM formats, or follows a VIC sprite (pointer, mode and colours) or the current character set. It can read memory as the CPU or as the video chip sees it (C64 character ROM), updates live while running, shows the address of the item under the mouse, opens items in the memory pane and exports PNG. The same decoder backs a new CLI `gfx` command (ASCII art or PNG) and MCP `render_graphics` tool. VIC-II and VIC-IV implement a new `IVideoInspect` interface for this. See `doc/README-graphics-viewer.md`.
- **Issue #117 - Bit-field syntax in expressions**: The expression evaluator (breakpoint conditions, `runto`, CLI/GUI/MCP expressions) accepts `value:N` for bit N and `value:L-H` for bits L..H shifted down (bit 0 = LSB), e.g. `break when SR:0 = 1`, `runto A:6-7 = 2`, `*$D011:7`. Single `=` now compares like `==`, and `SR`/`STATUS` name the CPU's status register. `*addr:16` is still the 16-bit word read (`*addr:16:8-15` takes its high byte).

### Commodore Fixes
- **Removed old SID design documents**: The seven filter, nonlinearity and saturation design notes in `doc/` (`SID6581_FILTER_*`, `SID6581_NONLINEAR_FILTER_DESIGN.md`, `SID_FILTER_SATURATION_DESIGN.md`, `SID_SATURATION_*`) described the replaced sample-rate filter. Also removed: `test_sid_saturation.cpp`, which tested that design and was never built. `doc/README-SID.md` describes the current SID.
- **Issues #120-#123 - SID clocked every cycle, measured filter, digis and filter saturation**: The SID ran its voices and filter once per 44.1 kHz output sample, which stepped envelopes and shifted the noise LFSR at most once per CPU instruction, so results changed with how time was ticked. It now follows reSID 0.16, clocking oscillators, the LFSR, hard sync, envelopes and the filter every cycle and averaging down to the output rate (#123).
  - **Envelopes** use reSID's rate and exponential counters, including the ADSR delay bug.
  - **Filter (#120):** cutoff follows curves measured on real chips instead of an invented log curve. On the 6581 that is 220 Hz to 18 kHz with the drop from 6 kHz to 4.6 kHz at $400; on the 8580, 0 to 12.5 kHz. Resonance is Q = 0.707 + res/15.
  - **Saturation (#121):** the filter integrators are limited softly at the op-amps' measured output swing, so loud resonant sounds compress on the 6581 but not the 8580.
  - **Digis (#122):** the 6581's measured DC offsets are scaled by the master volume, so `$D418`-only sample playback (drums, speech, digis) is audible. The C64's output RC filters remove the DC.
  - **Cleanup:** the old `FilterCurve` approximation and its tests are removed.
  - **Speed:** a C64 at max speed runs about 11× real time.
  - **Full scale:** three voices at full volume, as in reSID. Two voices, the first choice, clipped loud tunes such as Rob Hubbard's Commando.
- **Issue #119 - SID combined waveforms**: Selecting several waveforms on a voice (saw+pulse, triangle+pulse, saw+triangle, all three, pulse+noise) now uses the combined-waveform pulldown model fitted to real chips (reSIDfp's model and parameters: 6581 R3 0486S and 8580 R5 1088), chosen by the SID's chip variant. The previous tables were placeholders that ignored the pulse width and matched real-chip OSC3 samplings no better than chance (12167-15633 of 32768 bits wrong); the model gets 124-1030 wrong, as reSIDfp reports. Pulse now gates combinations (silent while low). Also: noise combined with another waveform writes 0 output bits back into the LFSR, so it locks up as on hardware; noise output bit 5 now comes from LFSR bit 2 (was bit 3); and the triangle's LSB is grounded.
- **Issue #159 - VIC-II sprites**: Multicolour sprites ($D01C) now render as pixel pairs in $D025 / sprite colour / $D026 instead of as hires bits. Sprites are placed at the hardware coordinates (X=24, Y=50 is the top-left of the text area; they were drawn 8 pixels too far left and 14 lines too low), are covered by the border, and honour sprite-background priority ($D01B: behind-background sprites are hidden by foreground pixels, including multicolour 10/11 pairs). Lower-numbered sprites stay on top. Also applies to the VIC-III and the VIC-IV in its VIC-II personality.

### Atari Fixes
- **Atari: cartridges, saved disk writes, GTIA modes, PAL (#10 follow-up)**:
  - **Cartridges:** `.car` and raw 8/16 KB `.rom` images go in through CLI `cart`/`eject`, the GUI's Attach Cartridge and MCP `attach_cartridge`, and cold-start. The supported types are standard, OSS, Williams, Express, Diamond, SpartaDOS X, DB, XEGS and switchable XEGS, MegaCart, Atarimax, Phoenix and Blizzard, with bank switching on `$D5xx` after atari800's `cartridge.c`. On the XL/XE, TRIG3 reads the cartridge's RD5 line.
  - **Disk writes are saved:** sectors written to a disk mounted from a `.atr` go back into the file at once. An unwritable file makes the drive write-protected (status bit 3; writes fail). CLI `disk mount <1-4> <file>`/`disk eject` and the GUI's Drive Status pane now handle D1:-D4: (the pane offers `.atr` files for them).
  - **GTIA modes 9, 10 and 11**, **VDELAY** and **multicolour players** (PRIOR bit 5).
  - **PAL machines:** `atari800-pal`, `atari800xl-pal`, `atari65xe-pal`: 312 lines, 1,773,447 Hz, the GTIA PAL flag and a PAL palette from atari800's PAL colour model.
  - **Tests:** GTIA modes, VDELAY, multicolour and the PAL palette; cartridge banking for each scheme; a cartridge booting on the 800XL and 800 and BASIC returning after eject; SIO writes saved to the file and refused on a read-only one; the PAL 800XL booting with `PALNTS` = 1 and 312 lines. Also checked through the CLI: the OS's `DSKINV` writing a sector to D2:, and GRAPHICS 9 and 11 from BASIC.
- **Issue #10 - Atari 8-bit machines (`atari800`, `atari800xl`, `atari65xe`)**: The Atari now boots to Atari BASIC's READY prompt and runs BASIC programs with graphics and sound, in the GUI and CLI.
  - **Machine plugin** (`mmemu-plugin-atari.so`):
    - A dedicated bus for the 800 map (48 KB, OS-B, BASIC cartridge) and the XL/XE map (64 KB, PORTB banking of the OS, BASIC and the self-test ROM).
    - I/O mirrors, POKEY+PIA IRQ and ANTIC NMI.
    - A scheduler that holds the CPU for WSYNC and the cycles ANTIC's DMA takes.
    - Real-time pacing on ANTIC's cycle count.
  - **Keyboard and joysticks:**
    - GUI and CLI key names, with shift and control.
    - Console keys and RESET, HELP and BREAK on F1-F8.
    - Typed text paced on the OS's key buffer.
    - Joysticks on PIA port A and the GTIA triggers.
  - **ANTIC** was a stub that never drew anything. It is rewritten:
    - Display lists, character modes 2-7 and map modes 8-F, LMS, DLI and JVB.
    - Narrow, normal and wide playfields, HSCROL/VSCROL, CHACTL/CHBASE.
    - Player/missile DMA, DLI and VBI NMIs, NMIST/NMIRES, VCOUNT.
    - WSYNC and the DMA cycles it takes from the CPU.
    - A 384 × 240 frame.
  - **GTIA** composes each line: players and missiles with sizes and the PRIOR priorities (including the fifth player), all 16 collision registers, high-resolution luminance, console keys and triggers.
  - **POKEY** is rewritten:
    - 64 kHz, 15 kHz and 1.79 MHz clocking, 16-bit joins, the polynomial distortions, high-pass filters and volume-only mode.
    - Timer IRQs, keyboard (KBCODE, SKSTAT, BREAK) and RANDOM.
    - A serial transmitter at channel 4's rate, so the OS's disk boot times out properly instead of being faked.
    - Both POKEY and ANTIC previously treated `tick()`'s cycle count as absolute.
  - **Loading software:** `.atr` disk images and `.xex`/`.com` executables load through `load`, the GUI's Load Image and MCP, and boot with BASIC off:
    - **SIO bus:** drives D1:-D4: answer the OS's command frames (read, write, status) through POKEY at 19,200 baud.
    - **Executables:** an executable boots from a generated disk whose 6502 boot loader handles INITAD and RUNAD.
    - **POKEY fix:** a SEROUT byte now enters the shift register at the next bit, not at once. Raising "output needed" immediately ran the OS's interrupt handler before its send routine stored the checksum, so every command frame had a wrong checksum.
  - **6502 decimal mode:** ADC and SBC ignored the D flag on every 6502 machine. They now follow NMOS decimal mode, per Bruce Clark's documentation. Atari BASIC, which does its arithmetic in BCD, printed `2+3*4` as `>`.
  - **Recordings:** `recordaudio` and MCP `record_audio` use the machine's clock (they assumed a PAL C64's).
  - **Removed:** the old Atari boot tests, which used a machine factory that didn't exist.
  - **Tests:** ANTIC, POKEY, decimal mode and machine tests: boot to READY on the 800 and 800XL, BASIC arithmetic, PORTB banking, keys, and the rendered screen. See `doc/README-ATARI.md`.
- **GTIA palette (#160)**: the 256-entry palette is now 0xAARRGGBB, the format every video chip's frames use since #156, instead of 0xRRGGBBAA. The old table was also wrong in layout: each hue had only 10 luminance steps followed by 6 black entries, and the grey row had a red at $05. It is replaced with the full 16-hue × 16-luminance NTSC palette from atari800's palette model ("standard" preset). `GTIA::getRGBA` is renamed `getARGB`.

### MEGA65 Fixes
- **F011 floppy controller: DIR, LOAD and SAVE on the MEGA65's drives**: `$D080-$D09F` was a stub, so drive 8 always reported "drive not ready" and BASIC 65 could only reach disks through the KERNAL host-file trap. The new `F011Fdc` (`src/plugins/devices/sdcard/main/f011.cpp`, after xemu's `f011_core.c`) serves drives 0 and 1 (units 8 and 9) to the ROM's DOS, in MEGA65 and C64 mode:
  - **The SD card's `MEGA65.D81`**, which HYPPO mounts through `$D68B` and the start sector in `$D68C` / `$D690`. `$D68B` bit 2 / 5 means "writable" (`sdcardio.vhdl` returns "not write protected"; the book's D0WP label and xemu have it the other way). `$D68A-$D693` can only be written in the hypervisor.
  - **A host `.d81`** attached with CLI `disk mount 8 <file>`, the GUI's Drive Status pane (now offering `*.d81`) or MCP; writes go to the file, and swapping disks sets the disk-changed signal.
  - **Disk buffers**: the SD card device owns the 4 KB at `$FFD6000` (F011 buffer at `+$C00`, SD buffer at `+$E00`); `$DE00` shows the buffer `$D689` bit 7 selects outside the hypervisor; `$D688`/`$D689` read the F011 buffer pointer.
  - Because the F011 answers for units 8 and 9, the CBM KERNAL HLE now leaves `LOAD`/`SAVE` on them to the ROM's DOS.
- **`$00`/`$01` were the CPU port even with block 0 MAP'd**: the C65 DOS maps its RAM at `$0000` and keeps its BAM pointer at `$00`/`$01`, so it read the CPU port's DDR and data instead, and `DIR` reported 4410 blocks free on an empty disk. The port is now there only while block 0 is not MAP'd (gs4510.vhdl: `reg_map_low(0)='0'`).
- **Writes to I/O registers also read them when a debugger was attached**: `MapMmu::write8` took the old value for the observer through the I/O hooks, so with the CLI or GUI debugger attached every `STA $D087` also read `$D087` and advanced the F011 buffer pointer twice, interleaving the bytes of every sector the DOS wrote. The old value now comes from memory only.
- Tests: `src/plugins/devices/sdcard/test/test_f011.cpp` (F011 reads, writes with SWAP, record not found, disk change, the SD card's D81 and its write-enable bit, hypervisor-only mount registers, `$DE00` BUFSEL, the I/O write and `$00`/`$01` fixes) and `mega65_dload_from_host_d81` (boots through HYPPO, mounts a generated D81 and runs a program `DLOAD`ed from it; skipped without HICKUP.M65 and an SD card image).
- **Cursor keys typed `,` and `;` in the GUI, and LEFT/UP went the wrong way in C64 mode**:
  - The GUI's positional (MEGA65) key mapping passed every key code through `strchr()`, which truncates to `char`: `WXK_RIGHT` (316) matched `'<'` and was sent as `,`, `WXK_LEFT` (314) matched `':'` and was sent as `;`. Only ASCII codes are remapped now (`src/gui/main/main.cpp`).
  - The keyboard matrix put LEFT and UP on the unshifted CRSR keys, so in C64 mode (GO64) they moved right and down. LEFT and UP are now Shift + CRSR RIGHT / CRSR DOWN, as on the C64. BASIC 65 takes cursor keys from the `$D610` queue and was not affected. Test `mega65_keyboard_cursor_left_up_are_shifted`.
- **HYPPO now loads MEGA65.ROM itself; boot no longer depends on the host's ROM file or on KERNAL traps**: booting reached `READY.` only on a computer with the right files in the right places, because:
  - **`[zp],Z` read its pointer from physical memory**: `NOP` + `STA/LDA [zp],Z` fetched the 32-bit pointer from physical `$0xxxx` instead of the base page as the CPU sees it. In the hypervisor (base page `$BF`, hypervisor RAM) HYPPO's pointer to `$0020000` was never read, so `MEGA65.ROM` and `CHARROM.M65` from the SD card went to the wrong addresses while HYPPO reported them loaded. The machine ran only because the emulator preloaded the host's ROM file at `$20000`. The pointer is now read through the CPU's view (`cpu45gs02.cpp`).
  - **ROM banking read the host file**: `$01`/`$D030` banking and the character ROM served bytes from the buffer loaded from the host's ROM file, not the RAM at `$20000-$3FFFF` HYPPO fills. They now read that RAM, and the VIC-IV draws characters from the character WOM at `$FF7E000` (filled by HYPPO). The host ROM file is optional; without it, `romLoad` no longer prints an error and a reset no longer wipes `$20000` with `$FF`.
  - **The F011 stub claimed a disk was inserted** but never found a sector, so BASIC 65's autoboot hung in the ROM's DOS. Booting only worked because the CBM KERNAL HLE trapped the `LOAD` vector (`$FFD5`) first. The stub now reports no disk, so autoboot and `DIR` end with `74,DRIVE NOT READY`, and the boot reaches `READY.` with `hle off`.
  - Tests: `gs02_flat_store_pointer_from_hypervisor_base_page`, `mega65_rom_banking_reads_physical_rom_ram`, and `mega65_hyppo_boot_to_ready` (boots through HYPPO with no host traps; skipped without `HICKUP.M65` and an SD card image). `doc/README-MEGA65.md` 4.4 lists what a boot needs.
- **Issue #172 - Startup warnings shown in the GUI and MCP**: The MEGA65's "No SD card image found" warning (HYPPO then stops at its ROM selection menu) and "HYPPO not found" warning only went to the log, so GUI users saw the boot stop with no explanation. Machines now record such problems in `MachineDescriptor::warnings`. The GUI shows them in a dismissable warning bar above the machine screen, wrapped to the window width, and MCP `create_machine` appends them to its reply. The CLI already printed them.
- **VIC-IV sprites (MEGA65 mode)**: The VIC-IV sprite renderer gets the same fixes as the VIC-II (#159): multicolour sprites with 8-bit `$D025`/`$D026`/sprite colours, clipping to the display window, and `$D01B` sprite-background priority against text, NCM and FCM foreground pixels. Sprite X is placed as in viciv.vhdl: counting starts one physical pixel after `$D067` (sprite first X, reset value 31), or at the raster's left edge with SPR640 (`$D05F` gives X bit 9), relative to TEXTXPOS (`$D04C`), so X=24 (X=80 with SPR640) meets the text area by default. Y expansion no longer copies the background line below a sprite row, and sprite-over-sprite overlap follows sprite number.
- **VIC-IV TEXTXPOS and side border**: As in viciv.vhdl, legacy register writes (`$D011/$D016/$D018/$D031` with hot registers) set TEXTXPOS (`$D04C/$D04D`) to the side border width plus twice the `$D016` X scroll (minus 2 in H640), and `$D05C` resets to 80. TEXTXPOS read 0 at READY before.
- **Graphics pane: narrow 16-colour sprites**: A VIC-IV 16-colour sprite without SPRX64EN is 6 pixels wide (3 bytes per row); the Graphics pane, CLI `gfx vic-sprite` and MCP `render_graphics` showed it as 16 pixels from 8 bytes per row. New `sprite16-6` format.
- **Issue #157 - Boot stops at HYPPO's ROM menu without an SD card image**: Not GUI-specific: HYPPO loads `MEGA65.ROM` from the SD card image, so with no `mega65.img` on the `sdcard.paths` list (e.g. a fresh `$HOME` without xemu's files) it waits at its ROM selection menu. The startup warning now says so and names the usual image location.
- **Screenshot / screen colours swapped red and blue**: Video chips disagreed on the frame pixel format: the VIC-II, VIC-20 and PET rendered 0xAARRGGBB, but the VIC-III/IV palette produced 0xAABBGGRR, so a MEGA65 in MEGA65 mode (and the VIC-IV after leaving its locked VIC-II personality) drew red and blue swapped in the GUI, while `screenshot` (CLI/MCP) wrote the C64's and VIC-20's PNGs swapped. Every `IVideoOutput` frame is now documented and produced as 0xAARRGGBB, and `exportPng` converts to R,G,B,A bytes (`IVideoOutput::toRgbaBytes`). A new test checks the VIC-II and VIC-IV (locked and unlocked) render blue and red identically.
- **Issue #127 - Hypervisor entry/exit per gs4510.vhdl**: Traps save the CPU port, VIC I/O mode and DMA list address into `$D650-$D658`, give the hypervisor port `$3F/$35`, MEGA65 I/O, an 8-bit stack and the upper 32KB mapped to `$FFF8000` (the lower mapping is kept), and restore it all on exit. The non-hardware "TAB leaves the hypervisor" shortcut is removed; HYPPO now leaves only through `$D67F`. As in xemu, a minimal flash-utility stand-in is preloaded at `$50000` when no flash utility file is configured, so HYPPO's boot path completes normally.
- **VIC-IV hot registers**: With `$D05D.7` set, writes to `$D011/$D016/$D018/$D031` re-derive the line step, character count, screen and colour RAM pointers (viciv.vhdl); `$D018` also sets the character set pointer.
- **Issue #129 - Typed text reaches BASIC 65**: CLI `type`, MCP `type_string` and GUI text entry now feed the MEGA65 hardware key queue (`$D610`/`$D611`) as well as the CIA matrix, by routing typed keys through the machine's key handler. Shifted symbols are typed as LSHIFT + base key, `\n` is RETURN, and letter casing follows the C64 (ASCII uppercase = unshifted key).
- **HYPPO import**: The MEGA65 ROM importer (`importmega65` CLI command, `import_mega65_roms` MCP tool, GUI pane) now also installs `roms/mega65/HICKUP.M65`. It copies a `HICKUP.M65` from the source directory when present; otherwise it extracts the HYPPO image built into the installed `xemu-xmega65` executable, located by its trap table. New `--xemu <binary>` option.
- **Issue #130 - Boot speed and debug output**: Unconditional stderr debug prints (MapMmu, MEGA65 machine, 45GS02, CIA, VirtualIEC, IEC, HyperSerial, Mega65IoStub, machine loader/registry, KERNAL HLE) now go through named loggers (`mapmmu`, `mega65`, `45gs02`, `cia`, `iec`, `mega65io`, `hyperserial`, `hle`, `system`) at debug/trace level; enable with `log level <name> debug`. `map log on|mem` tracing still prints at info. Hard-coded boot-investigation address windows are gone. Per-step debugger overhead is cut: no `sscanf` in the 6502/45GS02 `disassembleEntry`, trace entries are filled in place in the ring buffer instead of copied (no per-step allocation), and the kernal/basic loggers are cached. A 3M-step MEGA65 boot drops from 3.1s to 1.6s. The GUI run loop no longer calls `wxYield()` every 50 steps and runs 28ms of each 33ms tick (was 25ms).
- **Lazy trace formatting**: While a debugger is attached, the 6502 and 45GS02 cores now call a new `ICpuDisasm::decodeEntry()` on each step (length, call/return/branch flags, target and a new `DisasmEntry::stackOp`, without text). The trace records the raw instruction bytes and register values and formats the text and register map when an entry is first read, disassembling the recorded bytes, so the trace shows the code that ran even if it was later overwritten. Stack tracking and pause-on-BRK use `stackOp`. The 3M-step MEGA65 boot drops further from 1.6s to 0.7s.
- **CIA timer workarounds removed**: Starting a 6526 timer no longer force-enables its interrupt in the ICR mask, and the 65536-cycle "grace period" before the first underflow is gone (along with `setTimerGracePeriod()`). Both were workarounds for the old HYPPO-skipping MEGA65 boot; with them removed, the MEGA65 (BASIC 65 and GO64) and the C64 boot and run BASIC as before, and CIA2 no longer enables NMIs the ROM did not ask for.
- **Issue #139 - VIC-20 typed text casing**: `type`/`type_string`/GUI text entry on the VIC-20 now follow the C64 rule (ASCII uppercase = unshifted key, lowercase = shifted), so `type PRINT 1\n` works instead of producing graphics characters and `?SYNTAX ERROR`.
- **Issue #138 - MEGA65 keyboard matrix**: The MEGA65 CIA1 keyboard matrix was transposed (pressing A read back as R), so C64 mode (GO64) and anything else scanning `$DC00/$DC01` saw the wrong keys; BASIC 65 was unaffected because it reads the `$D610` key queue. Keys now sit at their standard C64/C65 matrix positions, verified for all 64 keys.
- **Issue #130 - MEGA65 boots to READY in under 1s**: Boot to `READY.` (about 5.8M steps) drops from 1.47s to 0.86s in the CLI with identical results. The SID caches its filter coefficients (recomputed only when cutoff, resonance, variant or sample rate change) and skips filter maths on ticks that produce no audio sample; silent idle voices skip the per-voice tick. The debugger's per-step trace records the CPU's `saveState()` snapshot instead of reading each register, reuses the instruction bytes the decoder already read (new `DisasmEntry::raw`), and skips the Lua cycle-event check when none are registered.
- **Issue #149 - Project templates, Debug layout, BASIC tokenizer**: `mmemu-cli --tokenize <in.bas> <out.prg> [--dialect basic2|basic65] [--start <addr>]` tokenises BASIC text (C64/VIC-20/PET BASIC V2 and MEGA65 BASIC 65 including its $CE/$FE two-byte tokens; petcat-style `{clr}`/`{red}`/`{$xx}` escapes; errors as `file:line: error:`). New-project templates (C64 BASIC, MEGA65 BASIC 65, MEGA65 assembly with ca45, MEGA65 C with cc45) via CLI `project templates` / `project new <template> <dir>` and the GUI's Project > New Project. The GUI switches to a Debug layout (CPU, Breakpoints, Stack, Trace, Memory; Source and Symbols) when a breakpoint stops execution and back on Run (View > Debug Layout on Break; View > Edit / Debug Layout); both layouts are saved. Run now also starts the newest `.prg` when the build was up to date, and machine code on machines with BASIC is started by typing `SYS <address>` (BASIC 65 maps ROM over $2000-$3FFF until SYS).
- **Issue #148 - Makefile projects (Build / Run)**: A project is a directory with a Makefile, plus an optional `.mmsim/project.json` (machine, build/run targets, `load`, `reset`, debug symbols and ca45 source maps, environment). **Run** runs `make run` (with `EMU=true`, so a `$(EMU) game.prg` recipe that would start VICE does nothing), then loads the newest `.prg` the build wrote (or `run.load`) and starts it: the machine is reset and booted to READY, programs at the BASIC start get the end-of-program pointer set and `RUN` typed, anything else starts at its load address; disk images are mounted and loaded with `LOAD"*",8,1`. **Build** runs `make all`. Build output is parsed for errors and warnings (gcc/ca45/64tass/cc45, ca65, acme, KickAssembler formats; follows make's directory changes). New non-GUI `libproject` shared by CLI `project open|info|build|run|close`, MCP `project_open|project_build|project_run`, and the GUI: Project menu (Open Project Ctrl-Shift-O, Build Ctrl-B, Run Project Ctrl-F5, Stop Build, Close), `--project <dir>`, Output and Problems panes (activating a problem opens the file at the line), error/warning line markers in the editor, builds on a worker thread, open sources saved before and reloaded after a build (and when the window is reactivated) if they changed on disk. The Breakpoints pane shortcut moves to Ctrl-Shift-B.
- **Issue #147 - Source editor and source-level debugging**: A Source pane (right-hand dock) shows assembly, BASIC and C files in tabbed editors (wxStyledTextCtrl) with syntax highlighting, line numbers, Ctrl-S save and unsaved markers. Clicking the symbol margin toggles an execution breakpoint at the line's first mapped address; breakpoints show as red dots and the executing line as an arrow with a highlighted background. The pane opens on its own when execution stops on a mapped line. New File > Open Source / Save Source and Debug > Load Source Map; Control > Step Into (F11), Step Over (F10), Step Out (Ctrl-F11) step by source line where the PC is mapped and by instruction otherwise, through the machine scheduler (GUI stepping previously skipped device ticks). Source maps come from ca45 `--source-map` files (`SourceMap::loadCa45SourceMap`, also via the new CLI `srcmap load|list|clear`), including their symbol tables. Stepping is shared in libdebug (`source_stepper`); CLI `next` now steps over subroutine calls (it behaved like `step`). The GUI also loads `machines/rawMega65.json` like the CLI and MCP, and reports unknown machine ids.
- **Issue #146 - Dockable GUI layout**: The GUI window now uses `wxAuiManager` docks instead of fixed splitters: the machine screen fills the centre, and tool panes are tabs in a bottom dock and a right-hand dock that appears only when something is in it. Only Console, CPU (registers beside the disassembly) and Breakpoints open by default; Memory, Stack, Trace, Symbols, Machine Explorer, Devices, I/O Watch, Cartridge, MEGA65 Status and plugin panes (tape, drive, ROM importers) are on a new **View** menu, which also has Reset Layout. Tabs can be dragged between the docks; closing a tab hides the pane. Open panes, their dock and the dock sizes are saved in `~/.mmemu` and restored at start-up. `PluginPaneManager::setHost()` lets the host place plugin panes.
- **CPU speed control (CLI, MCP, GUI; MEGA65)**: CLI `speed`, MCP `set_speed` and the GUI Control > Speed submenu (shown only for machines with a speed model, i.e. the MEGA65; it replaces the Warp checkbox) share one implementation (`libcore` `machine_speed`). CLI `speed` reports the current speed: `max` when warp is on (unbounded; devices advance one cycle per CPU cycle), otherwise the CPU clock in MHz. `speed <MHz>` (1, 2, 3.5 or 40/40.5; a trailing `MHz` is accepted) turns warp off and selects that clock the way software would (MEGA65 speed bits, force_fast cleared); `speed max` turns warp back on. New `MachineDescriptor::setCpuSpeedMHz` / `cpuSpeeds`.
- **Issue #142 - SID filter worked only at low cutoffs**: The FilterCurve cutoff tables filled 448 (6581) / 512 (8580) of their 2048 entries, so for every cutoff from about `$1C0` up the filter coefficient was 0 and filtered voices in LP/BP mode went silent; below that the tables re-applied the cutoff and made the coefficient ~10x too small. The cutoff curve is now a multiplier around 1.0 defined for every value (8580 linear, 6581 sagging up to 12% at the top; an approximation, measured curves remain #120), and the resonance tables are indexed across the whole cutoff range so resonance is no longer ignored above `$117`.
- **MEGA65 CPU speed model and warp**: The MEGA65 now selects 1, 2, 3.5 or 40.5 MHz as gs4510.vhdl does (`$D030`.0 C128 2 MHz bit in VIC-II I/O mode, `$D031`.6 FAST, `$D054`.6 VFAST; full speed in the hypervisor or after `POKE 0,65`). With warp off, the VIC-IV, SIDs, CIAs and keyboard run on the ~1 MHz phi clock in real time relative to the CPU, while DMA and the other 40 MHz devices keep the CPU clock; badline stalls apply only below full speed. Warp is on by default and keeps the previous one-device-cycle-per-CPU-cycle timing. New `warp [on|off]` CLI command, `set_warp` MCP tool and GUI Control > Warp menu item; `IORegistry` gains device-clocked handlers and a two-clock `tickAll()`.
- **MEGA65 timing fixes**: `MOS45GS02::step()` returned 1 for every instruction, so the scheduler advanced the devices one cycle per instruction instead of per CPU cycle (about 2.8x too slow); it now returns the instruction's cycles. `$00` writes of `$40`/`$41` set/clear force_fast instead of overwriting the CPU port DDR. `VIC4::D054_VFAST`/`D054_PALEMU` were swapped (VFAST is bit 6), and the VIC-IV now resets with FAST and VFAST set (40.5 MHz) as in viciv.vhdl.
- **Issue #124 - SID hard sync, ring modulation and OSC3**: Hard sync now follows reSID: all three oscillators advance first, then a voice with SYNC restarts at its source's MSB rising edge, at the right cycle within a multi-cycle tick (previously a "source phase < freq" check that depended on voice update order). Ring modulation applies only when the voice's RING bit is set (it was applied to every triangle) and folds the whole ramp on own MSB XOR source MSB instead of flipping output bit 11. OSC3 (`$D41B`) reads the upper 8 bits of voice 3's waveform output (folded triangle, noise, ...) instead of the raw accumulator.

### SID Validation Fixes
- **Issue #137 - SpectralAnalyzer accuracy**: The transform now covers every input sample (inputs longer than `fftSize` previously wrapped around in time) and uses a radix-2 FFT instead of an O(N²) DFT. Peak frequency and magnitude are interpolated between bins, bin-to-frequency conversion no longer uses `fftSize + 2`, and `harmonicRatio`/`thirdHarmonicRatio` come from the peaks identified as harmonics 2 and 3 rather than the 2nd/3rd largest peaks. `SIDValidationSuite::formatReport()` prints the report summary. Corrected the expected range in the high-resonance spectral-error test to match the documented RMS metric.

### IDE Integration & Debugger Bridges - Issues #31, #88, #92-100
- **Issue #31 - VICE Snapshot (.vsf) Import/Export Foundation**: Infrastructure and comprehensive documentation for loading and saving VICE emulator snapshots enabling cross-emulator compatibility. VICE_SNAPSHOTS.md provides 300+ lines of format specification, architecture design, usage examples, and roadmap. ViceSnapshotLoader/Saver class interfaces designed for CPU/RAM state serialization and extensible module-based device support. Enables importing VICE save states for debugging, exporting mmemu states for validation, and leveraging VICE tool ecosystem. Status: Documentation complete, implementation architecture ready.
- **Issue #88 - VICE Monitor Protocol Support**: Complete VICE remote monitor protocol implementation enabling compatibility with VICE-based debugging tools and IDEs (C64IDE, etc.) as emulation backend. ViceMonitorProtocol class implements command dispatch for register/memory/breakpoint/disassembly operations. ViceMonitorServer provides TCP server on configurable port (default 6510, VICE standard). Supports address parsing (hex/$XXXX, 0xXXXX, decimal, binary), register access through ICore interface, memory operations via IBus, breakpoint management through DebugContext. New `--vice-monitor-port` CLI flag. Comprehensive VICE_PROTOCOL.md documentation. Enables C64IDE, mflash, and other VICE-compatible tools to use mmemu without modification.
- **Issue #92 - C64IDE Symbol Database Import**: Comprehensive 150+ symbol database extracted from C64IDE including KERNAL/BASIC ROM entry points, VIC-II/SID/CIA registers, and special memory locations. New CLI command `sym load-c64ide` automatically loads symbols for current machine (`roms/c64/c64ide_symbols.sym`, `roms/vic20/vic20_symbols.sym`). Enables annotated disassembly and register-aware debugging without external symbol files.
- **Issue #93 - Stack Trace Viewer**: Display call stack with return addresses, function names from symbols, and stack frame boundaries. CLI `stack` command shows function context.
- **Issue #94 - Variable Symbol Table**: Foundation for symbol-aware debugging. VariableSymbolTable maps variable names to memory locations, supports function-scoped and global variables with type information.
- **Issue #95 - Source Location Hyperlinking**: Clickable source links in disassembly and breakpoint output, integrated across CLI/GUI/MCP. Resolves .c/.s files from symbol search paths.
- **Issue #96 - Frame Inspection**: Analyze stack frame layout with `frame` CLI command. Shows variables in memory order with gap detection, uninitialized value tracking, and struct-like visualization. Supports both table and pseudo-C layout formats.
- **Issue #97 - Enhanced Breakpoints** (#97): Conditional breakpoints with expression evaluator (e.g., `break when x == 0x01`), hit count limits (`break $2050 count 5`), and memory value watches (`watch value $2000 4`). Breakpoint display shows conditions, hit counts, and memory watch sizes.
- **Issue #98 - Debug Metadata Format** (#98): Standardized metadata format emitted by cc45 compiler as assembly comments. DebugMetadataParser/Registry classes enable symbol-aware debugging without external debug files. Format: `; .debug_var: function var_name offset=N size=N type=TYPE scope=SCOPE [src_line=N] [src_file=FILE] [name=DISPLAY_NAME]`. Integrated with VariableSymbolTable.
  - **Issue #98 Follow-up - Debug Metadata Tools**: CLI `load-debug-metadata` and `vars` commands, MCP `load_debug_metadata` and `list_variables` tools, GUI VariablePane widget. Complete end-to-end variable inspection across all frontends (CLI/GUI/MCP).
- **Issue #99 - Execution History and Reverse Debugging**: User-facing tools for execution history inspection built on existing TraceBuffer infrastructure. CLI `log` command with filtering options (`log show`, `log show -memory`, `log show -calls`), MCP tools for remote access (`get_execution_history`, `get_memory_access_history`). Enables debugging workflow: hit breakpoint → examine what led to it → step backwards → inspect state.
- **Issue #100 - GDB Protocol IDE Integration** (#100): Enhanced GDB Remote Serial Protocol server with mmemu-specific metadata exchange. New query commands: `qMmemuSymbols` (symbol table), `qMmemuVariables` (variable info), `qMmemuFrame` (frame info). Responses are hex-encoded JSON for compatibility. Enables IDE integration with VS Code, CLion, and other GDB-compatible debuggers. Full documentation for VS Code setup and debugging workflow.

### Hardware & Emulator Cross-Validation Testing Framework
- **Extended Bridge Support**: Added XemuTestBridge for subprocess-based xemu-xmega65 integration. Launches xemu with `-dumpmem` flag, captures memory results. Enables three-way comparison (mmsim vs xemu vs real hardware).
- **Enhanced CrossValidationRunner**: New factory methods `withXemu()` (two-emulator comparison) and `withAll()` (three-way validation). ComparisonResult now includes xemuPass, xemuMemory, xemuOutput fields.
- **Three-Way Validation Logic**: Uses mmsim as reference. Compares against xemu and hardware independently. Helps distinguish emulation bugs (mmsim differs from both) vs hardware-specific quirks (mmsim and xemu agree, hardware differs).
- **Virtual runTest Method**: Allows XemuTestBridge to override test execution via subprocess model.
- **Documentation**: New XEMU_VALIDATION.md with complete examples, factory method reference, result interpretation guide, performance notes.

### Hardware Cross-Validation Testing Framework
- **Hardware Test Runner Bridge**: Unified interface for running test programs on emulator (TCP to SerialMonitorServer) and real MEGA65 hardware (USB-UART serial port). Implements MEGA65 serial monitor protocol (M, S, R, D, G, T commands). Two backends: EmulatorTestBridge (TCP), HardwarePortBridge (serial port with termios configuration).
- **CrossValidationRunner**: High-level test orchestration with factory methods `withEmulator()`, `withHardware()`, `withBoth()`. Supports batch test execution, automatic memory comparison, detailed diff reporting. Gracefully degrades when hardware/emulator unavailable.
- **Test Results Struct**: Captures execution cycles, memory snapshots, serial output, error messages for each test. Comparison identifies emulation bugs vs hardware behavior divergence.
- **API**: `loadMemory()`, `readMemory()`, `readRegister()`, `writeMemory()`, `setPC()`, `step()`, `run()`, `runTest()`. Unified semantics across emulator and hardware.
- **Serial Setup**: USB-to-UART adapter (FTDI/CP2102) connected to MEGA65 JTAG/serial pins 2-3 (RX/TX). Auto-detects `/dev/ttyUSB0` on Linux. Default baud rate 2,000,000 bps.
- **Documentation**: Complete HARDWARE_VALIDATION.md with examples, hardware setup, API reference, debugging guide, integration patterns.
- **Test Suite**: 7 new integration tests validating API structure and factory methods. All 709 tests passing.

### Serial Monitor Server - Issues #112-115
- **Issue #112 - Phase 1: Core Implementation** ✅ Complete: TCP socket listener with register/memory/disassemble/PC commands, text-based protocol over localhost.
- **Issue #113 - Phase 2: Advanced Commands** ✅ Complete: Breakpoint/watchpoint management, trace/history inspection, UART divisor configuration, debug metadata access.
- **Issue #114 - Phase 3: Tool Integration** ✅ Complete: Full command dispatch, Python integration tests, MCP tool bridges.
- **Issue #115 - Phase 4: IDE Integration** ✅ Complete: 
  - **Python Client Library** (`tools/mmemu_client_py/`): Production-ready TCP client with full serial monitor protocol support, address parsing, register/memory/breakpoint/watchpoint/disassembly operations, debug metadata access, and utility functions. Includes `setup.py` for pip installation.
  - **Example Tools**: `memory_dump.py` (memory export), `breakpoint_manager.py` (interactive editor), `variable_inspector.py` (variable display with types).
  - **VS Code Extension** (`tools/vscode-mmemu/`): TypeScript/Node.js client, extension entry point, tree views for variables/memory/breakpoints, Debug Adapter Protocol factory, command integration, keyboard shortcuts.
  - **Protocol Documentation** (`PROTOCOL.md`): Complete reference for serial monitor text-based protocol, command formats, response examples, address parsing, error handling.
  - **Architecture & Integration Guide** (`PHASE4_IMPLEMENTATION.md`): Comprehensive implementation status, file structure, usage examples, testing procedures, future enhancements roadmap.

### MEGA65 Integration & Contention Model
- **Issue #70 - Raw MEGA65 with I/O Hardware** (#70): New machine variant `rawMega65_io` provides minimal MEGA65 configuration for hardware register testing. Includes 45GS02 CPU with VIC-IV, SID (SidPair), CIA1/CIA2, F018B DMA, math accelerator, and RTC at standard I/O addresses. No KERNAL/HYPPO/boot sequence. Enables cc45 compiler hardware register tests and isolated I/O device testing. Configured via JSON machine descriptor with proper device wiring and signal connections (IRQ/NMI).
- **Dual SID (SidPair) wired to MEGA65 machine** (#6): SidPair was included but never created or registered. Now created with PAL clock ($D400/$D420) and registered in IORegistry.
- **MEGA65 keyboard buffer** ($D610/$D619/$D60A/$D611): ASCII/PETSCII typing queue with modifier tracking. PC→MEGA65 function key remapping (PC F5→MEGA65 F1, etc.). Immediate modifier state at $D611.
- **DMA MIX (MINTERM) operation** (#5): Combines source and destination bytes using a 4-entry boolean truth table. Command/subcommand bits 4-7 select which combinations contribute, enabling arbitrary boolean operations (OR, AND, XOR, etc.) between memory regions.
- **DMA IRQ on completion** (#5): Command byte bit 3 fires the DMA IRQ line when the job finishes. Wired via `setIrqLine(ISignalLine*)`.
- **I/O stalls and DMA bus contention** (#20): VIC-II badline detection steals 40-43 cycles from CPU per badline. Shared `phi_backlog` counter between VIC-IV and MEGA65 scheduler matches VHDL `gs4510.vhdl` contention model. Configurable via `setBadlineEnable()` and `setBadlineExtraCycles()` (MEGA65 $D710 bits 0, 4-5).
- **Math accelerator divide-by-zero**: Returns all-ones ($FFFFFFFFFFFFFFFF quotient, $FFFFFFFF remainder) matching MEGA65 hardware behavior.
- **load_image writes to physical bus** (#79, #80): All load operations (CLI, GUI, MCP) now write to the physical bus directly, bypassing MAP translation. Ensures data lands at the literal PRG address regardless of MAP state.
- **Physical-address breakpoints** (#73): `break phys $02CBA5` in CLI, `physical: true` in MCP. Resolves CPU's virtual PC through MapMmu to 28-bit physical address before comparing. Catches the same ROM code regardless of MAP block. MapState moved to `imap_controller.h` with `resolvePhysical()` default implementation.
- **MCP conditional breakpoints** (#18): `condition` parameter on `set_breakpoint` and `set_watchpoint` MCP tools. Conditions shown in `list_breakpoints` output. Supports full expression syntax including `*addr` memory dereference.
- **I/O address map command** (#67): CLI `iomap $D770` shows which device claims an address. `iomap` lists all registered handlers with ranges.
- **Memory dereference operator** (#75): `*$1000` reads byte, `*$1000:16` reads 16-bit LE word. Works in breakpoint conditions, `run_until`, all expression contexts. Binary multiply still works when left operand present.
- **SD card and HDOS config in JSON** (#50): SD card image paths and HDOS root directory paths now fully driven by `mega65.json`. No hardcoded fallbacks in factory.
- **GUI function keys**: All F1-F12 plus ALT key events now passed to machine handlers.
- **MCP performance profiling tools** (#19): `profile_cpu` runs N steps sampling PC to build a frequency histogram, returns top hotspots with symbol annotations, percentages, and disassembly. `measure_region` runs from addr until PC leaves range, returns total cycles, instruction count, and average CPI.
- **MCP plugin tool dispatch fix**: Plugin-registered MCP tools appeared in `tools/list` but calling them returned "Unknown tool". Now routes through `PluginToolRegistry::dispatch()` before the catch-all error.
- **MCP plugin tool integration tests** (#17): Python tests verify plugin tools appear in `tools/list` and exercise `profile_cpu`/`measure_region` with structured JSON validation.
- **CLI commands bridging MCP gaps**: `profile` (hotspot analysis), `measure` (cycle counting for address range), `runto` (run until condition expression true), `devinfo` (device register dump), `undoinfo` (show what backstep would undo).
- **GUI ToolRunnerDialog framework** (#82, #83, #84): Generic wxDialog auto-generating input controls from field definitions, rendering results as summary text + sortable wxListCtrl table. ExpressionTextCtrl with register/symbol autocomplete popup. Tools menu: Profile CPU, Measure Region, Run Until, Device Info. Six existing dialogs (Fill/Copy/Swap Memory, Go To Address, Search Memory, Load Image) converted to ToolRunnerDialog with new `choice` and `file` field types. Old dialog files removed.
- **GUI device register editing** (#8): Double-click or right-click any register in DeviceInfoPane to edit its value. Hex input with validation, writes via `ioRegistry->dispatchWrite()`, auto-refreshes.
- **Memory heat map** (#25): Shared `MemoryHeatMap` data layer in libdebug tracking per-address read/write counts. CLI `heatmap` (top hotspots / ASCII page overview), MCP `get_heatmap`/`heatmap_control`, GUI Tools menu with sortable table. Enable/disable/reset control.
- **Raster beam visualization** (#27): VIC2 raster position exposed via `rasterLine()`/`rasterCycle()` accessors and DeviceInfo state entries. CLI `raster`, MCP `get_raster_position` (JSON with line/cycle/region/mode), GUI Tools dialog. All interfaces use DeviceInfo — works with VIC-II/III/IV without type-specific coupling.
- **C64IDE integration analysis**: Evaluated integration paths with C64IDE (macOS-only Swift IDE). VICE monitor protocol, MCP JSON-RPC, and embedded library approaches documented. Non-UI C64IDE components (BASIC toolchain, assembler, disk parsers, Claude AI, disassembler, hardware deployment) scoped for porting to M65Compiler repo. Debugger protocol client (#88) and ROM symbol import (#92) remain in mmsim.
- **New integration tests** (#6, #7): MAP translation, DMA stall, VIC-IV raster, dual SID, math accelerator, personality switching, DMA MIX/IRQ, plus existing VIC-IV, DMA copy/fill/chain tests. 581 C++ tests total.

### MEGA65 Boot to BASIC
- **MEGA65 boots to READY. prompt** with proper 80-column display, colour palette, rainbow sprite bars, and real-time clock. HYPPO boot stage skipped; starts directly at C65 KERNAL reset vector ($E4B8).
- **Real-Time Clock (RTC) device** (#76): Emulates MEGA65 I2C RTC at $FFD7110-$FFD7115. Returns host system date/time in BCD. Writes create a machine-local offset (does not affect host clock). Boot banner now shows actual date/time instead of "NO-RTC-2000".
- **DMA line drawing mode** (#74): Bresenham-like line rendering via enhanced DMA options $87-$8F/$97-$9F. Slope accumulator with card boundary handling for VIC-IV 8×8 pixel layout.
- **CLI run loop responsiveness** (#72): SIGINT handler for Ctrl-C, decimal cycle count argument (`run 5000000`), status reporting.
- **MEGA65 boot integration test**: Verifies boot completes with banner text and uncorrupted SP (test #551).
- **PHW push order** (fixes boot crash): PHW was using push16() (high-byte-first) but VHDL pushes low-address-byte first. Fixed to push8(lo); push8(hi) matching VHDL PushWordLow/PushWordHigh. This was the root cause of the Get_DOS trampoline crash.
- **JMP ($abs,X)**: Opcode $7C was wrongly implemented as EOM/NOP. Implemented as 65C02 indirect indexed jump, required for KERNAL escape/control character dispatch tables.
- **VIC-IV SCRNPTR default**: $D060-$D062 initialized to $000400 matching VHDL `screen_ram_base := x"0000400"`. Was $000000, causing DMA screen clear to fill zero page (clobbering cbdos pointer at $C8).
- **VIC-IV palette nibble swap**: Palette values expanded using VHDL nibble-swap (low nybl → high output bits, high nybl → low output bits) instead of incorrect `*17` linear scaling.
- **Colour RAM physical bus mapping**: Colour RAM buffer mapped to physical bus at $080000 (DMA attribute fill target) and $0FF80000 (I/O space). DMA fills were writing to SparseMemoryBus, not the shared colour RAM buffer used by VIC-IV rendering.
- **ATTR mode screen RAM**: ATTR ($D031 bit 5) enables extended colour attributes but does NOT change screen RAM to 2-byte per character. Only CHR16 ($D054 bit 0) makes screen codes 2 bytes. The KERNAL uses 1-byte screen codes with ATTR mode.
- **CIA SDR shift register flag**: Set ICR bit 3 (SP) on SDR write when CRA bit 6 is set (output mode), matching xemu's `CIA_OUT_SDR_SETS_ICR_BIT3`. Prevents IEC serial polling loops from hanging (mega65-rom-public#169).
- **KernalHLE simulateRts**: Was truncating 16-bit SP to 8 bits, corrupting SPH on 45GS02. Now checks RegWidth and E flag for correct 8-bit/16-bit stack handling.
- **HDOS getdisksize ($08)**: Virtualized to prevent entering uninitialized HYPPO. Returns dummy disk size.
- **VIC2 PAL timing**: Configurable via setPal(). Default PAL (63 cycles/line, 312 lines). Was hardcoded NTSC (65/263).

### Added
- **Commodore Plus/4 Architecture Plan** (`.plan/plus4.md` & Phase 25 in `.plan/todo.md`): Detailed roadmap covering MOS 7501/8501 CPU, MOS 7360/8360 TED chip, 121-color RGBA video engine, 2-channel audio subsystem, MOS 6551 ACIA, and machine factories for Plus/4, C16, and C116.
- **Commodore 128 Architecture Plan** (`.plan/c128.md` & Phase 34 in `.plan/todo.md`): Detailed roadmap covering MOS 8502 CPU, Zilog Z80A core, MOS 8722 MMU, VDC 8563 80-column video controller, VIC-IIe, and machine factory.
- **MCP test automation tools** (#71): `test_sequence` (batch commands), `test_assert` (load+run+assertions), `test_diagnose` (watchpoint root cause analysis).
- **MCP `stop_on_brk`**: Flag on `run_cpu` and `test_assert` to halt before executing BRK instruction.
- **MCP `run_until` precision**: Per-step condition checking by default. `loose=true` for every-256-step performance mode.
- **45GS02 `setHypervisorMode()`**: Direct flag control for reset override.
- **CIA `ICR_SP` constant**: Serial port shift register flag ($08).

### Fixed
- **`mflash.prg` PRG header loading**: Stopped skipping the 2-byte load address header when loading `mflash.prg` into physical BRAM at `$050000`. The `HYPPO` hypervisor expects the 2-byte header to be present to correctly copy the program to RAM. Skipping it caused a 2-byte alignment shift that misaligned subroutines (such as `pushax` ending up at `$74B2` instead of `$74B4`), resulting in corrupted return addresses and crashes at `$03D3`.
- **`C64BankController` hypervisor condition**: Refined the `inHypervisor()` read condition in `C64BankController::ioRead()` to allow ROM reads at or above `$E000` (block 7) in hypervisor mode. This ensures the trampoline's `JMP ($FFF6)` fetch resolves to the KERNAL reset/boot vector rather than falling through to unmapped RAM.
- **I/O device clock ticking**: Corrected `schedulerStep` in `machine_mega65.cpp` to tick all I/O devices by the actual CPU instruction cycles (`cycles`) instead of a hardcoded `1` cycle. This keeps the VIC4/VIC3 raster line counters in sync with the CPU execution rate.
- **45GS02 E-flag stack wrapping**: In 8-bit stack mode (E flag set), `push8`/`pull8` incorrectly wrapped the stack pointer within the B (base page) register instead of the current stack page (SPH, set via TYS). This caused corrupted return addresses during HYPPO boot where B=$BF and SPH=$BE are intentionally different pages.
- **VIC3 shadowed F011 FDC range**: VIC3 claimed $D080-$D09F and returned $FF, blocking HYPPO's F011 FDC register polling at $D086. Narrowed VIC3 to $D0A0-$D0FF, leaving $D080-$D09F free for the FDC.
- **SD card command $53 (flash read)**: Was returning a hardcoded CSD identification response; real MEGA65 uses $53 as a sector read from flash. Sector 0 returns flash identification ($DE71=$01 for card presence check), non-zero sectors read from the mounted SD card image, enabling HYPPO to load C65 ROMs during boot.
- **Enhanced DMA option parsing**: Options with bit 7 clear ($0A=F018A, $0B=F018B, $06/$07=transparency) were not handled, causing the option parser to misalign job data. Added per-job megabyte options ($80/$81) and per-job F018A revision flag.
- **F018B DMA: CPU halts permanently after DMA job** (fixes #45): The 45GS02 `haltLine` flag was latched on when the bus requested halt (DMA active) and never cleared when the bus released. CPU now re-checks bus halt status each step and clears `haltLine` when DMA completes.
- **45GS02 BRK instruction**: BRK was a no-op that set `haltLine`, halting the CPU. Now properly implemented as a software interrupt: pushes PC+2 and flags (with B flag set), vectors through $FFFE/$FFFF, sets I flag and clears D flag.
- **`isProgramEnd` false positives during DMA**: Removed bus halt check from `isProgramEnd`; `haltLine` alone is sufficient since the scheduler handles transient DMA halts without calling `step()`.
- **MCP `step_cpu` tool**: Now uses `schedulerStep` when available, ensuring DMA ticks are processed during stepping (matching CLI and GUI behavior).
- **Physical bus routing for 32-bit addressing**: The 45GS02's `write8_phys`/`read8_phys` routed through MapMmu, which masked 28-bit addresses to 16 bits. HYPPO's flash read loop (`STA [$18],Z` writing to physical `$0005_8000+`) was truncated to `$8000`, overwriting hypervisor RAM. Now routes directly to SparseMemoryBus via `IMapController::getPhysBus()`.
- **DMA bus clobbered by ioWrite**: The F018B DMA device's `m_bus` was overwritten on every register write, replacing the physical bus (set via `setDmaBus`) with the MapMmu. 28-bit DMA addresses were truncated to 16 bits.
- **DMA megabyte option zero value**: Enhanced DMA option `$80 $00` (srcMB=0) was treated as "not set" because zero is falsy. Added explicit `srcMBset`/`dstMBset` flags. Chained jobs now inherit megabyte settings.
- **C65 ROM banking via $D030**: Added all four ROM banking regions (ROM8/ROMA/ROMC/ROME) using ioRead interception instead of SparseMemoryBus overlays. Overlays were permanently removed when CPU port changed HIRAM. The MEGA65 KERNAL reset at `$E4B8` sets `$D030=$20` (ROMC) and jumps to `$C800` — the MEGA65 extended KERNAL.
- **BRK with uninitialized IRQ vector** (fixes #51): BRK now properly detected as program end when IRQ vector ($FFFE/$FFFF) is `$0000` or `$FFFF`, preventing infinite BRK loops on rawMega65.
- **Hypervisor RAM as physical bus region**: m_hyperRam registered as writable region on SparseMemoryBus at $FFF8000. DMA writes to $FFF8000+ update m_hyperRam directly, keeping CPU overlay and DMA in sync. Fixes longpeek and task_set_pc_to_reset_vector.
- **isProgramEnd skips hypervisor mode**: Never treat hypervisor execution as program end. Also improved RTS empty-stack check to skip when IRQ vector is set (full machine with ROM).
- **Hypervisor RAM restore after mflash**: Workaround for HICKUP.M65/mflash.prg version mismatch. Detects mflash→HYPPO return and restores zeroed hypervisor RAM region.
- **Keyboard buffer $D610**: Write-to-clear semantics. mflash's keyboard flush loop wrote then read $D610, getting back the non-zero value it wrote, causing infinite loop.
- **F011 FDC stubs ($D080-$D09F)**: Status A ($D082) now returns 0 (not busy) instead of $FF. mflash polled $D082 bit 7 and hung when it read $FF (always busy).
- **MAP upper-32K blocked in hypervisor mode**: MAP instruction offset/enable changes for $8000-$FFFF are blocked in hypervisor mode, matching xemu/real hardware. Only megabyte selection allowed.
- **Hypervisor RAM restore after mflash**: Workaround for HICKUP.M65/mflash.prg version mismatch. Detects mflash→HYPPO return and restores zeroed hypervisor RAM region.
- **45GS02 Y register corruption in hypervisor transitions**: Y register was always saved as 0 in `enterHypervisor()` and never restored in `exitHypervisor()`. SYSCALL traps and other hypervisor transitions would corrupt Y, breaking loop control variables and array indices. Now properly saves actual Y value on entry and restores it on exit.
- **45GS02 infinite loops on repeating 0x42 instruction prefixes**: Unbounded prefix parsing loops in `step()`, `disassembleOne()`, and `disassembleEntry()` could hang indefinitely on code with repeating `0x42` bytes (common in compiler-generated binaries, padding, and data sections). Added `MAX_PREFIXES = 16` iteration limit to all three loops, preventing hangs while preserving correct instruction decoding.
- **Disassembler incomplete 32-bit indirect opcode coverage**: `isInfiniteLoop()` and related analysis functions only checked 3 of 8 valid 32-bit indirect addressing opcodes (`0x12`, `0x92`, `0xB2`), missing `0x32`, `0x52`, `0x72`, `0xD2`, `0xF2`. Now checks all 8 opcodes, matching the instruction executor and enabling correct program analysis.
- **PRG loader diagnostic logging**: Added logging to display load address range for all PRG files loaded (format: `"Loaded PRG ($2000-$22C1) 706 bytes"`). Helps debug address conflicts, truncation issues, and data section presence verification.

### Added
- **MCP `run_until` tool**: Run the CPU until a condition expression becomes true, with configurable report output (regs, disasm, stack, mem). Supports compound conditions with `&&`, `||`.
- **Expression evaluator logical operators**: Added `&&` (logical AND) and `||` (logical OR) at lowest precedence.
- **HDOS trap virtualization framework**: HypervisorRegs intercepts trap 0 (DOS) from user mode, checks function code from A register, and can service requests from host filesystem. Initial stubs for get_drive_info, set_filename, find_file, chdir, cdrootdir, closeall.
- **MEGA65 JSON machine descriptor** (`machines/mega65.json`): All ROM paths, BRAM files, SD card paths, I/O defaults defined in JSON. Factory reads config at startup.
- **JsonMachineLoader SparseMemoryBus support**: Bus type field selects FlatMemoryBus or SparseMemoryBus.
- **MCP `load_image` tool address range display**: Enhanced to show complete address information when loading programs. Output format: `"Loaded '<path>' ($<start>-$<end>) <count> bytes"`. Provides consistent visibility across CLI, GUI, and MCP frontends.

### Changed
- **Version tracking**: Single source of truth in `VERSION` file; Makefile reads it to generate `version.h`. Version format changed from `MAJOR.MINOR.PATCH-GITHASH` to `MAJOR.MINOR.PATCH.GITHASH`.

## [0.3.2] - 2026-05-28

### Added
- **CBM Disk Image Format Support** (`src/plugins/cbm-loader/`):
    - D71 (1571 double-sided, 70 tracks, 349,696 bytes)
    - D80 (8050, 77 tracks, 533,248 bytes)
    - D81 (1581, 80 tracks, 819,200 bytes — used by MEGA65)
    - D82 (8250 double-sided, 154 tracks, 1,066,496 bytes)
    - All formats support error-info variants (appended per-sector error bytes)
    - `CbmSectorDisk` base class with geometry-driven shared logic for directory parsing, file chain reading, disk name/ID extraction, and BAM-based free block counting
    - `DiskImageLoader` now dispatches `.d71`, `.d80`, `.d81`, `.d82` extensions
    - 10 new unit tests: format open/read, geometry validation, size rejection, extension dispatch
- **MCP `diff_file` Tool** (`src/mcp/main/main.cpp`):
    - Compare two binary files byte-by-byte
    - Summary: file sizes, changed byte count/percentage, region count
    - 6502 vector table comparison (NMI, RESET, IRQ) with change detection
    - Per-region hex dump with configurable context bytes
    - Optional symbol annotations via `machine_id` (nearest label + offset)
- **MCP Snapshot Tools** (`src/mcp/main/main.cpp`):
    - `snapshot_save`: Capture named snapshot of CPU registers and memory (configurable range)
    - `snapshot_diff`: Compare two snapshots — shows register changes and memory diff regions with symbol annotations
    - `snapshot_list`: List all saved snapshots for a machine instance
    - `snapshot_delete`: Remove a snapshot (or all with `"*"`)
- **MCP `analyze_routine` Tool** (`src/mcp/main/main.cpp`):
    - Control-flow walking disassembler from an entry point
    - Follows branches (both paths), unconditional jumps; stops at RTS/RTI/BRK
    - Detects loops (backward branches), subroutine calls (JSR with dedup + count)
    - Identifies I/O accesses ($D000-$DFFF) as read/write with symbol annotations
    - Reports indirect jumps (JMP ($xxxx)) as unresolvable exit points
    - `recursive` mode: follows into JSR targets to analyze subroutine internals
    - Reports max call depth (deepest JSR nesting level)
    - Configurable max_instructions limit (default 200, max 5000)
- **MCP `generate_tests` Tool** (`src/mcp/main/main.cpp`):
    - Automated test vector generation for 6502 routines
    - Runs a routine with all combinations of input register values
    - Captures output registers after routine returns (RTS/RTI/BRK detection via call-depth tracking)
    - Saves/restores full machine state between test runs
    - Configurable: input/output registers, test values, max steps per run
    - Handles nested JSR calls (tracks call depth, only stops at outermost return)
    - Outputs formatted table with input→output mappings, step count, completion status
- **MCP `record_audio` Tool** (`src/mcp/main/main.cpp`):
    - Run the CPU while recording audio output to a 16-bit PCM WAV file
    - Auto-detects audio device (SID, VIC-I, POKEY) via `IAudioOutput` interface
    - Supports mono (single SID/VIC) and stereo (dual SID / SidPair)
    - Configurable duration (default 1s, max 60s)
    - Periodic sample pulling during CPU execution to avoid ring buffer overflow
- **MCP `load_sid` Tool** (`src/mcp/main/main.cpp`):
    - Load PSID/RSID (.sid) files from the HVSC collection
    - Parses v1/v2 headers: title, author, copyright, load/init/play addresses, song count
    - Loads SID data into machine memory at the correct address
    - Calls init routine with selected subtune in accumulator
    - Installs play loop at $0002 for use with `record_audio`
    - Supports subtune selection (1-based, defaults to startSong from header)
- **Time-Travel Debugging** (`src/libdebug/main/trace_buffer.h/cpp`):
    - `TraceEntry` extended with `memWrites` field recording (addr, old_value) for each bus write
    - `TraceBuffer::reverseStep()` pops the last entry, undoes memory writes in reverse, restores registers
    - `DebugContext::reverseStep()` convenience method
    - `CircularBuffer::popBack()` and `back()` methods added
    - MCP `reverse_step` tool: reverse N instructions with register display
    - MCP `undo_info` tool: buffer size, capacity, memory estimate
    - CLI `backstep` / `bs` command for interactive reverse stepping
    - Tool count: 51 → 62
- **I/O Register Watch Pane** (`src/gui/main/register_watch_pane.h/cpp`):
    - New "I/O Watch" tab in the GUI notebook
    - wxGrid showing all registers for the selected device: Address, Name, Hex, Binary, Decoded
    - Bit-field decoding via new `BitField` struct in `DeviceRegister` (device_info.h)
    - Change highlighting: cells turn yellow when values change between refreshes
    - Device selector dropdown, refreshes only when visible (30 FPS timer)
    - VIC-II registers annotated with bit fields: SCR1 (YSCROLL/RSEL/DEN/BMM/ECM/RST8), SCR2 (XSCROLL/CSEL/MCM), VMEM (CB/VM), IRQ, IRQEN
- **GUI Back Step & Trace Pane** (`src/gui/main/main.cpp`, `trace_pane.h/cpp`):
    - "Back Step" toolbar button (back arrow icon) and menu item (Shift+F11)
    - Calls `DebugContext::reverseStep()` and refreshes all panes
    - New "Trace" notebook tab showing instruction trace buffer as a list
    - Columns: #, PC, Instruction, A, X, Y, SP
    - Right-click "Rewind to here" context menu: repeatedly reverse-steps until the selected trace entry is reached
    - Status bar shows rewind progress
- **GDB Remote Serial Protocol Server** (`src/cli/main/gdb_server.h/cpp`):
    - TCP listener on configurable port (`--gdb-port <port>` CLI flag)
    - Full RSP packet framing with checksums and ACK/NAK
    - Register read/write (`g`/`G`) with 6502 register mapping (A, X, Y, SP, PC, P)
    - Memory read/write (`m`/`M`) via `peek8`/`write8`
    - Single step (`s`) and continue with async interrupt (`c` + Ctrl+C detection)
    - Software breakpoints (`Z0`/`z0`) via `BreakpointList`
    - Query support (`qSupported`, `qAttached`, thread info)
    - Detach (`D`) and kill (`k`) commands
    - Runs in background thread, coexists with CLI
    - 12 Python integration tests (`make test-gdb`)

### Fixed
- **D64 getDiskName()**: Read disk name from wrong sector (18/1 directory instead of 18/0 BAM). Fixed to use geometry-defined header sector.
- **D64 getFreeBlocks()**: BAM entry offset was off by 4 bytes (read from sector start instead of skipping the 4-byte header). Fixed via configurable `bamEntryOffset` in geometry.
- **D64 readFile()**: Removed leftover `std::cerr` debug statements.

## [0.3.0] - 2026-05-19

### Added
- **VIC-III (CSG 4567) Implementation** (`src/plugins/devices/vic3/`):
    - 256-color palette ($D100-$D3FF: R, G, B channels)
    - $D030 banking register (CRAM2K, EXTSYNC, PAL, ROM8/A/C/E, CROM9)
    - $D031 mode control (H640, FAST, ATTR, BPM, V400, H1280, MONO, INT)
    - 80-column text mode (H640 bit) with half-width character cells
    - Bitplane mode (BPM) with up to 8 planes, complement flags, X/Y offsets
    - Extended attributes (ATTR) with 8-bit colour register access
    - Bitplane enable ($D032), addresses ($D033-$D03A), DAT ports ($D040-$D047)
    - Personality lock: C64 mode delegates to VIC-II, C65 mode uses VIC-III
    - Plugin registered as device type "4567"
- **VIC-IV Full Colour Mode (FCM)**:
    - 8×8 character cells with 64 bytes of pixel data (one byte per pixel = palette index)
    - FCLRLO ($D054.1): FCM for character numbers ≤ $FF
    - FCLRHI ($D054.2): FCM for character numbers > $FF
    - CHR16 ($D054.0): 16-bit character numbers from screen RAM
    - Pixel value $FF selects foreground colour from colour RAM
    - $D054 bit constants (CHR16, FCLRLO, FCLRHI, SPRH640, VFAST, PALEMU, ALPHEN)
- **VIC-IV Nibble-Colour Mode (NCM)**:
    - 4 bits per pixel, 16-pixel-wide characters (2 pixels per byte)
    - Lower 4 bits from character data nibble, upper 4 from colour RAM
    - Nibble $F selects full colour RAM value as palette index; $0 = background
    - Per-character selection via colour RAM bit 3 when CHR16 enabled
    - Shares 64-byte character data format with FCM
- **VIC-IV Variable Display Geometry**:
    - $D05E (CHRCOUNT): configurable characters per row (0 = default 40/80)
    - $D07B (DISPROWS): configurable text rows (0 = default 25)
    - $D058-$D059 (LINESTEP): bytes between screen RAM rows (16-bit, 0 = auto)
    - Enables non-contiguous screen RAM layouts
- **VIC-IV Sprite Extensions**:
    - Extended height ($D055 SPRHGTEN per-sprite, $D056 SPRHGHT 0-255px)
    - 16-colour sprites ($D06B SPR16EN): 4 bits/pixel, color = sprite# × 16 + nibble, nibble 0 = transparent
    - 64-pixel wide mode ($D057 SPRX64EN): 8 bytes per row
    - Sprite pointer relocation ($D06C-$D06E SPRPTRADR, 24-bit base)
    - 16-bit sprite pointers ($D06E.7 SPRPTR16)
    - Sprite Y MSBs ($D077 SPRYMSBS): 9-bit Y positioning
- **VIC-IV Display Geometry Registers**:
    - $D048-$D049 (TBDRPOS): top border position (12-bit)
    - $D04A-$D04B (BBDRPOS): bottom border position (12-bit)
    - $D04C-$D04D (TEXTXPOS): text X position (12-bit)
    - $D04E-$D04F (TEXTYPOS): text Y position (12-bit)
    - $D05C-$D05D (SDBDRWD): side border width
- **VIC-IV Palette Banks** ($D070):
    - SPRPALSEL, BTPALSEL, ABTPALSEL, MAPEDPAL (2 bits each)
- **VIC-IV System Flags**:
    - $D054.5 (VFAST): 40 MHz CPU mode
    - $D06F.7 (PALNTSC): PAL/NTSC timing selection
- **VIC-IV 16-Colour Bitplanes** ($D071 BP16ENS):
    - Pairs of bitplanes form 4-bit colour nibbles instead of individual bits
- **MEGA65 Machine Factory**: VIC4 now receives char ROM and colour RAM wiring, fixing C64-mode text rendering
- **MEGA65 Boot Support**: Machine now boots to C64 KERNAL reset vector ($E4B8) and executes MEGA65 KEY unlock sequence, VIC-III banking, and INTERFACE ROM transition
- **Hypervisor Mode** (`src/plugins/45gs02/main/cpu45gs02.cpp`):
    - `enterHypervisor(trapAddr)`: saves full CPU state (A/X/Y/Z/B/SP/P/PC) and MAP state into virtualisation control registers, switches to hypervisor mode with stack at $BE00 and base page at $BF00
    - `exitHypervisor()`: restores saved CPU and MAP state, returns to user mode
    - On reset with HYPPO ROM present, CPU enters hypervisor at $8100 instead of reading the standard reset vector
    - Hypervisor mode maps $8000-$BFFF to HYPPO ROM (reads from ROM, writes ignored)
- **Hypervisor Virtualisation Registers** (`src/plugins/devices/hypervisor/main/hypervisor_regs.cpp`):
    - New `HypervisorRegs` IOHandler at $D640-$D67F (64 bytes)
    - In user mode: writes trigger SYSCALL traps (entry point = $8000 + offset*4)
    - In hypervisor mode: read/write access to saved CPU state (A, X, Z, B, SP, P, PC, MAP state, port $00/$01, VIC mode)
    - $D67F write (ENTEREXIT) exits hypervisor mode
- **HYPPO ROM Loading** (`src/plugins/machines/mega65/main/machine_mega65.cpp`):
    - Searches for 16KB `HICKUP.M65` in `roms/mega65/`, current directory, and `~/.local/share/xemu-lgb/mega65/`
    - Falls back to standard C64 reset vector if not found
- **F018B DMA `getDeviceInfo()`**: Register descriptions (ADDRLSB, ADDRMSB, ADDRBANK, EXECUTE, ADDRMB, ETRIGGER) and runtime state (list address, active flag, enhanced mode, job count)

### Fixed
- **MEGA65 ROM offset mapping**: C64 compatibility regions (BASIC, KERNAL, Char ROM) were loaded from wrong offsets in the 128KB ROM file. Corrected per MEGA65 Book Appendix J (C65 Compatibility ROM Layout): BASIC at file $A000, KERNAL at $E000, Char ROM at $D000.
- **MEGA65 onReset callback**: Machine factory did not set `onReset`, so the CPU was never reset after bus wiring. PC stayed at $0000 instead of reading the reset vector. Added reset callback matching JSON loader pattern.
- **MCP server onReset**: `createMachineInstance()` did not call `onReset` after setup, leaving newly created machines with PC=$0000. Now calls `onReset` after wiring, matching CLI behavior.
- **F018B DMA overlapping copy**: `doCopy()` now detects overlapping source/destination regions and copies backward when destination falls within the source range, preventing data corruption during self-overlapping DMA transfers.

### Changed
- **VIC-IV now extends VIC-III** (`VIC2 → VIC3 → VIC4`): Palette, personality lock, and VIC-III register handling moved from VIC4 into VIC3. VIC4 retains only VIC-IV specific registers ($D048-$D07F), 32KB internal colour RAM, and MEGA65-specific rendering. Net reduction of 133 lines.

### Fixed
- **VIC-IV register address mapping**: Screen RAM pointer was incorrectly mapped to $D04C-$D04F (TEXTXPOS). Fixed to $D060-$D063 (SCRNPTR, 28-bit). Character set pointer fixed from $D050-$D053 to $D068-$D06A (CHARPTR, 24-bit). Colour RAM pointer added at $D064-$D065 (COLPTR).

## [0.2.1] - 2026-05-18

### Added
- **Code Coverage**: `make coverage` target using gcov instrumentation with optional lcov HTML reports.
- **175 New Unit Tests** (309 → 484 total):
    - `test_stack_trace`: Push/pop, recent ordering, clear, type name strings.
    - `test_breakpoint_list`: Exec/read/write watchpoints, enable/disable, conditions, hit counts.
    - `test_source_map`: KickAssembler list parsing, address lookup, missing files, malformed input.
    - `test_tap_parser`: TAP load/save, normal and long pulse decoding, recording roundtrip.
    - `test_mega65_math`: Multiply, divide, divide-by-zero, RNG advancement, reset, out-of-range.
    - `test_datasette`: Mount, play/stop/rewind, recording, signal line wiring, end-of-tape.
    - `test_crtc6545`: Register select/write, read-only registers, counter advancement, reset.
    - `test_sim_config`: Missing config defaults.
    - `test_expression_evaluator`: Condition evaluation, char escapes, error paths (91% → 98%).
    - `test_toolchain`: Named assemblers, getAssemblerNames, resolveAssembler 3-level precedence (53% → 98%).
    - `test_cli`: Registers, memory dump/fill/copy/swap, step, disasm, breakpoints, watchpoints, symbols, search, stack, save, config, inline assemble-execute, no-machine error paths (22% → 61%).
    - `test_cpu45gs02`: Register access (inc. Q), reset, LDA/LDX/LDY/LDZ, stores, transfers, INC/DEC, JMP, JSR/RTS, flags, ADC/SBC, PHA/PLA (10% → 16%).
    - `test_vic2_unit`: Registers, raster counter/IRQ, rendering (all 5 video modes), sprites (Y-expansion), banking, char ROM shadow, device info, logger (48% → 99.7%).
    - `test_vic3`: Registers, palette, locked/unlocked state, 80-col rendering, bitplane rendering, complement flags, DAT ports, ATTR mode, VIC-II backward compat (new, 98.9%).
    - `test_vic4`: Locked/unlocked rendering, tick, register accessors, FCM basic/CHR16/$FF substitution/FCLRLO selection (new + expanded).
    - `test_virtual_iec_unit`: Initial state, readPort, writePort ATN, reset, mount/eject/status, LED activity, device info (14% → 24%).

### Fixed
- **ImportResult ODR violation**: `vice_importer` and `mega65_importer` both defined `struct ImportResult` at global scope with different member layouts (swapped `string`/`vector` fields). This caused a double-free crash when building at `-O0`. Fixed by moving each struct into its respective namespace.

### Changed
- `make clean` now removes coverage artifacts (`.gcno`, `.gcda`, `coverage/`).

## [0.2.0] - 2026-05-14

### Added
- **MEGA65 Phase 21 Completion**:
    - **Machine Factory**: Implemented `Mega65MachineFactory` for the full "mega65" machine preset.
    - **ROM Loading**: Integrated automatic loading of `mega65.rom` (128KB) into physical Banks 2-3 with write protection.
    - **Device Registration**: Wired and registered all core MEGA65 devices: F018B DMA, Math Accelerator, Hyper Serial, and Exit Trap.
    - **Integration Testing**: Added `test_mega65_integration.cpp` verifying 28-bit bus wiring, MMU translation, and I/O personality switching.
    - **MEGA65 Phase 24 (ROM Importer)**:
        - New plugin: `lib/mmemu-plugin-mega65-importer.so`.
        - Automatically discovers MEGA65 ROMs in standard `xemu-xmega65` installation paths across Linux, macOS, and Windows.
        - Added "Manual Selection..." in the GUI to allow users to browse for their `MEGA65.ROM` file.
        - CLI command `importmega65` and MCP tool `import_mega65_roms` for scriptable setup.
        - Namespaced existing `viceImporter` to prevent symbol collisions during static linking in tests.
    - **Build System Refactor**:
 Moved plugin entry points (`mmemuPluginInit`) to separate `plugin_init.cpp` files for Exit Trap, DMA, and Math Unit to prevent linker collisions in test binaries.
- **45GS02 CPU Enhancements**:
    - **Quad immediate modes**: LDQ/ADCQ/SBCQ/CMPQ/ANDQ/ORAQ/EORQ #imm32 now execute (previously halted).
    - **Decimal mode (BCD)**: ADC/SBC now handle FLAG_D correctly via doAdc8/doSbc8 helpers.
    - **Full disassembly**: All opcodes including quad prefix (#imm32 shown as 8-digit hex).
- **28-bit Symbol Table Display**: CLI and GUI adapt address width based on bus (4 digits for 16-bit, 7 for 28-bit) across sym list, memory dump, disasm, and search results.
- **Build System**:
    - Added `make cppcheck` target for static analysis of the codebase.
    - Fixed several One Definition Rule (ODR) violations in test suites by using anonymous namespaces for mock classes.
- **Version string**: Build system generates `version.h` with git hash.

### Changed
- Suppressed "Registering machine:" and plugin load messages to spdlog debug level.
- Refactored all inline 8-bit ADC/SBC to shared doAdc8/doSbc8 helpers (decimal-aware).

### Fixed
- MAP instruction enable bit encoding (was swapped between lower/upper halves).
- EOM incorrectly clearing MAP state.
- GUI plugin pane manager segfault on window destroy in test environment.

## [0.1.2-dev] - 2026-05-09

### Added
- **Pluggable Assembler Infrastructure**:
    - **SimConfig** class loads tool configuration from `config.json` (with search in standard paths)
    - **ToolchainRegistry** extended with name-based assembler registration
    - **resolveAssembler()** function with 3-level precedence: runtime override → machine-preferred → ISA-default
    - **MachineDescriptor** now includes `preferredAssembler` field (loaded from machine JSON)
- **CA45 Assembler Backend**:
    - Integrated CA45 assembler for 45GS02/MEGA65 as pluggable backend
    - Auto-configured via SimConfig tool settings
    - Registered in 45GS02 plugin
- **MCP Server Assembler Tools**:
    - `set_assembler` — Override assembler for a specific machine at runtime
    - `get_assembler` — Query current assembler for a machine
    - Enhanced `asm` tool with fallback from line-by-line to file-based assembly modes
- **MCP Multi-Instance Machine Management**:
    - `list_instances` — List all running machine instances with their type and display name
    - `destroy_machine` — Close and release a machine instance by instance ID
- **Previous MCP Enhancements** (from earlier 0.8.1 work):
    - Code generation tools with error diagnostics
    - Search navigation tools for memory pattern navigation
    - MEGA65-specific address translation and I/O personality control
    - Trace buffer integration

### Changed
- **Multi-Instance Machine Support**: `create_machine` refactored for independent instances
  - `machine_id` parameter renamed to `machine_type` (the registry type: "c64", "rawMega65", etc.)
  - `machine_id` now an optional instance name; auto-generated as `<type>_<n>` if omitted
  - Enables multiple independent instances of the same machine type (e.g., "c64_1", "c64_2")
- **Assembler Handling**: Moved from ISA-only default to configurable per-machine selection
- **MCP Tool Count**: 40 → 53 → 55 tools (multi-instance support adds 2 new tools)

### Technical Details
- **SimConfig**: Searches `./config.json` then standard data paths for tool configuration
- **ToolchainRegistry**: Now supports both ISA-based and name-based assembler factories
- **MachineState (MCP)**: Added `IAssembler* assem` field; global `g_assemblerOverrides` map for per-machine runtime selection

## [0.1.1-dev] - 2026-05-07

### Added
- **Phase 19: Sparse Memory and Memory Management Unit (MEGA65)**:
    - **SparseMemoryBus**: 28-bit address space with 4KB lazy-allocated pages for efficient MEGA65 memory management.
    - Pre-mapped ROM regions for KERNAL, BASIC, and Character ROM with configurable overlays.
    - Snapshot serialization support for sparse memory format.
    - **MapMmu**: Pure address translator implementing MEGA65 memory mapping (MAP instruction support).
    - Full support for memory region translation with snapshot persistence.
    - Unit tests validating both `SparseMemoryBus` and `MapMmu` functionality.
- **Phase 21: MEGA65 Machine Factory (In Progress)**:
    - Wire `MapMmu` as CPU's bus pointer for transparent address translation.
    - Integration with 45GS02 CPU for MAP instruction support.
    - Foundation for ROM loading and I/O personality switching.

### Changed
- **Memory Architecture**: Moved from flat 16-bit memory model to sparse 28-bit addressing for MEGA65 support.

## [0.7.0-dev] - 2026-04-25

### Added
- **Installation Infrastructure**:
    - New `install-local.sh` and `install-system.sh` scripts in `tools/` for easy deployment to `~/.local` or `/usr/local`.
    - **Man Page Generation**: Added a `make man` target using `pandoc` to convert Markdown documentation into system man pages (installed to Section 1 for binaries and Section 7 for plugins/concepts).
- **Resource Discovery System (`PathUtil`)**:
    - Centralized path resolution for plugins, machine configurations, and ROM images.
    - Automatic search in standard locations: `./`, `~/.local/share/mmsim/`, and `/usr/local/share/mmsim/`.
    - Plugins are now automatically discovered in `~/.local/lib/mmsim/plugins` and `/usr/local/lib/mmsim/plugins`.
- **45GS02 CPU Implementation** (Phase 18):
    - Initial implementation of the MEGA65 45GS02 CPU core in `src/plugins/45gs02/`.
    - Support for 65CE02 extensions: `TAZ`, `TZA`, `TAB`, `TBA`, `PHZ`, `PLZ`, `INW`, `DEW`, `ASW`, `ROW`, `PHW`, `BSR`, `RTN`.
    - Support for MEGA65 specifics: `NEG`, `MAP`, `EOM`.
    - Implemented **Quad (Q) pseudo-register** (A+X+Y+Z) and associated 32-bit operations:
        - `LDQ`, `STQ` (Zero Page, Absolute, and 32-bit Indirect addressing).
        - `ADCQ`, `SBCQ`, `CMPQ`, `ANDQ`, `ORQ`, `EORQ`.
        - `ASLQ`, `LSRQ`, `ROLQ`, `RORQ`.
    - Enhanced disassembler with support for all Quad instructions.
    - Integrated `translate()` logic for memory mapping (4-slot MMU).
    - Support for the `B` (Base Page) register in zero-page addressing modes.
    - Hypervisor exit trap support (triggered at `$D6CF`).
- **45GS02 Validation Suite**:
    - Automated cross-validation of the `mmsim` 45GS02 core against the reference `xemu-xmega65` emulator.
    - Test cases for arithmetic, transfers, and advanced CPU features.
    - **Note**: Running these tests may trigger a GUI dialog from `xemu-xmega65` even in headless mode on some systems.
- **IEC Bus Enhancements**:
    - Added comprehensive tests for IEC serial bus communication and `.d64` disk image handling.
- **`rawMega65` Machine Preset**:
    - A minimal machine target for testing the 45GS02 CPU without full MEGA65 chipset complexity.

### Changed
- **Pathing**: Updated `xmega65` path configuration in validation scripts to be less user-specific, improving portability.

### Fixed
- **CLI Cleanup**: Minor code cleanups in `cli_interpreter.cpp` to use modern C++17 `if` with initializer and removed redundant code.

## [0.6.0-dev] - 2026-04-11

### Added
- **Engineering Calculator** (GUI):
    - Pop-up modal calculator accessible via **Tools > Calculator** (**Ctrl-Shift-C**).
    - **28-Button Dynamic Layout**: Key labels and functions adapt automatically based on the selected numerical base.
    - Supports **Floating Point** and **Integer** modes.
    - Supports **Hex**, **Decimal**, **Binary**, and **Octal** bases with automatic formatting.
    - Features **8 persistent memory slots** (`Rcl0`-`Rcl7`) and an `Ans` button for result recall.
    - **Direct Keyboard Entry**: The display field is editable, allowing manual typing of complex expressions.
    - Comprehensive function support: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `sqrt`, `log`, `exp`, `abs`, `^`, `<<`, `>>`, and bitwise complements.
- **Enhanced Expression Evaluator**:
    - Refactored `ExpressionEvaluator` to use `double` precision for all internal math.
    - Added support for bitwise shift operators (`<<`, `>>`).
    - Added support for power operator (`^`).
    - Integrated library of math functions: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `sqrt`, `log` (log10), `exp`, `abs`, and `between(n, val, delta)`.
    - Added `defaultBase` support to allow parsing literals without prefixes (e.g., `ffd2` in hex mode).

### Fixed
- **Expression Parsing**: Resolved issue where hex literals like `ffd2` were rejected in Hex mode because they were interpreted as invalid decimals. The evaluator now respects the context-provided default base.
- **UI Layout**: Fixed status line overflow in the calculator dialog by expanding the minimum width and optimizing sizer alignment.

## [0.5.0-dev] - 2026-04-09

### Added
- **Virtual IEC Bus Device** (`src/plugins/devices/virtual_iec/`):
    - `VirtualIECBus` class implementing Level 2 (protocol-level) IEC serial bus emulation.
    - State machine: `IDLE → ATTENTION → ADDRESSING → ACKNOWLEDGE` with correct ATN/CLK/DATA open-collector semantics.
    - Implements `IPortDevice` for direct attachment to CIA #2 Port A (C64) or VIA #2 (VIC-20).
    - `IOHandler` interface satisfied: `tick()`, `reset()`, `mountDisk()`, `ejectDisk()`.
    - Plugin entry point (`plugin_init.cpp`) and `.so` build target (`mmemu-plugin-virtual-iec.so`).
    - JSON machine-loader wiring via new `iecWiring` section in machine specs.
    - Unit test `virtual_iec_handshake` verifying ATN detection and DATA acknowledge within a single tick.
- **`IOHandler` base class**: Added default `mountDisk(int, const std::string&)` and `ejectDisk(int)` virtual methods so plugins exposing disk management don't need to inherit a separate interface.
- **Build system**: `virtual_iec.o` added to `ALL_PLUGIN_OBJS`; `test_virtual_iec.cpp` added to `TEST_SRCS`; test count grows to 162.

### Fixed
- **`VirtualIECBus::tick()` timer reset bug**: Transitioning IDLE→ATTENTION previously reset `m_stateTimer` *after* the tick's cycles had been accumulated, making it impossible for the ATTENTION threshold to be satisfied in the same tick as ATN detection. Fixed by preserving the accumulated cycle count across the transition and processing up to two state changes per tick.
- **Stale-artifact vtable crash**: Adding virtual methods to `IOHandler` without a full rebuild caused a vtable layout mismatch between the statically-linked test binary and the dynamically-loaded `.so` plugins, crashing `c64_tape_load` with `basic_string: construction from null`. Resolved by ensuring all artifacts are rebuilt consistently.

## [0.4.0-dev] - 2026-04-08

### Added
- **Advanced Expression Evaluator**:
    - Support for high byte (`<`) and low byte (`>`) unary operators.
    - Support for logical negation (`!`).
    - Support for multiplication (`*`), division (`/`), and modulus (`%`).
    - Support for comparison operators (`==`, `!=`, `<`, `>`, `<=`, `>=`).
    - Support for parentheses `( )` to control evaluation order and precedence.
    - Support for character literals (e.g., `'a'`, `'\n'`) returning ASCII values.
    - Disambiguation between modulus `%` and binary literal prefix.
- **Enhanced Debugger GUI**:
    - **DisasmPane Context Menu**: Added a comprehensive right-click menu for breakpoint management, navigation (Go to Address/PC, Follow Operand, Set PC, Go up stack frame), and clipboard operations (Copy Address/Disassembly).
    - **Conditional Breakpoints**: Integrated condition support into the Breakpoint Pane, allowing users to set and edit expressions that must be non-zero to trigger a break.
    - **Breakpoint Management**: Added right-click menu to Breakpoint Pane for toggling, editing conditions, and clearing hit counts.
    - **Memory Sync**: "Show address in memory pane" feature to instantly jump the primary memory view to a specific address from the disassembler.
- **Improved Symbol Support**:
    - Enhanced address-entry dialogs and `Ctrl-G` to resolve machine symbols and complex expressions (e.g., `CHROUT`, `start + 5`).

### Changed
- **UI Refactoring**: Replaced magic numbers in GUI event handlers with structured enums for better maintainability.
- **Expression Parsing**: Enforced strict rules for literal prefixes (`$`, `%`) to prevent ambiguity with whitespace.

## [0.3.0-dev] - 2026-04-07

### Added
- **Enhanced Symbol Table Management**:
    - CLI: Added `sym` command with `add`, `del`, `list`, `search`, `load`, and `clear` subcommands.
    - GUI: Integrated a new **Symbols** pane with real-time search, manual management, and double-click navigation.
    - MCP: Added `list_symbols`, `add_symbol`, `remove_symbol`, `load_symbols`, and `clear_symbols` tools.
    - Support for multiple `.sym` file formats (`label = value` or `label value`).
- **Core Expression Evaluator**:
    - Implemented a robust recursive-descent parser for addresses and values.
    - Supported formats: Hex (`$prefix` or `0x`), Binary (`%prefix`), and Decimal.
    - Integrated support for using symbols and CPU registers (e.g., `PC + 5`, `start_addr`) in expressions.
    - Integrated the evaluator into all address-taking CLI commands, GUI dialogs, and MCP tools.
- **Kernal Symbols & Monitoring**:
    - Added standard KERNAL symbol tables for C64, VIC-20, and PET machines.
    - Implemented auto-loading of these symbols upon machine creation via the new `symbolFiles` machine configuration field.
    - Added **Kernal Routine Monitoring**: Setting `log level kernal debug` now logs register states upon entry to Kernal routines and displays result states upon matching `RTS` exit.
- **Testing Expansion**:
    - Added 5 new test suites (totaling 153 passing tests) covering Expression Evaluation, Symbol Management, Kernal Autoloading, and C64 I/O logic.

### Fixed
- **Build System**: Fixed several Makefile syntax errors and redundant variable definitions.
- **Datasette/Tape Pane**: Fixed "No Datasette found" error by ensuring `Datasette` and other plugins correctly implement `setName()` and `name()` to match machine configurations.
- **C64 Logic**: Repaired severely broken bit-manipulation logic in the 6510 I/O port (`cpu6510.cpp`).
- **Stability**: Added null checks and move semantics to MCP machine management to prevent crashes and double-free errors.

## [0.2.0-dev] - 2026-04-07 (2)

### Added
- **CLI/MCP parity with GUI memory operations**:
    - CLI: Added `swap <addr1> <addr2> <len>` command to swap two equal-length memory regions.
    - CLI: Added `findnext` / `findprior` commands to navigate forward and backward through occurrences of the last `search`/`searcha` pattern (wraps around the full address space).
    - CLI: `search` and `searcha` now report **all** matches, save state for `findnext`/`findprior`, and use the bus address mask instead of the hardcoded `0x10000` range.
    - MCP: Added `swap_memory(machine_id, addr1, addr2, size)` tool, matching the GUI Swap Memory operation.

---

## [0.2.0-dev] - 2026-04-07

### Added
- **MemoryPane Enhancements**:
    - Added a context menu to `MemoryPane` for quick access to Fill, Copy, Swap, Go to Address, and Search Memory.
    - Implemented **Swap Memory** functionality (`ID_SWAP_MEM`) with a new `SwapMemoryDialog`.
    - Integrated Fill, Copy, and Swap operations into `MmemuFrame` and `MemoryPane`.
    - Added error handling (try-catch) to `CopyMemoryDialog` and `SwapMemoryDialog` for robust hex input validation.
    - Implemented **in-place editing** of memory cells by clicking on a hex value. Pressing **Return** commits the change, while **Escape** or losing focus cancels it.

## [0.2.0-dev] - 2026-04-05

### Added
- **C64 Video Color Update Test** (`src/plugins/devices/vic2/test/test_c64_video.cpp`): Added an integration test to verify that the C64 VIC-II chip correctly updates screen and border colors after simulating 5 million cycles, ensuring memory management and color rendering are accurate.
- **C64 Boot Screen Content Test** (`src/plugins/devices/vic2/test/test_c64_boot_screen.cpp`): Added a test to verify that the C64 boots correctly and displays non-space characters on the screen after initialization, indicating successful KERNAL ROM execution and screen memory updates.
- **`ExpressionEvaluator`** (`src/libdebug/main/expression_evaluator.h/cpp`): Stub condition evaluator used by `BreakpointList::checkExec/checkWrite/checkRead`; empty condition always matches.
- **CLI breakpoint/watchpoint commands** (`break`, `watch`, `delete`, `enable`, `disable`, `info breaks`) wired to `DebugContext::breakpoints()`.
- **`tests/test_breakpoints.cpp`**: Integration test exercising `create vic20` → assemble JMP loop → `break 0400` → `run 0400` → assert paused at correct PC.

### Changed
- **Test File Location Refactoring**: Moved various tests from the top-level `tests/` directory to their corresponding module-specific `src/plugins/test/<module>/` directories for better organization. Temporarily excluded problematic Atari-specific machine integration tests from the main test executable due to missing machine definitions.

### Fixed
- **Build System**:
    - Repaired corrupted `src/include/util/stb_image_write.h` which had invalid multi-line string literals and macros.
    - Fixed missing initializers for `SimPluginManifest` in multiple plugin initialization files to eliminate compiler warnings.
- `cli_interpreter.cpp`: Removed duplicate `info` and `break` else-if blocks that were unreachable dead code after the `eject` handler.
- `tests/test_plugin_extension.cpp`: Removed duplicate `int refresh;` field in local `TestCtx` struct (caused compile error).
- `tests/test_breakpoints.cpp`: Assembly line used label syntax (`loop: JMP loop`) unsupported by the mini-assembler; replaced with `JMP $0400`.

---

## [0.2.0-dev] - 2026-03-31

### Added
- **Atari 8-bit Video implementation (Phase 26)**:
    - **ANTIC DMA Engine**: Implemented display list interpretation, cycle-accurate timing (114 cycles/line, 262 lines/frame), LMS instruction, WSYNC, and NMI generation (VBI/DLI).
    - **GTIA Color and PMG**: Implemented color palette (256 colors), player-missile graphics positioning, collision detection, and console switches.
    - **POKEY Audio and IO**: Implemented 4-channel audio synthesis, polynomial-counter noise generators, frequency division, joined channels, 3 interval timers with IRQs, and skeletons for keyboard scanning and SIO.
    - Added dedicated documentation for each chip in `doc/README-ANTIC.md`, `doc/README-GTIA.md`, and `doc/README-POKEY.md`.

## [0.2.0-dev] - 2026-03-30

### Changed
- **Plugin ABI Stability (Phase 0.5)**:
    - Refactored `SimPluginHostAPI` to use stable C function pointers (`createCore`, `createDevice`, etc.) instead of passing C++ class pointers.
    - Updated `PluginLoader` to provide host-side wrappers for registry operations.
    - Updated all plugins to use the new host API, removing brittle registry singleton synchronization.
- **Resource Management (Phase 0.5)**:
    - Implemented RAII-based cleanup in `MachineDescriptor` via a new destructor that handles cpus, buses, I/O registry, and ROM buffers.
    - Updated `IORegistry` to support owned handlers, ensuring automatic deletion of machine-specific devices.
    - Added a `deleters` mechanism to `MachineDescriptor` for cleaning up generic machine-resident objects.
    - Fixed memory leaks in CLI, GUI, and integration tests by properly deleting machine instances on switch or shutdown.
- **Undefined Behavior (Phase 0.5)**:
    - Fixed potential undefined behavior in `FlatMemoryBus` address mask calculation for 32-bit address spaces.
- **Interface Segregation (Phase 0.5)**:
    - Refactored `ICore` into `ICpuRegs` (register access) and `ICpuDisasm` (disassembly) interfaces.
    - Refactored `IBus` to be leaner, segregating `ISnapshotable` (state management) and `IBusWriteLog` (write logging) interfaces.
    - Updated all CPU cores (`MOS6502`, `MOS6510`) and memory buses (`FlatMemoryBus`, `PortBus`) to implement the new segregated interfaces.
- **Encapsulation & Security**:
    - Moved `ExecutionObserver* observer` to protected members in `ICore` and `IBus`.
    - Added `setObserver()` and `getObserver()` accessors for safer observer management.
    - Made `IBus::peek8()` pure virtual to guarantee side-effect-free memory reads in all implementations.

### Fixed
- **Build System**: 
    - Fixed `make test` target in `Makefile` which was previously broken (Permission denied).
    - Configured the test binary to correctly link against plugin object files and registry implementations.
    - Fixed naming mismatch for the `vice-importer` plugin in the build system.
- **Hardware Emulation**:
    - Fixed incorrect wired-AND logic in `PIA6520` read-back of Peripheral Registers A and B. It now correctly returns the combination of latch values for output bits and pin values for input bits.
- **Testing**:
    - Fixed missing device registrations in VIC-20 integration tests.
    - Added missing include paths to the test build.

## [0.2.0-dev] - 2026-03-29

### Added
- **Deep Code Review (Critique)**:
    - Performed a comprehensive architectural and implementation review of the Phase 0/1 codebase.
    - Documented critical findings in `critique.md`, including ABI stability risks, memory leaks, encapsulation breaks, and performance bottlenecks.
    - Identified "fat" interfaces and bug-prone `peek8` implementations in `ICore` and `IBus`.
    - Recommended corrective actions for resource ownership, plugin API stability, and interface segregation.

## [0.2.0-dev] - 2026-03-28

### Added
- **Core Loader Infrastructure (Phase 13.1)**:
    - Defined `IImageLoader` and `ICartridgeHandler` interfaces in `src/libcore/main/image_loader.h`.
    - Implemented `ImageLoaderRegistry` singleton for managing loaders and cartridge factories.
    - Implemented `BinLoader` for raw binary images (.bin) at user-specified addresses.
    - Extended `SimPluginHostAPI` and `SimPluginManifest` to support plugin-provided loaders and cartridges.
    - Updated `PluginLoader` to handle automatic registration of plugin image loaders.
- **CBM Loader Plugin (Phase 13.2)**:
    - Implemented `PrgLoader` for Commodore `.prg` files with automatic load address detection.
    - Implemented `CrtParser` for the OpenC64Cart `.crt` format, supporting standard 64-byte headers and CHIP packets.
    - Implemented `CbmCartridgeHandler` for mapping standard 8 KB and 16 KB C64 cartridges into the bus.
    - Registered loaders into the host via `lib/mmemu-plugin-cbm-loader.so`.
- **Snapshot Integration for Cartridges**:
    - Integrated active cartridge tracking into the system snapshot system.
    - Extended `SystemSnapshot` to include the `cartridgePath`.
    - `DebugContext` now automatically reattaches the correct cartridge image upon snapshot restoration.
- **Bus Overlay API Enhancements**:
    - Added `addRomOverlay` and `removeRomOverlay` to the `IBus` interface.
    - Implemented these methods in `FlatMemoryBus` to support dynamic cartridge attachment and ejection.
- **Commodore PET Video (Phases 12.2, 12.3)**:
    - Implemented MOS 6545 / 6845 CRTC emulation with full timing counters and address generation.
    - Implemented PET Video Subsystem supporting PET 2001, 4000, and 8000 series.
    - Added support for character ROM glyph rendering, inverse video, and phosphor simulation.
- **Commodore PET Series (Phases 12.4, 12.5)**:
    - Implemented `IEEE488Bus` interface and `SimpleIEEE488Bus` implementation for PET peripherals.
    - Added `MachineDescriptor` factories for PET 2001, 4032, and 8032 models.
    - Implemented PET keyboard matrix (Graphics and Business layouts) wired to PIA/VIA.
    - Configured model-specific memory maps with RAM and ROM overlays.
- **Tape (Datasette) Support (Phase 14)**:
    - Added a roadmap for .tap pulse-encoded archive support across VIC-20, C64, and PET.
    - Planned core timing-exact decoder and machine-specific signal wiring.
- **IEC Serial Bus and Disk Drive Support (Phase 15)**:
    - Added a roadmap for IEC serial bus emulation and disk image support (.d64, .g64).
    - Planned High-Level Emulation (HLE) via KERNAL trapping and Level 2 virtual bus simulation.
- **Runtime Image and Cartridge Loading (Phase 13)**:
    - Added a roadmap for dynamic loading of `.prg`, `.bin`, and `.crt` images.
    - Planned CLI, GUI, and MCP interfaces for runtime memory injection and cartridge attachment.
- **Commodore 128 Implementation Plan (Phase 16)**:
    - Inserted a comprehensive roadmap for the C128, including the 8502 and Z80 CPUs, the 8722 MMU, and the 8563 VDC.
- **Commodore PET/CBM Implementation Plan (Phase 12)**:
    - Inserted a roadmap for the PET series, including the 6520 PIA, 6545 CRTC, and PET-specific video and memory configurations.
- **MOS 6510 CPU (Phase 11.1)**:
    - Implemented `MOS6510 : public MOS6502` in `src/plugins/6502/main/cpu6510.h/cpp`.
    - Built-in 6-bit I/O port at $0000 (DDR) and $0001 (DATA) using a `PortBus` proxy, since `MOS6502::read/write` are private non-virtual.
    - Power-on defaults: DDR=$00, DATA=$3F — all lines pulled high (KERNAL+BASIC+I/O visible immediately after reset).
    - Three `ISignalLine*` outputs (`signalLoram()`, `signalHiram()`, `signalCharen()`) for the C64 PLA.
    - Registered as core `"6510"` and machine preset `"raw6510"` in `mmemu-plugin-6502.so`.
- **C64 PLA Banking Controller (Phase 11.2)**:
    - Implemented `C64PLA : public IOHandler` in `src/plugins/devices/c64_pla/`.
    - Monitors LORAM/HIRAM/CHAREN signal lines to route BASIC ROM ($A000–$BFFF), KERNAL ROM ($E000–$FFFF), Character ROM or I/O ($D000–$DFFF) on every read.
    - Registered with `baseAddr()=0xA000` so IORegistry sort order places it before $D000 device handlers.
    - Returns `false` for I/O-visible state, allowing VIC-II/SID/CIA handlers to respond.
    - Plugin: `lib/mmemu-plugin-c64-pla.so`; registration name `"c64pla"`.
- **MOS 6526 CIA (Phase 11.3)**:
    - Implemented `CIA6526 : public IOHandler` in `src/plugins/devices/cia6526/`.
    - 16-register interface (`addrMask=0x000F`); configurable base address for CIA #1 ($DC00) and CIA #2 ($DD00).
    - Timer A and Timer B: 16-bit countdown with latches, continuous/one-shot modes; Timer B can count Timer A underflows.
    - TOD BCD clock with freeze/latch behavior on TODHR read and alarm interrupt.
    - ICR set/clear/read-to-clear semantics; IRQ propagated via `ISignalLine`.
    - Peripheral injection via `IPortDevice` for keyboard matrices and joysticks.
    - Plugin: `lib/mmemu-plugin-cia6526.so`; registration name `"6526"`.
- **MOS 6567/6569 VIC-II (Phase 11.4)**:
    - Implemented `VIC2 : public IOHandler, public IVideoOutput` in `src/plugins/devices/vic2/`.
    - Five video modes: Standard/Multicolor Text, Standard/Multicolor Bitmap, Extended Background Color.
    - 8-sprite engine with X/Y expansion, priority, mono/multicolor, hardware sprite-sprite and sprite-BG collision.
    - 384×272 total frame (RGBA8888) with 320×200 active display area and 32/36px borders.
    - Raster IRQ via `ISignalLine`; write-1-to-clear $D019 semantics.
    - VIC-II banking via `setBankBase()`; character ROM always visible at bank offsets $1000 and $9000.
    - DMA reads through dedicated `IBus*`; color RAM read via `m_dmaBus->peek8(0xD800+cellIdx)`.
    - Plugin: `lib/mmemu-plugin-vic2.so`; registration name `"6567"`.
- **MOS 6581 SID (Phase 11.5)**:
    - Implemented `SID6581 : public IOHandler, public IAudioOutput` in `src/plugins/devices/sid6581/`.
    - Three voices: 24-bit phase accumulator, Triangle/Sawtooth/Pulse/Noise waveforms (ANDed when combined).
    - 23-bit LFSR noise (taps 22,17) clocked on phase bit-19 rising edge.
    - ADSR envelope generator with 16 attack and 16 decay/release rate levels.
    - Hard sync (reset by previous voice overflow) and ring modulation (triangle XOR'd with previous voice MSB).
    - Chamberlin state-variable filter: LP/BP/HP individually selectable per $D418 mode bits.
    - `IAudioOutput` pull model: `tick()` fills 8192-sample ring buffer; `pullSamples()` drains it.
    - Plugin: `lib/mmemu-plugin-sid6581.so`; registration name `"6581"`.
- **Documentation (Phase 11.1–11.5)**:
    - Created `doc/README-6510.md` — MOS 6510 I/O port and PortBus proxy.
    - Created `doc/README-C64PLA.md` — C64 PLA banking matrix and signal wiring.
    - Created `doc/README-6526.md` — CIA 6526 register map, timer, TOD, and ICR details.
    - Created `doc/README-VIC2.md` — VIC-II video modes, sprite engine, banking, and register map.
    - Created `doc/README-SID.md` — SID voice architecture, ADSR, filter, and audio output.
    - Updated `doc/README-6502.md` — added MOS 6510 variant section.
    - Updated `doc/README-PLUGINS.md` — added all Phase 11 plugins; updated distribution strategy.
    - Updated `README.md` — expanded quick links and roadmap.

## [0.1.0-dev] - 2026-03-23

### Added
- **Machine Plugin Refactor & Build Cleanup:**
    - Moved the VIC-20 machine descriptor into a dedicated dynamic plugin (`lib/mmemu-plugin-vic20.so`).
    - Standardized the `lib/` directory to contain only dynamic plugins intended for distribution.
    - Relocated internal static archives (`.a` files) to `lib/internal/` to keep them hidden from end-users.
    - Updated the `Makefile` to handle complex cross-plugin dependencies while maintaining test suite stability.
- **VIC-I Video Chip & Standard Video Interface (Phase 10.2):**
    - Defined `IVideoOutput` standard interface for all video-generating plugins.
    - Implemented MOS 6560 (VIC-I) rendering logic, including character fetch, multi-color support, and palette.
    - Integrated VIC-I as a modular device plugin (`lib/mmemu-plugin-vic6560.so`).
    - Wired VIC-I and Color RAM into the VIC-20 machine descriptor.
- **Project Structure Refactoring:**
    - Reorganized all sub-modules into `main` and `test` subdirectories (e.g., `src/libdebug/main` and `src/libdebug/test`).
    - Moved module-specific unit tests from `tests/` to their respective module `test` directories.
    - Updated `Makefile` and all C++ include directives to reflect the new structure.
    - Reserved the top-level `tests/` directory for cross-cutting integration scenarios.
- **Device Infrastructure (Phase 9):**
    - Implemented `IOHandler` abstract base class for memory-mapped devices.
    - Implemented `IORegistry` for centralized device management and I/O dispatching.
    - Added `IPortDevice` interface for parallel I/O peripherals (VIA/CIA support).
    - Added `ISignalLine` interface for hardware signals (IRQ/NMI/Reset).
    - Integrated `IORegistry` into the build system and added unit tests in `tests/test_devices.cpp`.
- **Plugin Extension Host API (Phase 10.5):**
    - Extended `SimPluginHostAPI` with dynamic registration for GUI panes, CLI commands, and MCP tools.
    - Implemented `PluginPaneManager` (GUI) with `wxAuiNotebook` integration for plugin-provided tabs.
    - Implemented `PluginCommandRegistry` (CLI) for dynamically adding commands to the REPL.
    - Implemented `PluginToolRegistry` (MCP) for exposing plugin methods as JSON-RPC tools.
    - Updated `PluginLoader` to automatically wire host registration stubs during plugin initialization.
    - Verified cross-module callback logic with comprehensive unit tests.
- **VIC-20 Machine Integration (Phase 10.3 & 10.4):**
    - Implemented full VIC-20 memory map with support for Character, BASIC, and KERNAL ROM overlays.
    - Integrated `romLoad` utility for validated ROM loading into machine memory.
    - Implemented a standard 5-bit active-low joystick device as an `IPortDevice`.
    - Wired the Joystick to VIA #1 Port B in the VIC-20 machine descriptor.
    - Added `onJoystick` hook to `MachineDescriptor` for external input injection.
    - Updated `MachineDescriptor` to manage ROM memory lifetimes.
- **GUI Advanced Debugger Features (Phase 8.5):**
    - Implemented `ConsolePane` providing an integrated CLI environment within the GUI.
    - Added `FillMemoryDialog` and `CopyMemoryDialog` for easier memory manipulation.
    - Added `AssembleDialog` for on-the-fly instruction assembly and RAM injection.
    - Added `GotoAddressDialog` with optional PC update and "Go to" toolbar button.
    - Added `SearchMemoryDialog` for Hex/ASCII pattern searching in memory.
    - Added CLI `setpc`, `search`, and `searcha` commands for advanced debugging.
    - Added MCP `set_pc` and `search_memory` tools for AI-driven debugging.
    - Refactored GUI layout to include a vertical split for the console and notebook support.
- **GUI Target Implementation (Phase 8):**
    - Implemented a modular graphical user interface using wxWidgets (`src/gui/main.cpp`).
    - Added `MachineSelectorDialog` for dynamic machine preset selection.
    - Implemented `RegisterPane` with automatic change highlighting and support for multiple CPU slots.
    - Implemented `MemoryPane` providing a high-performance hex/ASCII dump with scroll support and side-effect-free reads.
    - Implemented `DisasmPane` with scrollable disassembly view centered on the current program counter.
    - Integrated all core libraries (mem, core, toolchain, debug, plugins) into the GUI frontend.
    - Established GUI standards: fixed-width fonts, asynchronous refresh (30 Hz), and DPI-aware layout using splitters.
- **MCP Target Implementation (Phase 7):**
    - Implemented MCP (Model Context Protocol) server over JSON-RPC 2.0 (`src/mcp/main.cpp`).
    - Created a lightweight, dependency-free JSON library (`src/mcp/minijson.h`).
    - Registered essential tools for AI interaction: `step_cpu`, `read_memory`, `write_memory`, and `read_registers`.
    - Exposed `machine_state` as an MCP resource.
    - Added a Python-based validation harness (`tests/mcp_test.py`) for automated MCP interface testing.
    - Updated `PluginLoader` to use `std::cerr` for logging to keep `stdout` clean for JSON-RPC.
    - Fixed a race condition in the `Makefile` when running `make -j clean all`.
- **libmem — Address Bus Abstraction (Phase 1):**
    - Implemented `IBus` abstract interface for memory and I/O access.
    - Implemented `FlatMemoryBus` with support for heap-allocated storage, configurable address masking, and ROM overlays.
    - Added high-performance write-logging to `FlatMemoryBus` for debugger synchronization.
    - Added state snapshot/restore support to memory buses.
    - Created `tests/test_flatmembus.cpp` with comprehensive coverage for basic R/W, masking, and overlays.
- **libcore — ICore and MOS 6502 (Phase 2):**
    - Implemented `ICore` abstract CPU interface, featuring register descriptors and convenience accessors.
    - Implemented a fully functional `MOS6502` CPU core supporting all official NMOS opcodes and common undocumented instructions.
    - Implemented all 6502 addressing modes including the infamous indirect JMP bug for cycle-accurate behavior.
    - Added `MachineDescriptor`, `CpuSlot`, and `BusSlot` structures for data-driven machine composition.
    - Implemented `MachineRegistry` for dynamic registration and creation of machine presets.
    - Created `tests/test_cpu6502.cpp` to verify instruction accuracy, status flags, and state snapshots.
- **libtoolchain — Disassembler and Assembler (Phase 3):**
    - Implemented `IDisassembler` interface and `DisasmEntry` for detailed instruction metadata.
    - Implemented `Disassembler6502` with support for symbol resolution and control-flow detection.
    - Implemented `IAssembler` interface with support for configurable backend metadata.
    - Implemented `KickAssemblerBackend` for integration with external Java-based tools.
    - Implemented `Assembler6502` native mini-assembler for single-line instruction support.
    - Implemented `SymbolTable` and `SourceMap` for mapping between machine code and source labels/locations.
    - Added `BaseDisassembler` for default delegation to CPU core disassembly.
    - Created `tests/test_disasm6502.cpp` to verify disassembly and symbol resolution.
- **libdebug — Debug Infrastructure (Phase 4):**
    - Implemented `ExecutionObserver` interface for monitoring CPU execution and memory events.
    - Implemented `BreakpointList` supporting EXEC, READ_WATCH, and WRITE_WATCH breakpoints.
    - Implemented `TraceBuffer` for cycle-accurate execution history.
    - Implemented `DebugContext` for managing debug sessions, breakpoints, and system-wide snapshots.
    - Added support for diffing memory states between snapshots.
    - Created `tests/test_debug.cpp` to verify breakpoint logic, tracing, and snapshot integrity.
- **Plugin Architecture & Refactor (Phase 6):**
    - Finalized the `mmemu_plugin_api.h` C-ABI contract for runtime plugins.
    - Implemented `PluginLoader` using `dlopen`/`dlsym` for dynamic loading of `.so` modules.
    - Implemented `CoreRegistry` and `ToolchainRegistry` for centralized management of CPU cores and machine-specific tools.
    - Decoupled the 6502 implementation from the host libraries, moving it into a dedicated `mmemu-plugin-6502.so`.
    - Updated the CLI to automatically discover and load plugins from the `lib/` directory.
- **CLI Target Implementation (Phase 5):**
    - Implemented interactive REPL with command history and parsing.
    - Added support for dynamic machine creation and selection.
    - Implemented core debugging commands: `step`, `regs`, `m` (memory dump), `f` (memory fill), `disasm`.
    - Added direct CPU instruction execution using the `.` prefix (e.g., `.lda #$42`).
- **Utility & Performance:**
    - Implemented a generic `CircularBuffer<T>` template for high-performance fixed-capacity logging.
    - Refactored `FlatMemoryBus` write-log and `TraceBuffer` execution log to use `CircularBuffer`, eliminating $O(N)$ removal overhead.
- **Project Infrastructure:**
    - Unified the test suite into a single `mmemu-tests` binary using `tests/test_main.cpp`.
    - Added `tests/test_harness.h` for simple, lightweight unit testing.
- **Project Foundation (Phase 0):**
    - Created directory skeleton for core libraries: `src/libmem/`, `src/libcore/`, `src/libdevices/`, `src/libtoolchain/`, and `src/libdebug/`.
    - Created `roms/` and `tests/` directories.
    - Added `.gitkeep` and `.gitignore` files to ensure proper tracking of empty directories and ignored ROM files.
- **Library Build System:**
    - Extensively updated `Makefile` to support modular builds.
    - Added targets for building static archives (`make libs`) and running tests (`make test`).
    - Configured binaries (`mmemu-cli`, `mmemu-mcp`, `mmemu-gui`) to link against internal libraries.
- **ROM Loader Utility:**
    - Implemented `romLoad` in `src/libcore/rom_loader.h/cpp` for validated binary loading.
- **Documentation:**
    - Created `GEMINI.md` for project overview and quick-start instructions.
    - Created `STYLEGUIDE.md` to establish project-wide coding conventions.

### Changed
- **Directory Reorganization:**
    - Moved global `include/` directory to `src/include/` to centralize all source-related files.
    - Updated `Makefile` and `GEMINI.md` to reflect the new header search paths.
- **Style Alignment:**
    - Updated all architectural specifications (`.plan/arch.md`) and roadmaps (`.plan/todo.md`) to use `camelCase` for variable and function names as per the new style guide.
    - Refactored placeholder headers in `src/` to follow `camelCase` and `PascalCase` conventions.
    - Renamed several identifiers in internal stubs for consistency.

### Fixed
- Fixed build system to correctly handle library object dependencies and include paths.
- Fixed a bug in the 6502 `Mode::REL` implementation where branch targets were calculated incorrectly.
