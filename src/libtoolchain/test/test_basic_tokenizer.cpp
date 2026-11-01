#include "test_harness.h"
#include "libtoolchain/main/basic_tokenizer.h"

using namespace basic_tokenizer;

// 10 PRINT "HELLO" -> the well-known C64 bytes
TEST_CASE(basic_tokenize_print_hello) {
    auto r = tokenize("10 print \"hello\"\n", Dialect::Basic2, 0x0801);
    ASSERT(r.errors.empty());
    const std::vector<uint8_t> expected = {
        0x01, 0x08,                    // load address
        0x0F, 0x08, 0x0A, 0x00,        // link $080F, line 10
        0x99, 0x20, 0x22, 0x48, 0x45, 0x4C, 0x4C, 0x4F, 0x22, 0x00,
        0x00, 0x00 };
    ASSERT(r.prg == expected);
}

TEST_CASE(basic_tokenize_keywords_and_literals) {
    // Longest match (PRINT# over PRINT), operators, ?, quotes/REM/DATA left alone
    auto r = tokenize("10 PRINT#1,A:?\"FOR\"\n20 REM GOTO 10\n30 DATA FOR,1:GOTO 10\n", Dialect::Basic2, 0x0801);
    ASSERT(r.errors.empty());
    const auto& p = r.prg;
    // line 10 body starts at offset 2 + 4
    std::vector<uint8_t> l10(p.begin() + 6, p.begin() + 6 + 9);
    ASSERT((l10 == std::vector<uint8_t>{0x98, 0x31, 0x2C, 0x41, 0x3A, 0x99, 0x22, 0x46, 0x4F}));
    std::string s(p.begin(), p.end());
    ASSERT(s.find(std::string("\x8F GOTO 10", 9)) != std::string::npos);            // REM keeps GOTO as text
    ASSERT(s.find(std::string("\x83 FOR,1:\x89 10", 13)) != std::string::npos);     // DATA literal until ':'
}

TEST_CASE(basic_tokenize_escapes_and_basic65) {
    auto r = tokenize("10 PRINT\"{clr}{red}{down*2}{$41}\"\n", Dialect::Basic2, 0x0801);
    ASSERT(r.errors.empty());
    std::string s(r.prg.begin(), r.prg.end());
    ASSERT(s.find(std::string("\"\x93\x1C\x11\x11\x41\"", 7)) != std::string::npos);

    auto r65 = tokenize("10 BORDER 0:SPRITE 1,1:LOG10(1)\n", Dialect::Basic65, defaultStart(Dialect::Basic65));
    ASSERT(r65.errors.empty());
    ASSERT(r65.prg[0] == 0x01 && r65.prg[1] == 0x20);   // $2001
    std::string s65(r65.prg.begin(), r65.prg.end());
    ASSERT(s65.find(std::string("\xFE\x3C 0:\xFE\x07 1,1:\xCE\x08(1)", 16)) != std::string::npos);
    // In BASIC V2 the BASIC 65 keywords are just letters
    auto r2 = tokenize("10 BORDER 0\n", Dialect::Basic2, 0x0801);
    ASSERT(r2.errors.empty() && r2.prg[6] == 'B');
}

TEST_CASE(basic_tokenize_errors) {
    auto r = tokenize("10 PRINT\nPRINT\n5 END\n20 PRINT \"{bogus}\"\n", Dialect::Basic2, 0x0801, "t.bas");
    ASSERT(r.prg.empty());
    ASSERT_EQ((int)r.errors.size(), 3);
    ASSERT(r.errors[0] == "t.bas:2: error: expected a line number");
    ASSERT(r.errors[1].find("t.bas:3: error: line number 5 does not follow 10") == 0);
    ASSERT(r.errors[2].find("t.bas:4: error: unknown escape {bogus}") == 0);
}
