# Intel 8085 / Oki MSM80C85 CPU (`mmemu-plugin-8085.so`)

The 8085 core is the CPU of the Tandy Model 100/102/200 (#334). It is also
available as a core (`8085`), a disassembler and line assembler (ISA
`8085`), and a `raw8085` machine with 64 KB of RAM and an empty I/O space.

```bash
./bin/mmemu-cli -m raw8085
```

## The core (`src/plugins/8085/main/cpu8085.h`)

`class I8085 : public ICore`, created with `new I8085()`:

- **Buses:** memory on the data bus (`setDataBus`). `IN`/`OUT` use the I/O bus
  (`setIoBus`), addressed by the 8-bit port number. The 8085 puts the port
  number on A0-A7 and on A8-A15.
- **Instructions:** the 8080 set, RIM and SIM, and the undocumented DSUB,
  ARHL, RDEL, LDHI n, LDSI n, RSTV, SHLX, JNK nn, LHLX and JK nn.
- **T-states:** the 8085's, from the Intel 8085AH data sheet. Some examples:
  MOV r,r 4, INX/DCX 6, PUSH 12, CALL 18, Ccc 9/18, Rcc 6/12, Jcc 7/10,
  PCHL/SPHL 6 and HLT 5. `step()` returns the T-states. Each step while
  halted counts 4.
- **Registers:** AF BC DE HL SP PC, plus IM (what RIM reads: SID, the pending
  interrupts, IE and the masks). Writing IM sets the masks, IE and the 7.5
  latch. The GDB server uses GDB's Z80 order (AF BC DE HL SP PC).
- **Interrupts:**
  - `setTrap()`: TRAP is taken on a rising edge, but only if the line is
    still high when it is sampled.
  - `setRst75()`: RST 7.5 is latched on a rising edge, even while it is
    masked. The latch is cleared by SIM bit 4 or when the interrupt is taken.
  - `setRst65()`, `setRst55()`: RST 6.5 and 5.5 are level-triggered.
  - `setIntr()`: INTR is level-triggered.
  - **Priority:** TRAP > 7.5 > 6.5 > 5.5 > INTR. SIM masks RST 5.5, 6.5 and
    7.5. Every interrupt except TRAP needs IE, and EI takes effect after the
    next instruction. Taking any interrupt clears IE.
  - **INTR vector:** INTR reads its instruction from `setIntaCallback()`,
    one byte per call. With no callback it reads $FF, which is RST 7. RST n
    takes 12 T-states and CALL nn takes 18.
  - **Acknowledge hook:** `setAckHook()` is called for every interrupt that
    is taken.
  - **RIM after TRAP:** the first RIM after a TRAP shows IE as it was before
    the TRAP.
  - The ICore IRQ line is INTR and the NMI line is TRAP.
- **HLT:** the CPU waits until an interrupt is taken. With IE off, only TRAP
  wakes it. PC already points past the HLT.
- **Serial:** `setSid()` or `setSidCallback()` drives SID, which RIM bit 7
  reads. `setSodCallback()` and `sod()` report SOD, which SIM sets with bits
  6 and 7.

## Flags

Flags are S Z K AC 0 P V CY, from bit 7 down to bit 0. Bit 3 always reads 0
and stays 0 after POP PSW.

- **AC:**
  - ANA/ANI set AC. This follows Intel's 8085 documentation, and the real-8085
    CRCs in 8085EXM (whose flag mask includes AC) confirm it; the 8080 sets
    it to the OR of the operands' bit 3.
  - ORA, XRA, ORI and XRI clear AC.
  - Subtractions set AC to the carry out of bit 3 of A + ~v + !borrow, as
    on the 8080.
- **V (bit 1):** set when the signed result overflows, meaning the carry
  into bit 7 differs from the carry out. This applies to ADD, ADC, SUB, SBB
  and CMP and their immediate forms.
  - RLC and RAL count as A + A.
  - INR sets V only going from $7F, and DCR only going from $80.
  - After DAA, V is set only when the top digit goes from 7 to 8.
  - DAD sets V from the 16-bit overflow.
  - The logic operations, RRC and RAR clear V.
- **K (bit 5, also called X5 or UI):**
  - For 8-bit results and DAD, K is V XOR the sign of the result. After SUB
    or CMP, K means A < operand as signed numbers.
  - INX and DCX set K from the 16-bit incrementer's carry: INX when the
    result wraps to $0000, DCX when it wraps to $FFFF. They change no other
    flag.
  - RRC and RAR leave K unchanged.
- **Undocumented instructions:**
  - DSUB (HL - BC): computed as SUB L,C followed by SBB H,B. The flags come
    from the high byte, except Z, which covers all 16 bits.
  - ARHL: shifts HL right arithmetically. CY gets bit 0.
  - RDEL: rotates DE left through CY. V is the XOR of DE's old bits 15 and
    14.
  - LDHI, LDSI, SHLX and LHLX change no flags.
  - RSTV is RST 8 (to $0040) when V is set: 12 T-states, or 6 when V is
    clear.

## References and choices

- **Intel documents:** the Intel 8085AH data sheet and the MCS-80/85
  Family User's Manual (instructions, T-states, interrupts, RIM/SIM).
- **Undocumented instructions:** W. Dehnhardt and V. M. Sorensen,
  "Unspecified 8085 op codes enhance programming", *Electronics*, 1979.
- **V and K:** Ken Shirriff, "Looking at silicon to understand the 8085's
  undocumented flags" and "Reverse-engineering the flag circuits in the
  8085" (righto.com, 2013).
  - The 1979 article's K formula is wrong for XOR with two negative
    operands. The core follows Shirriff's die analysis: K = V XOR S.
- **MAME, `src/devices/cpu/i8085/i8085.cpp`:**
  - The core follows MAME on DSUB (as reworked in 2025), on Z covering
    16 bits, and on RIM after TRAP.
  - MAME sets V on DCR, SUB and CMA, and does not compute V for adds. The
    core follows Shirriff here instead.
- **VirtualT (`do_instruct.h`) and Alan Cox's V85:** used for comparison.
  - V85 computes DSUB's Z from the high byte and uses the 8080's AC for ANA.
  - VirtualT also sets V on INX/DCX.
  - Both are known to be inexact for V and K.
- **Test programs:** Ian Bartholomew's 8080/8085 CPU Exerciser, in the
  8085 version (`8085EXM`) with CRCs from a real 8085 and flag mask $D5.
  TST8080, 8080PRE and SuperSoft's CPUTEST. These are freely distributed
  CP/M programs, for example from altairclone.com/downloads/cpu_tests.

## Tests

`src/plugins/8085/test/test_cpu8085.cpp` covers:

- every instruction group, with the flags (including V and K) and T-states;
- interrupts: priorities, masks, RIM/SIM, the TRAP edge, HLT, and the INTR
  vectors;
- snapshots;
- a disassemble/assemble round trip over all 256 opcodes.

To run the CP/M test programs as well:

```bash
MMEMU_8080_TESTS=/path/to/8080-tests bin/mmemu-test "i8085_cpm*"
```

This runs TST8080, 8080PRE, CPUTEST and 8085EXM. They are skipped when the
variable is unset.

| Program | Result |
|---------|--------|
| TST8080 | Passes. |
| 8080PRE | Passes. |
| 8085EXM | Passes every test: about 24 billion T-states, roughly a minute. |
| CPUTEST | Passes tests A-Z and identifies the CPU as 8080/8085. Its next phase fails (see below); the test reports this but does not assert on it. |

CPUTEST's phase after the timing test compares results against an 8080
model, in which flag bit 1 always reads 1. On the 8085 that bit is V, so
the first sequence checked, INR B from $16, reports bit 1 clear. When
PUSH PSW is changed to push the 8080's bit 1 instead, that error goes away
and the phase runs on for more than 2x10^10 instructions.

8080EXM uses the 8080's flag mask, $FF, so its CRCs expect bit 1 always
set and bits 3 and 5 always clear. On the 8085, POP PSW keeps V and K, so
25 of its groups fail, as expected. These include the loads, whose flags
come only from POP PSW. `MMEMU_8080_EXM=1` also runs and prints 8080EXM, which
has the 8080's CRCs, all flag bits included. `MMEMU_8080_SHOW=1` echoes the
exerciser's output as it runs.
