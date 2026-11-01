#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * BASIC text -> tokenised PRG (#149), like VICE's petcat.
 *
 * Input: one program line per text line, "<number> <statements>". Keywords
 * are recognised case-insensitively (longest match), except inside quotes,
 * after REM, and in DATA statements. "?" is PRINT. Letters become unshifted
 * PETSCII (upper case in the default character set); special characters are
 * written in braces, petcat style:
 *   {clr} {home} {up} {down} {left} {right} {rvs on} {rvs off} {del} {inst}
 *   {blk} {wht} {red} {cyn} {pur} {grn} {blu} {yel} {orng} {brn} {lred}
 *   {gry1} {gry2} {lgrn} {lblu} {gry3} {f1}..{f8} {return} {space} {pi}
 *   {$xx} (hex byte), {nnn} (decimal byte), and {name*n} to repeat.
 *
 * Dialects: BASIC V2 (C64, VIC-20, PET) and BASIC 65 (MEGA65; V2 plus the
 * BASIC 10/65 keywords, including the $CE/$FE two-byte tokens). MOD is not
 * tokenised: its token is not documented (the MEGA65 book lists "$NN").
 */
namespace basic_tokenizer {

enum class Dialect { Basic2, Basic65 };

/** Default load address: $0801 (C64) for BASIC V2, $2001 for BASIC 65. */
uint16_t defaultStart(Dialect d);

/** Parse "basic2" / "v2" / "c64" / "basic65" / "65" / "mega65". */
bool parseDialect(const std::string& name, Dialect& out);

struct Result {
    std::vector<uint8_t>     prg;      // load address + program; empty if there were errors
    std::vector<std::string> errors;   // "<file>:<line>: error: <message>"
};

/**
 * Tokenise `text`. `fileName` is used in error messages. Blank lines and lines
 * starting with ';' or '#' (outside a program line) are ignored.
 */
Result tokenize(const std::string& text, Dialect dialect, uint16_t startAddress,
                const std::string& fileName = "<input>");

}  // namespace basic_tokenizer
