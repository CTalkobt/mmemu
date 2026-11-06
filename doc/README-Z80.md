# mmemu-plugin-z80: Zilog Z80

A Z80 core (#22: the C128's second CPU), its disassembler, and a bare
`rawz80` machine (64 KB RAM, an empty I/O space) for trying code in the CLI.

## The core (`Z80`, core name `z80`)

- **Instructions**: everything, documented and undocumented: the CB, ED,
  DD/FD and DDCB/FDCB prefixes; IXH, IXL, IYH, IYL; SLL; the DDCB/FDCB
  forms that also copy the result to a register; `IN (C)` / `OUT (C),0`;
  undefined ED opcodes run as 8-T-state NOPs; repeated DD/FD prefixes (the
  last one wins).
- **Flags**: including the undocumented bits 3 and 5 (X, Y), with MEMPTR
  (WZ) kept so `BIT n,(HL)` shows its high byte, the block instructions'
  X/Y and I/O flags, and the Q latch that SCF and CCF read X/Y through
  (Patrik Rak). References: "The Undocumented Z80 Documented" (Sean Young),
  the Zilog manual, "Decoding Z80 Opcodes" (Cristian Dinu).
- **Timing**: `step()` returns T-states (the Zilog figures, including taken
  and not-taken branches and the repeating block instructions).
- **Interrupts**: a level-sensitive /INT (`setIrqLine`) in modes 0, 1 and 2,
  with the data bus reading `$FF` (mode 0 runs `RST 38h`, mode 2 takes the
  vector at I x 256 + `$FF`); an edge-triggered /NMI to `$0066`; HALT; EI's
  one-instruction delay; RETN/RETI restore IFF1 from IFF2; R counts M1
  cycles (7 bits).
- **I/O**: `IN`/`OUT` go to the I/O bus (`setIoBus`) with the full 16-bit
  port address: B (or A for `IN A,(n)` / `OUT (n),A`) in the high byte.
- **Registers**: AF BC DE HL IX IY SP PC, the shadow set AF' BC' DE' HL', I,
  R, and internally IM, IFF1/IFF2 and WZ.

## Testing

`src/plugins/z80/test/test_z80.cpp`: instructions and flags, index
registers, block copy, I/O, interrupt modes, the disassembler. Frank
Cringle's exercisers ZEXDOC (documented flags) and ZEXALL (all flags) are
in `tests/z80/` and run under a minimal CP/M harness (BDOS 2 and 9); they
take a few minutes each, so they run only on request:

```bash
MMEMU_ZEX=1 bin/mmemu-test z80_zex      # both (MMEMU_ZEX=doc or all for one)
```

Both report every group OK.
