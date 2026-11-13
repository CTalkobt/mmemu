# mmemu-plugin-6801: Motorola MC6801 / MC6803

## The core (`M6801`, core names `6803` and `6801`)

The core follows the MC6801/MC6803 data sheet and the MC6801 8-Bit
Single-Chip Microcomputer Reference Manual (Motorola).

- **Instructions:** the 6800 instruction set with the 6801's additions:
  MUL, ABX, ADDD/SUBD, LDD/STD, PSHX/PULX, LSRD/ASLD, BRN, JSR direct, and
  CPX setting all four flags. TST clears C; shifts and rotates set V to
  N xor C; DAA sets C but never clears it.
- **Cycles:** `step()` returns the data sheet's counts (MUL 10, RTI 10,
  SWI 12, WAI 9, PSHX 4, PULX 5, ABX 3, JSR 5/6/6, BSR 6, an interrupt 12).
  Each bus cycle ticks the timer and the SCI as it happens; `cycleInStep()`
  tells a machine which cycle of the instruction an external access is
  in.
- **Undocumented opcodes** behave as XRoar's `mc6801.c` found them on
  hardware: `$00` CLB, `$02` SEXA, `$03` SETA, `$12`/`$13` SBC/subtract-one
  forms of SBA, `$14`/`$15`/`$1C`/`$1D` B=A-1 / A=B-1, `$18`/`$1A` ABA
  without H and C, `$1E` TAB, `$1F` TBA setting C, the RMW group's `$x1`
  (NEG's flags, no store), `$x2` (NEG or COM by C), `$x5` (LSR), `$xB`
  (DEC with C = no borrow), `$87`/`$C7` (load `$FF`), `$8F`/`$CD`/`$CF`
  (flags only), and `$4E`/`$5E` (HCF: the CPU stops until /RESET). The
  disassembler shows them as `FCB $xx`.
- **Interrupts:** /NMI on its edge (`setNmiLine`), /IRQ1 level-sensitive
  (`setIrqLine`), and IRQ2 from the timer (ICF, OCF, TOF with their enables
  in TCSR) and the SCI (RDRF or ORFE with RIE, TDRE with TIE). Priority
  NMI, IRQ1, ICF, OCF, TOF, SCI; vectors `$FFFC`, `$FFF8`, `$FFF6`,
  `$FFF4`, `$FFF2`, `$FFF0`; SWI `$FFFA`, /RESET `$FFFE`. CLI (or TAP
  clearing I) lets an interrupt in only after the next instruction. WAI
  stacks the registers first: the interrupt after it takes 3 cycles.

## The on-chip peripherals

| Address | Register |
|---------|----------|
| `$00`/`$01` | Port 1 / port 2 data direction (write only; read `$FF`) |
| `$02`/`$03` | Port 1 / port 2 data. Port 2 reads PC2-PC0 (the mode) in bits 7-5 |
| `$04`-`$07` | Ports 3 and 4 (internal in the single-chip modes only) |
| `$08` | TCSR: ICF OCF TOF EICI EOCI ETOI IEDG OLVL |
| `$09`/`$0A` | The free-running counter (reading the MSB latches the LSB; writing the MSB presets `$FFF8`) |
| `$0B`/`$0C` | Output compare |
| `$0D`/`$0E` | Input capture |
| `$0F` | Port 3 control/status (single-chip modes) |
| `$10` | RMCR: clock control and the rate (E/16, /128, /1024, /4096) |
| `$11` | TRCSR: RDRF ORFE TDRE RIE RE TIE TE WU |
| `$12`/`$13` | RDR / TDR |
| `$14` | RAM control: RAME (bit 6), STBY PWR (bit 7) |
| `$80`-`$FF` | 128 bytes of RAM |

- **Modes:** PC2-PC0 come from P22-P20 at /RESET (`setModePins`, default
  2). The MC6803 runs in mode 2 (expanded multiplexed, internal RAM) or 3
  (no internal RAM); the MC6801's 2 KB ROM (`setInternalRom`) is at
  `$F800` in modes 0, 5, 6 and 7. In the multiplexed modes the port 3/4
  registers are external, as the data sheet has them.
- **Timer:** TOF at `$FFFF`; OCF when the counter matches the output
  compare register (the compare held off for the cycle after a write to
  its MSB), with OLVL driven out on P21 if its DDR bit is set; ICF on
  P20's edge (IEDG) with the counter captured (`setPort2Input`). Each flag
  clears as the data sheet says: read TCSR with it set, then read the
  counter's MSB (TOF), write the output compare (OCF) or read the capture's
  MSB (ICF).
- **SCI:** NRZ at the internal rate (or `sciExternalClock()`, 8 a bit, for
  CC = 11). Enabling TE sends a preamble of ten 1s; TDR is shifted out
  LSB first with start and stop bits on P24 (`onTxLine`, `onTxByte`).
  The receiver samples P23 (or characters queued with `receiveByte`):
  RDRF, overrun and framing errors (ORFE), and the wake-up mode (WU,
  cleared by ten 1s). RDRF/ORFE clear by reading TRCSR then RDR; TDRE by
  reading TRCSR then writing TDR.
- **Ports:** each `Port` has the DDR, the data register, what drives its
  input pins (`in`), and `preRead`/`postWrite` callbacks for the machine.

Not modelled: the bi-phase SCI format (CC = 00 runs as NRZ), port 3's IS3
handshake and its interrupt, the standby/power-down behaviour of RAM
control's STBY PWR, and the address trap.

## Disassembler and assembler

The disassembler writes Motorola syntax (`LDAA #$12`, `LDAA $12` direct,
`LDAA $1234`, `LDAA $05,X`, `BNE $1000`, `LDD`, `ASLD`); toolchain ISAs
`6803` and `6801`. The line assembler (`.asm` in the CLI and GUI)
reads all of it back, plus `LDA`/`STA`/`ORA` (and the B forms) as short
names, `LSLD`/`LSL`, `BHS`/`BLO`, `<` and `>` to force direct or extended
(four hex digits, `$0012`, are extended), `,X` for `0,X`, and
`FCB`/`FDB` (`DB`/`DW`, `.BYTE`/`.WORD`).

## Machines

`raw6803`: an MC6803 in mode 2 with 64 KB of RAM. The debugger sees the
chip's registers and RAM where the chip decodes them. The TRS-80 MC-10 and
the Matra Alice use it: see [README-MC10.md](README-MC10.md).

## Testing

`src/plugins/6801/test/test_cpu6801.cpp`:
- arithmetic and the 16-bit operations with flags and cycles;
- the addressing modes' cycles (direct, indexed, extended);
- subroutines and the stack (BSR, JSR in three modes, RTS, PSH/PUL, PSHX/PULX, TSX/TXS);
- shifts and the memory read-modify-write forms (V = N xor C, TST clearing C);
- every branch condition, taken and not;
- DAA and the inherent operations;
- IRQ1 (stacking order, 12 cycles), RTI, SWI, CLI's delay, NMI's edge, WAI;
- the internal RAM and registers, the modes, the MC6801's ROM;
- the ports; the timer (counter, preset, TOF/OCF/ICF and their clearing, the OCF interrupt, OLVL);
- the SCI (a character out and in, TDRE, RDRF and its interrupt);
- the undocumented opcodes; the disassembler.

`test_asm6801.cpp` assembles every documented opcode's disassembly back to
the same bytes.
