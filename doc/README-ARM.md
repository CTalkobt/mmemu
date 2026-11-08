# mmemu-plugin-arm: Acorn ARM1 / ARM2

## The core (`Arm2`, cores `arm2` and `arm1`)

The core follows the 26-bit architecture of the ARM2 datasheet (VLSI VL86C010) and the ARM1 manual.

**R15** holds the PC (bits 2-25) and the PSR:

| Bits | Holds |
|------|-------|
| 31-28 | N Z C V |
| 27-26 | I F |
| 1-0 | The mode: user, FIQ, IRQ, supervisor |

R15 reads as the instruction's address + 8, or + 12 as a register-shift operand or as STR's source.

**Instructions:**
- **Data processing:** the barrel shifter, with immediate and register shifts, RRX, and the shifter's carry. The `P` forms (TEQP etc.) set the PSR. An S write to R15 sets the PSR as well; in user mode only N, Z, C and V change.
- **Multiply:** MUL and MLA, on the ARM2 only; the ARM1 traps them as undefined.
- **Single transfers:** LDR/STR, byte and word, with pre/post index and writeback. Unaligned word loads are rotated.
- **Block transfers:** LDM/STM. With `^` they use the user bank, or for LDM with R15 the PSR. The base is written back in the current mode's bank.
- **B/BL and SWI.** Coprocessor instructions trap as undefined, since none is fitted.

**Banked registers:** R8-R14 in FIQ mode; R13-R14 in IRQ and supervisor modes.

**Exceptions:**

| Vector | Exception |
|--------|-----------|
| `$00` | Reset |
| `$04` | Undefined instruction |
| `$08` | SWI |
| `$14` | Address exception (a data address above 64 MB) |
| `$18` | IRQ |
| `$1C` | FIQ |

/IRQ and /FIQ are level-sensitive. `setNmiLine` drives FIQ.

**Memory:** words go to the bus as `read32`/`write32` (little-endian), so a board can treat word and byte accesses to its devices differently.

**Also:** a disassembler in Acorn syntax (`LDRB R3,[R1,#&4]!`, `STMDB R13!,{R1,R14}`, `TEQP PC,#&0`), with toolchain ISA `arm`, and the `rawarm` machine (4 MB of RAM).

**Used by:** the BBC Micro's `bbcb-arm` (the ARM Evaluation System), as an ARM1.

## Testing

`src/plugins/arm/test/test_cpu_arm2.cpp` covers:
- data processing, the shifter's carry and RRX, conditions, and R15 reads;
- LDR/STR, LDM/STM, BL, and returns with `MOVS PC,R14`;
- SWI, user mode, IRQ and FIQ with their banks;
- MUL on the ARM2, undefined on the ARM1;
- the disassembler.

The core was also compared with b-em's `arm.c` (Tom Walker's, from Arculator), in ARM2 mode, on about 160,000 random instructions in every mode. Registers, the PSR, the next PC and memory all agree, apart from three places where b-em departs from the datasheet:
- **A user-mode S write to R15:** b-em changes I, F and the mode bits; the datasheet allows only N Z C V.
- **R15 as a destination:** b-em adds its pipeline's 4 to the whole value, so the carry can spill into the PSR.
- **A register-specified ROR by a multiple of 32:** b-em clears C; the datasheet gives bit 31.

The comparison found two bugs in this core, both fixed:
- **STR writing back into its own base:** it stored the updated base.
- **LDM/STM `^` from a privileged mode:** the writeback went to the user bank.

Left out of the comparison, as unpredictable on the ARM2:
- R15 as an operand with a register-specified shift, or as Rs;
- MUL/MLA into R15;
- block transfers based on R15;
- a single transfer writing back into its own Rd;
- byte transfers of R15;
- TST/TEQ/CMP/CMN with S clear;
- LDRT/STRT;
- addresses outside RAM.
