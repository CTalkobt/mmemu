#include "test_harness.h"
#include "plugins/machines/bbc/main/machine_bbc.h"
#include "plugins/machines/bbc/main/bbc_tape.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libmem/main/ibus.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <zlib.h>

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
// Run until `text` is on the screen (or `limit` cycles)
bool runUntil(MachineDescriptor* m, const std::string& text, uint64_t limit) {
    for (uint64_t t = 0; t < limit; t += 1000000) {
        run(m, 1000000);
        if (screenHas(m, text)) return true;
    }
    return false;
}
std::vector<uint8_t> gzip(const std::vector<uint8_t>& in) {
    z_stream zs{};
    deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    std::vector<uint8_t> out(in.size() + 1024);
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = (uInt)in.size();
    zs.next_out = out.data();
    zs.avail_out = (uInt)out.size();
    deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}
}  // namespace

// A UEF, plain or gzipped: carrier raises the ACIA's DCD, data bytes
// arrive one byte time apart while the motor runs
TEST_CASE(bbc_uef_playback) {
    std::vector<uint8_t> uef = {'U', 'E', 'F', ' ', 'F', 'i', 'l', 'e', '!', 0, 10, 0};
    auto chunk = [&](uint16_t id, std::vector<uint8_t> body) {
        const uint32_t n = (uint32_t)body.size();
        uint8_t h[6] = {(uint8_t)id, (uint8_t)(id >> 8), (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
        uef.insert(uef.end(), h, h + 6);
        uef.insert(uef.end(), body.begin(), body.end());
    };
    chunk(0x0000, {'h', 'i', 0});           // origin: skipped
    chunk(0x0110, {40, 0});                 // 40 carrier waves: 2 byte times
    chunk(0x0100, {0x12, 0x34});
    for (int gz = 0; gz < 2; gz++) {
        bbc::Acia6850 acia;
        bbc::BbcTape tape(&acia);
        ASSERT(tape.load(gz ? gzip(uef) : uef));
        acia.write(0, 0x83);                // receive interrupt on
        tape.clock(130 * 128 * 4);          // motor off: nothing moves
        EXPECT_EQ(tape.position(), (size_t)0);
        tape.serialUlaWrite(0x85);          // motor on, cassette
        tape.clock(130 * 128 * 2);          // carrier
        EXPECT_TRUE(acia.status() & bbc::Acia6850::DCD);
        std::vector<uint8_t> got;
        for (int i = 0; i < 8 && got.size() < 2; i++) {
            tape.clock(130 * 128);
            if (acia.status() & bbc::Acia6850::RDRF) got.push_back(acia.read(1));
        }
        ASSERT_EQ(got.size(), (size_t)2);
        EXPECT_EQ(got[0], 0x12);
        EXPECT_EQ(got[1], 0x34);
        EXPECT_TRUE(!(acia.status() & bbc::Acia6850::DCD));
        EXPECT_TRUE(tape.atEnd());
    }
}

// The cassette filing system both ways: SAVE onto a recording, saved as a
// UEF; a fresh machine CHAINs it back and runs it
TEST_CASE(bbc_tape_save_and_chain) {
    std::string err;
    MachineDescriptor* m = bbc::create(bbc::Model::B, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    const std::string path = "/tmp/mmemu_test_bbc_tape.uef";
    run(m, 4000000);
    auto* tape = find<bbc::BbcTape>(m);
    auto* kbd = find<IKeyboardMatrix>(m);
    ASSERT(tape && kbd);
    ASSERT(tape->startTapeRecord());
    kbd->enqueueText("*TAPE\n10 PRINT \"FROM TAPE\"\nSAVE \"PROG\"\n");
    ASSERT(runUntil(m, "RECORD then RETURN", 30000000));
    kbd->enqueueText("\n");
    // Wait for the prompt after the save (the cursor line ends with ">")
    uint64_t t = 0;
    for (; t < 80000000; t += 1000000) {
        run(m, 1000000);
        const std::string s = screen(m);
        const size_t at = s.find("RECORD then RETURN");
        if (at != std::string::npos && s.find('>', at) != std::string::npos) break;
    }
    tape->stopTapeRecord();
    ASSERT(tape->saveTapeRecording(path));
    ASSERT(tape->recording().size() > 60);   // carrier, the header and data block
    delete m;

    m = bbc::create(bbc::Model::B, &err);
    ASSERT(m);
    run(m, 4000000);
    tape = find<bbc::BbcTape>(m);
    kbd = find<IKeyboardMatrix>(m);
    ASSERT(tape->mountTape(path));
    kbd->enqueueText("*TAPE\nCHAIN \"\"\n");
    const bool ok = runUntil(m, "FROM TAPE", 120000000);
    if (!ok) printf("%s", screen(m).c_str());
    EXPECT_TRUE(ok);
    EXPECT_TRUE(screenHas(m, "PROG"));      // "Loading" shows the file name
    delete m;
    if (ok) std::remove(path.c_str());
}
