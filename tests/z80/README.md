# Z80 instruction exercisers

`zexdoc.com` / `zexall.com` and their sources `zexdoc.z80` / `zexall.z80`:
the Z80 instruction set exercisers by Frank D. Cringle (1994), GNU GPL
version 2 or later (see the source headers). ZEXDOC checks the documented
flags, ZEXALL all of them. Copies from the z80emu project
(github.com/anotherlin/z80emu, `testfiles/`).

Run with `MMEMU_ZEX=1 bin/mmemu-test z80_zex` (see `doc/README-Z80.md`).

`fuse/tests.in` / `fuse/tests.expected`: Fuse's Z80 core tests (Fuse
`z80/tests`, Philip Kendall and contributors, GNU GPL version 2 or
later). Each test's bus events with their T-states, registers and changed
memory; run by `bin/mmemu-test z80_fuse_core_tests`.
