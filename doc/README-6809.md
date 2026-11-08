# mmemu-plugin-6809: Motorola 6809

## The core (`M6809`, core name `6809`)

The core follows the MC6809/MC6809E data sheet (Motorola).
- **Instructions:** every documented instruction on pages 0, 2 (`$10`) and 3 (`$11`).
- **Indexed modes:** 5-, 8- and 16-bit offsets, A/B/D offsets, auto-increment and decrement, PC-relative, extended indirect, and the indirect forms, each with its extra cycles.
- **Other instructions:** TFR/EXG (an 8-bit register reads as `$FF00` | it in a 16-bit transfer), PSH/PUL, CWAI, SYNC, SWI/SWI2/SWI3, DAA and MUL.
- **Cycles:** `step()` returns the data sheet's cycle counts.
- **Interrupts:**
  - /IRQ (`setIrqLine`) and /FIRQ (`setFirqLine`) are level-sensitive; /NMI is edge-triggered.
  - NMI is not taken until S has been loaded (LDS, LEAS, TFR/EXG or PULU into S), as the data sheet says.
  - FIRQ stacks PC and CC with E clear; IRQ, NMI and SWI stack everything with E set.
  - After CWAI, an interrupt doesn't stack the state again.
- **Vectors:** at `$FFF0-$FFFF`. `setVectorBase` moves them; b-em's 6809 Second Processor board fetches them from `$FEF0`.
- **Disassembler:** Motorola mnemonics (`LDA [$1234]`, `PSHS A,B,X`, `LEAX D,X`, `BNE $1000`); toolchain ISA `6809`.
- **Machines:** `raw6809` (64 KB of RAM). The BBC Micro's `bbcb-6809` uses it as its second processor.

## Testing

`src/plugins/6809/test/test_cpu6809.cpp` covers:
- arithmetic and flags with their cycle counts, including DAA;
- the indexed modes and their extra cycles;
- FIRQ/IRQ/NMI stacking and RTI, and NMI arming;
- CWAI and SYNC;
- the disassembler.

The core was also compared with GCC6809's simulator (b-em's `mc6809nc.c`, by Arto Salmi, Joze Fabcic and Brian Dominy) on about 160,000 random instructions: registers, memory and cycles all agree, apart from:
- **DAA:** mc6809nc can add `$60` twice; the data sheet and MAME add it once.
- **16-bit stores at `$FFFF`:** the comparison's plain memory array doesn't wrap to `$0000`.

Undocumented encodings were left out of the comparison: opcodes `$01`, `$4E` and the like; store-immediate; and the indexed postbytes `$x7`, `$xA`, `$xE`, and `$xF` without indirection.
