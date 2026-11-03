# Contributing to mmemu

Project home: **https://github.com/CTalkobt/mmemu**

## Licence

mmemu is free software under the **GNU Affero General Public License, version 3 or later** (AGPL-3.0-or-later); see [LICENSE](LICENSE). In short:

- You may use, study, modify and share it.
- If you distribute mmemu or a modified version, you must make the complete source of that version available, under the same licence.
- If you run a modified version for others over a network (for example, hosting the MCP server), you must offer those users its source too.

Contributions are accepted under the same licence.

## Please offer your changes back

The licence requires that modified versions you distribute stay open. Beyond that, we ask that you **offer your changes back to the project before you distribute them**, so everyone gets the fixes and the versions don't drift apart. This is a request, not a licence condition. The easiest way is a pull request to https://github.com/CTalkobt/mmemu; an issue describing the change is welcome too.

## How to contribute

1. **Discuss larger changes first:** open an issue at https://github.com/CTalkobt/mmemu/issues.
2. **Branch** from `master`, one topic per branch.
3. **Follow the conventions** in [STYLEGUIDE.md](STYLEGUIDE.md) and [CLAUDE.md](CLAUDE.md): C++17, `#pragma once`, camelCase functions, PascalCase classes.
4. **Add tests** next to the code: `src/<module>/test/test_*.cpp`, listed in `TEST_SRCS` in the Makefile.
5. **Run the tests:** `make -j 12 all bin/mmemu-test && bin/mmemu-test`, plus `make test-mcp` if you touched the MCP server.
6. **Update [CHANGELOG.md](CHANGELOG.md)** and the relevant `doc/README-*.md`.
7. **Sign off your commits** (`git commit -s`), adding a `Signed-off-by:` line. This certifies the [Developer Certificate of Origin](https://developercertificate.org/): you wrote the change or have the right to submit it under the project's licence.
8. **Open a pull request** against `master` at https://github.com/CTalkobt/mmemu.

## Third-party code and data

Credit any code, models or measured data taken from other projects in the source and in [NOTICE](NOTICE). Their licence must be compatible with AGPL-3.0-or-later.
