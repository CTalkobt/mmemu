# Atari 8-bit computers (`atari800`, `atari800xl`, `atari65xe`)

The Atari 800 and the 800XL/65XE (#10), with NTSC timing: a 1.79 MHz 6502, ANTIC, GTIA, POKEY and a 6520 PIA. The machine plugin is `src/plugins/machines/atari/` (`mmemu-plugin-atari.so`).

```
mmemu-gui -m atari800xl        # boots to BASIC's READY in about 4 s (the disk boot times out)
mmemu-cli -m atari800 --speed max
```

## Models

| Id | RAM | OS | BASIC |
|---|---|---|---|
| `atari800` | 48 KB | OS revision B (`roms/atari/osb.bin`, 10 KB) | cartridge at `$A000` (`basic.bin`) |
| `atari800xl` | 64 KB | XL OS (`roms/atari/osxl.bin`, 16 KB) | built in, banked by PIA port B |
| `atari65xe` | 64 KB | XL OS | built in |

## Memory map

| Address | 800 | 800XL / 65XE |
|---|---|---|
| `$0000-$9FFF` | RAM | RAM (`$5000-$57FF`: the self-test ROM when port B bit 7 is 0) |
| `$A000-$BFFF` | BASIC cartridge | BASIC when port B bit 1 is 0, else RAM |
| `$C000-$CFFF` | nothing (`$FF`) | OS ROM when port B bit 0 is 1, else RAM |
| `$D000-$D0FF` | GTIA (mirrored every 32 bytes) | same |
| `$D200-$D2FF` | POKEY (every 16) | same |
| `$D300-$D3FF` | PIA: PORTA, PORTB, PACTL, PBCTL (every 4) | same |
| `$D400-$D4FF` | ANTIC (every 16) | same |
| `$D800-$FFFF` | OS-B: floating point at `$D800`, OS at `$E000` | OS ROM when port B bit 0 is 1, else RAM |

On reset, port B's lines float high, so the OS is on and BASIC off. The OS then turns BASIC on unless OPTION is held.

## Timing and interrupts

- **Scheduler:** each step runs one CPU instruction and clocks the devices for its cycles. While ANTIC holds the CPU (WSYNC, or the DMA cycles ANTIC takes), the step instead clocks the devices one cycle without the CPU.
- **Interrupts:**
  - POKEY and the PIA share IRQ.
  - ANTIC drives NMI: the vertical blank, display-list interrupts, and the 800's RESET key.
- **Real time (#169):** paced on ANTIC's cycle count, which includes the cycles the CPU loses.

## Keyboard

- **Keys:** names from the GUI or CLI `key` (`a`, `return`, `space`, `left_shift`, `control`, `delete`, `escape`, `tab`, arrow keys...) are converted to POKEY key codes. Shifted symbols come in as names such as `dquote` and `exclaim`.
- **Function keys:**

  | Key | Atari key |
  |---|---|
  | F1 | Atari (inverse) key |
  | F2 | OPTION |
  | F3 | SELECT |
  | F4 | START |
  | F5 | RESET: a reset on the XL/XE, the RESET NMI on the 800 |
  | F6 | HELP |
  | F7 | BREAK |
  | F8 | CAPS |

- **Typed text (CLI `type`, MCP `type_string`):**
  - Each key is held 2 frames and released for 2.
  - A repeated key waits longer, because of the OS's debounce.
  - The next key waits until the OS has taken the last one from `CH` (`$2FC`), so BASIC keeps up.
  - Letters are typed as caps-lock capitals.
- **Joysticks:** `onJoystick` maps them to PIA port A (sticks 0 and 1) and the GTIA triggers.

## Not yet supported

- SIO devices: disk drives, `.atr` and `.xex` loading, and the cassette.
- Cartridges other than the 800's BASIC.
- PAL machines.
- The 400 and 1200XL.
- GTIA modes and VDELAY.
- The console speaker (key click).
