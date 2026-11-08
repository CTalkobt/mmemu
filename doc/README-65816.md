# mmemu-plugin-65816: WDC 65C816

## The core (`W65C816`, core name `65816`)

The core follows the W65C816S data sheet (WDC).

**Registers:**
- **Accumulator and index registers:** 16 bits, made 8-bit by the M and X flags.
- **Direct page and bank registers:** the direct page register, and the data and program bank registers.
- **Emulation mode:** entered with XCE. M and X stay set, the stack is in page 1, and the direct page wraps within its page when its low byte is 0.

**Instructions:** all 256 opcodes and every addressing mode:
- direct page, indexed and indirect, including `[dp]` and `[dp],Y` long;
- absolute, long and long indexed;
- stack relative `sr,S` and `(sr,S),Y`;
- `(abs,X)` jumps, MVN/MVP block moves, PEA/PEI/PER, COP, WAI, STP;
- decimal mode in 8 and 16 bits.

**Cycles:** each bus cycle is a 24-bit access to the bus or an internal cycle, in the data sheet's order. That includes the conditional cycles:
- a direct page whose low byte isn't 0;
- an index crossing a page, or a 16-bit index;
- a branch crossing a page in emulation mode;
- the emulation-mode dummy write of a read-modify-write.

`step()` returns the cycle count.

**MVN and MVP** move one byte a pass and repeat until A runs out, so an interrupt can come between bytes, as on the chip.

**Interrupts:**
- /IRQ is level-sensitive and /NMI edge-triggered.
- Native-mode vectors: COP `$FFE4`, BRK `$FFE6`, NMI `$FFEA`, IRQ `$FFEE`.
- Emulation-mode vectors: COP `$FFF4`, NMI `$FFFA`, RESET `$FFFC`, IRQ/BRK `$FFFE`. B is set on the stack for BRK and clear for IRQ.
- WAI wakes on an interrupt even when it is masked.

**Also:** a disassembler with WDC mnemonics, whose operand widths follow the current M and X (toolchain ISA `65816`), and the `raw65816` machine (16 MB of RAM).

## Testing

`src/plugins/65816/test/test_cpu65816.cpp` runs Tom Harte's SingleStepTests (github.com/SingleStepTests/65816, MIT licence): 10,000 tests for each opcode in native mode and 10,000 in emulation mode. For each test it checks:
- the final registers and memory;
- the number of bus cycles;
- the sequence of bus accesses: address, read or write, and value.

All 5,080,400 match. The tests (3 GB) aren't in this repository: clone them beside it as `../refs/ss65816`, or set `MMEMU_65816_TESTS`.

```bash
bin/mmemu-test w65c816_single_step_tests                      # the first 200 of each file (about a second)
MMEMU_65816_FULL=1 bin/mmemu-test w65c816_single_step_tests   # all of them (about 80 seconds)
```

`w65c816_interrupts` covers what the suite doesn't: IRQ and NMI in both modes, and WAI.
