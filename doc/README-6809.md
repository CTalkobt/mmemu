# mmemu-plugin-6809: Motorola 6809 and Hitachi HD6309

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

## The HD6309 (`HD6309`, core name `6309`, #332)

`HD6309` (`src/plugins/6809/main/cpu6309.h`) subclasses `M6809` and is used the same way (`new HD6309()`, `setDataBus`, `step()` returns cycles). It is the CoCo 3's 6309 upgrade.
- **Registers:** E, F (W = E:F), Q (D:W), V and MD after the 6809's in `regRead`/`regWrite`, so `regs` in the CLI and the GDB server's register description show them. MD bit 0 is NM (native mode), bit 1 FM (FIRQ stacks the entire state), bit 6 IL (illegal instruction), bit 7 /0 (divide by zero).
- **Instructions:** all of them. The E/F/W/D/Q loads, stores and arithmetic on pages 2 and 3; the inherent D, W, E and F ops (W, E and F have no NEG, ASR or ASL); ADDR ADCR SUBR SBCR ANDR ORR EORR CMPR; PSHSW PULSW PSHUW PULUW; TFM in its four forms; BAND BIAND BOR BIOR BEOR BIEOR LDBT STBT; MULD DIVD DIVQ; SEXW, LDMD, BITMD; AIM OIM EIM TIM.
- **Indexed modes:** `,W` `n16,W` `,W++` `,--W` and their indirect forms (postbytes `$8F/$90`, `$AF/$B0`, `$CF/$D0`, `$EF/$F0`), plus E, F and W as accumulator offsets.
- **TFR/EXG:** the 6309's register codes, with 0 (codes `$C` and `$D`), W, V, E and F. An 8-bit register read as 16 bits appears in both halves. A 16-bit value written to A, DP or E gives its high byte, and to B, CC or F its low byte.
- **Traps:** undefined opcodes, and a TFM naming a register other than D X Y U S, set MD.IL, stack the entire state and vector through `$FFF0`. DIVD/DIVQ by zero set MD./0 and do the same. BITMD reads and clears those two bits; NM and FM always read as 0.
- **Timing:** counted bus cycle by bus cycle. Emulation mode (NM = 0, the reset state) gives the 6809's counts, and native mode the reduced 6309 counts. In native mode the interrupt frames hold E and F (14 bytes, IRQ/NMI/SWI 21 cycles), and RTI pulls them back (17 cycles). With FM set, FIRQ stacks the entire state and sets E.
- **TFM:** takes 6 + 3n cycles, one byte per `step()`, with PC left on the TFM until it ends. An interrupt is taken between a byte's read and its write. The pointers and W haven't moved yet, so the stacked PC is the TFM's and the transfer resumes after RTI. Z is clear while the transfer runs and set when it ends (hoglet67).
- **Disassembler:** toolchain ISA `6309`. It uses Atkinson's syntax: `AIM #$3C;4,U`, `BAND A,2,7,<$10` (register, source bit, destination bit, address), `TFM X+,Y+`, `LDQ #$12345678`, `LDA ,W++`, `LDA E,X`.
- **Machines:** `raw6309` (64 KB of RAM).

### References and choices

The references are Darren Atkinson's *Motorola 6809 and Hitachi 6309 Programmers Reference* (2009) and its cycle table, David Banks' (hoglet67) *Undocumented 6309 Behaviours*, XRoar's `hd6309.c` (for the cycle sequences) and MAME's `hd6309`. Where they disagree:
- **AIM/OIM/EIM/TIM cycles:** 6 (direct), 7+ (indexed) and 7 (extended) in both modes, from Atkinson. XRoar and MAME each mark their counts as a guess.
- **TSTE/TSTF:** 3/2 cycles, from Atkinson and MAME. XRoar has 4/3.
- **ORCC/ANDCC:** 3/2 cycles, from XRoar and MAME. Atkinson has 3.
- **NEGW (`$1050`):** illegal, as in Atkinson and XRoar. MAME decodes it.
- **MULD flags:** N and Z come from D alone (Atkinson note 6, hoglet67), C is cleared (Atkinson, MAME), and V is left alone (Atkinson, XRoar). XRoar sets N where it means Z when D = 0, and MAME takes N and Z from all 32 bits.
- **LDQ #:** Z comes from D alone and V is left alone (hoglet67, XRoar). The memory forms take Z from all of Q.
- **DIVD/DIVQ:** they follow hoglet67's measurements as XRoar does: one more cycle for each negative operand; on a range overflow, N is set for a negative dividend and D (Q) keeps the dividend's magnitude. Atkinson says N is cleared.
- **Inter-register ops:** a 16-bit destination takes DP as `DP:M`, M being the internal latch (hoglet67; Atkinson says `DP:00`). With CC as the destination, the result is stored first and the flags are then set over it, as in Atkinson's diagram.
- **Bit ops with register code 3:** the register reads as 0 and nothing is written. XRoar writes the result to CC.
- **TFM with bad registers:** also sets MD.IL, as an illegal-instruction exception should; XRoar doesn't set it.

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

`src/plugins/6809/test/test_cpu6309.cpp` covers:
- emulation mode against the 6809 core on 24,000 random documented instructions (registers, memory and cycles);
- about 100 opcode and addressing-mode cycle counts, emulation and native, against Atkinson's table;
- every new instruction group and its flags;
- the W modes and the TFR/EXG register codes;
- MULD, DIVD and DIVQ, including their overflows;
- the /0 and illegal-instruction traps and BITMD;
- TFM in all four forms, including one interrupted part way and resumed;
- native-mode and FM interrupt frames, CWAI and SWI;
- the disassembler and save/load state.

Undocumented encodings were left out of the comparison: opcodes `$01`, `$4E` and the like; store-immediate; and the indexed postbytes `$x7`, `$xA`, `$xE`, and `$xF` without indirection.
