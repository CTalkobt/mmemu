#include "test_harness.h"
#include "libtoolchain/main/source_map.h"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

static fs::path writeTempList(const std::string& content) {
    fs::path p = fs::temp_directory_path() / "mmemu_test_kickass.lst";
    std::ofstream f(p);
    f << content;
    return p;
}

TEST_CASE(source_map_load_kickass_list) {
    // Simulate a KickAssembler listing:
    //   [1000] 4c 00 10  1  main: jmp main
    //   [1003] a9 42     2  lda #$42
    auto path = writeTempList(
        "[1000] 4c 00 10  1  main: jmp main\n"
        "[1003] a9 42     2  lda #$42\n"
        "  ; comment line\n"
        "\n"
        "[2000] 60        5  rts\n"
    );

    SourceMap sm;
    ASSERT(sm.loadKickAssList(path.string()));

    // Addresses should be parsed
    auto loc1 = sm.addrToSource(0x1000);
    ASSERT(!loc1.file.empty());

    auto loc2 = sm.addrToSource(0x1003);
    ASSERT(!loc2.file.empty());

    auto loc3 = sm.addrToSource(0x2000);
    ASSERT(!loc3.file.empty());

    // Unknown address
    auto locMissing = sm.addrToSource(0xFFFF);
    ASSERT(locMissing.file.empty());
    ASSERT_EQ(locMissing.line, -1);

    fs::remove(path);
}

TEST_CASE(source_map_load_nonexistent) {
    SourceMap sm;
    ASSERT(!sm.loadKickAssList("/tmp/mmemu_test_nonexistent_file.lst"));
}

TEST_CASE(source_map_source_to_addr) {
    SourceMap sm;
    // sourceToAddr on empty map returns 0xFFFFFFFF
    ASSERT_EQ(sm.sourceToAddr("test.asm", 1), (uint32_t)0xFFFFFFFF);
    ASSERT_EQ(sm.sourceToAddr("nonexistent.asm", 99), (uint32_t)0xFFFFFFFF);
}

TEST_CASE(source_map_empty_file) {
    auto path = writeTempList("");
    SourceMap sm;
    ASSERT(sm.loadKickAssList(path.string()));
    auto loc = sm.addrToSource(0x0000);
    ASSERT(loc.file.empty());
    fs::remove(path);
}

TEST_CASE(source_map_malformed_lines) {
    // Lines without brackets should be skipped
    auto path = writeTempList(
        "this is not a valid line\n"
        "[abcd incomplete line\n"
        "[CAFE] ea       nop\n"
    );
    SourceMap sm;
    ASSERT(sm.loadKickAssList(path.string()));
    auto loc = sm.addrToSource(0xCAFE);
    ASSERT(!loc.file.empty());
    fs::remove(path);
}

// ca45 --source-map files (#147): "Line N" rows belong to the given source,
// "<file>:<line>" rows name their file; the Symbol Table section is returned.
TEST_CASE(source_map_load_ca45_map) {
    fs::path dir = fs::temp_directory_path() / "mmemu_test_ca45map";
    fs::create_directories(dir);
    fs::path map = dir / "demo.map";
    {
        std::ofstream f(map);
        f << "Source Map \xE2\x80\x94 Binary Address to Source Line Mapping\n"
             "=====================================================\n\n"
             "00002000  Line 3  start:  lda #$00\n"
             "00002002  Line 4  sta $d020\n"
             "00002005  Line 4  ; second instruction on the same line keeps the first address\n"
             "0000200d  sub.s:2  flash:  ldx #$03\n\n\n"
             "Symbol Table\n"
             "============\n"
             "flash                            0000200d (ADDR)\n"
             "start                            00002000 (ADDR)\n";
    }
    std::vector<std::pair<std::string, uint32_t>> symbols;
    SourceMap sm;
    int n = sm.loadCa45SourceMap(map.string(), (dir / "demo.s").string(), &symbols);
    ASSERT_EQ(n, 4);

    const std::string demo = SourceMap::normalizePath((dir / "demo.s").string());
    const std::string sub  = SourceMap::normalizePath((dir / "sub.s").string());
    ASSERT(sm.addrToSource(0x2000).file == demo);
    ASSERT_EQ(sm.addrToSource(0x2000).line, 3);
    ASSERT_EQ(sm.addrToSource(0x2005).line, 4);
    ASSERT_EQ((int)sm.sourceToAddr(demo, 4), 0x2002);      // first address of the line
    ASSERT(sm.addrToSource(0x200d).file == sub);           // file:line form, relative to the map
    ASSERT_EQ(sm.addrToSource(0x200d).line, 2);
    ASSERT_EQ((int)sm.sourceToAddr(demo, 99), (int)0xFFFFFFFF);

    ASSERT_EQ((int)symbols.size(), 2);
    ASSERT(symbols[0].first == "flash" && symbols[0].second == 0x200d);

    ASSERT_EQ(sm.loadCa45SourceMap((dir / "missing.map").string(), demo), 0);
    sm.clear();
    ASSERT(sm.empty());
    fs::remove_all(dir);
}
