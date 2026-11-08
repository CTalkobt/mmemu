#include "test_harness.h"
#include "plugins/machines/bbc/main/machine_bbc.h"
#include "plugins/machines/bbc/main/bbc_disc.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libmem/main/ibus.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {
uint64_t run(MachineDescriptor* m, uint64_t cycles) {
    uint64_t t = 0;
    while (t < cycles) t += (uint64_t)m->schedulerStep(*m);
    return t;
}
std::string screen(MachineDescriptor* m) {
    IBus* bus = m->buses[0].bus;
    std::string s;
    for (int i = 0; i < 1000; ++i) {
        const char c = (char)(bus->peek8((uint16_t)(0x7C00 + i)) & 0x7F);
        s += (c >= 0x20 && c < 0x7F) ? c : ' ';
        if (i % 40 == 39) s += '\n';
    }
    return s;
}
bool screenHas(MachineDescriptor* m, const std::string& t) { return screen(m).find(t) != std::string::npos; }
template <typename T> T* find(MachineDescriptor* m) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if (auto* p = dynamic_cast<T*>(h)) return p;
    return nullptr;
}
// An empty DFS disc: the catalogue (sectors 0 and 1) with a title and the
// sector count (800 for 80 tracks)
std::vector<uint8_t> blankDisc(const char* title) {
    std::vector<uint8_t> d(80 * 2560, 0);
    std::memcpy(d.data(), title, std::min<size_t>(8, std::strlen(title)));
    d[0x106] = 0x03;        // sectors high bits; boot option 0
    d[0x107] = 0x20;        // 800 sectors
    return d;
}
// The catalogue's file names
std::vector<std::string> catalogue(const std::vector<uint8_t>& d) {
    std::vector<std::string> names;
    const int n = d[0x105] / 8;
    for (int i = 0; i < n; i++) {
        std::string name;
        for (int c = 0; c < 7; c++) if (d[8 + i * 8 + c] != ' ') name += (char)(d[8 + i * 8 + c] & 0x7F);
        names.push_back(std::string(1, (char)(d[8 + i * 8 + 7] & 0x7F)) + "." + name);
    }
    return names;
}
std::string tmpPath(const char* name) { return std::string("/tmp/mmemu_test_") + name; }
}  // namespace

// .ssd: track after track; .dsd: side 0 and side 1 tracks alternate
TEST_CASE(bbc_dfs_image_layout) {
    auto ssd = bbc::DfsImage::fromBytes(std::vector<uint8_t>(100 * 1024, 0), false);
    EXPECT_EQ(ssd->tracks(), 40);
    EXPECT_EQ(ssd->sides(), 1);
    EXPECT_EQ(ssd->offset(1, 0, 2), 2560 + 512);
    EXPECT_EQ(ssd->offset(0, 1, 0), -1);
    EXPECT_EQ(ssd->offset(0, 0, 10), -1);
    auto dsd = bbc::DfsImage::fromBytes(std::vector<uint8_t>(400 * 1024, 0), true);
    EXPECT_EQ(dsd->tracks(), 80);
    EXPECT_EQ(dsd->offset(0, 1, 0), 2560);
    EXPECT_EQ(dsd->offset(1, 0, 0), 5120);
    EXPECT_EQ(dsd->offset(79, 1, 9), 79 * 5120 + 2560 + 9 * 256);
}

// DFS 0.90 on the 8271: the banner shows "Acorn DFS"; *CAT reads the
// catalogue; SAVE writes a program to the image file, CHAIN loads and runs it
TEST_CASE(bbc_dfs_save_and_chain) {
    std::string err;
    MachineDescriptor* m = bbc::create(bbc::Model::B, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    const std::string path = tmpPath("dfs.ssd");
    {
        std::ofstream f(path, std::ios::binary);
        auto d = blankDisc("MMEMU");
        f.write((const char*)d.data(), (std::streamsize)d.size());
    }
    auto* fdc = find<bbc::I8271>(m);
    ASSERT(fdc);
    ASSERT(fdc->mountDisk(0, path));
    run(m, 4000000);
    ASSERT(screenHas(m, "Acorn DFS"));
    auto* kbd = find<IKeyboardMatrix>(m);
    ASSERT(kbd);
    kbd->enqueueText("*CAT\n");
    run(m, 8000000);
    EXPECT_TRUE(screenHas(m, "MMEMU"));
    kbd->enqueueText("10 PRINT \"FROM DISC\"\nSAVE \"PROG\"\n");
    run(m, 30000000);
    {
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto names = catalogue(d);
        ASSERT_EQ(names.size(), (size_t)1);
        EXPECT_EQ(names[0], std::string("$.PROG"));
    }
    kbd->enqueueText("NEW\nCHAIN \"PROG\"\n");
    run(m, 25000000);
    if (!screenHas(m, "FROM DISC")) printf("%s", screen(m).c_str());
    EXPECT_TRUE(screenHas(m, "FROM DISC"));
    delete m;
    std::remove(path.c_str());
}
