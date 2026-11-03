/// Test: the ca45 backend's assembleLine() (dot-assembly of 45GS02 lines)
/// through libca45 (m65compiler #117). Built without libca45 it returns -1.

#include "test_harness.h"
#include "plugins/45gs02/main/ca45_assembler.h"
#include <cstring>

namespace {
// assemble <line> at <addr>; true when the bytes are exactly <want>
bool bytes(Ca45AssemblerBackend& a, const char* line, uint32_t addr,
           std::initializer_list<uint8_t> want) {
    uint8_t buf[16];
    std::memset(buf, 0, sizeof buf);
    int n = a.assembleLine(line, buf, sizeof buf, addr);
    if (n != (int)want.size()) return false;
    int i = 0;
    for (uint8_t b : want)
        if (buf[i++] != b) return false;
    return true;
}
}

#ifdef MMEMU_HAVE_LIBCA45

TEST_CASE(ca45_assemble_line_45gs02_instructions) {
    Ca45AssemblerBackend a;
    ASSERT(bytes(a, "MAP", 0x2000, {0x5C}));
    ASSERT(bytes(a, "EOM", 0x2000, {0xEA}));
    ASSERT(bytes(a, "NEG A", 0x2000, {0x42}));
    ASSERT(bytes(a, "ASR A", 0x2000, {0x43}));
    ASSERT(bytes(a, "LDZ #$01", 0x2000, {0xA3, 0x01}));
    ASSERT(bytes(a, "LDA [$6C],Z", 0x2000, {0xEA, 0xB2, 0x6C}));
    ASSERT(bytes(a, "LDQ $12", 0x2000, {0x42, 0x42, 0xA5, 0x12}));
    ASSERT(bytes(a, "lda #$42", 0x2000, {0xA9, 0x42}));
}

TEST_CASE(ca45_assemble_line_branches_use_current_address) {
    Ca45AssemblerBackend a;
    // 8-bit: $2010 - ($2000 + 2)
    ASSERT(bytes(a, "BRA $2010", 0x2000, {0x80, 0x0E}));
    ASSERT(bytes(a, "BEQ $1000", 0x1006, {0xF0, 0xF8}));
    // out of 8-bit range: the 16-bit branch, $3000 - ($2000 + 2)
    ASSERT(bytes(a, "BNE $3000", 0x2000, {0xD3, 0xFE, 0x0F}));
}

TEST_CASE(ca45_assemble_line_errors) {
    Ca45AssemblerBackend a;
    uint8_t buf[16];
    ASSERT(a.assembleLine("FOO $12", buf, sizeof buf, 0x2000) == -1);
    ASSERT(a.lastError().find("FOO") != std::string::npos);
    // too small a buffer
    ASSERT(a.assembleLine("LDQ $12", buf, 2, 0x2000) == -1);
    // a success clears the error
    ASSERT(a.assembleLine("MAP", buf, sizeof buf, 0x2000) == 1);
    ASSERT(a.lastError().empty());
}

#else

TEST_CASE(ca45_assemble_line_without_libca45) {
    Ca45AssemblerBackend a;
    uint8_t buf[16];
    ASSERT(a.assembleLine("MAP", buf, sizeof buf, 0x2000) == -1);
    ASSERT(!a.lastError().empty());
    (void)bytes;
}

#endif
