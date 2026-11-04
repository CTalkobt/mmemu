#include "test_harness.h"
#include "plugins/devices/sdcard/main/f011.h"
#include "plugins/devices/sdcard/main/sdcard.h"
#include "plugins/devices/map_mmu/main/map_mmu.h"
#include "plugins/devices/map_mmu/main/c64_bank_controller.h"
#include "libmem/main/sparse_memory_bus.h"
#include "libdebug/main/execution_observer.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

// A D81 where each 512-byte physical sector starts with its (track, side, sector)
std::string makeD81(const char* tag) {
    std::string path = std::string("/tmp/mmemu_f011_") + tag + "_" + std::to_string(getpid()) + ".d81";
    std::vector<uint8_t> img(F011Fdc::D81_SIZE, 0);
    for (int t = 0; t < 80; t++)
        for (int side = 0; side < 2; side++)
            for (int s = 1; s <= 10; s++) {
                size_t off = (size_t)t * 10240 + side * 5120 + (s - 1) * 512;
                img[off] = (uint8_t)t; img[off + 1] = (uint8_t)side; img[off + 2] = (uint8_t)s;
                img[off + 511] = 0xA5;
            }
    FILE* f = fopen(path.c_str(), "wb");
    fwrite(img.data(), 1, img.size(), f);
    fclose(f);
    return path;
}

std::vector<uint8_t> readFile(const std::string& path, size_t off, size_t n) {
    std::vector<uint8_t> b(n);
    FILE* f = fopen(path.c_str(), "rb");
    fseek(f, (long)off, SEEK_SET);
    size_t got = fread(b.data(), 1, n, f);
    fclose(f);
    b.resize(got);
    return b;
}

uint8_t rd(F011Fdc& f, uint32_t a) { uint8_t v = 0; f.ioRead(nullptr, a, &v); return v; }
void    wr(F011Fdc& f, uint32_t a, uint8_t v) { f.ioWrite(nullptr, a, v); }

// The C65 DOS sequence: reset buffer pointers, set track/sector/side, read
void readSector(F011Fdc& f, int track, int side, int sector) {
    wr(f, 0xD081, 0x01);                 // reset buffer pointers
    wr(f, 0xD084, (uint8_t)track);
    wr(f, 0xD085, (uint8_t)sector);
    wr(f, 0xD086, (uint8_t)side);
    wr(f, 0xD081, 0x40);                 // read sector
}

} // namespace

TEST_CASE(f011_reads_host_d81_into_buffer) {
    std::string path = makeD81("rd");
    SdCardDevice sd;
    sd.reset();
    F011Fdc fdc(&sd);
    ASSERT_EQ((int)(rd(fdc, 0xD083) & 0x08), 0);          // no disk
    ASSERT(fdc.mountDisk(8, path));
    ASSERT_EQ((int)(rd(fdc, 0xD083) & 0x08), 0x08);       // DSKIN
    ASSERT_EQ((int)(rd(fdc, 0xD082) & 0x02), 0);          // not write protected

    readSector(fdc, 39, 1, 3);
    ASSERT_EQ((int)(rd(fdc, 0xD082) & 0x90), 0);          // not BUSY, no RNF
    ASSERT_EQ((int)(rd(fdc, 0xD083) & 0x80), 0x80);       // RDREQ
    const uint8_t* buf = sd.fdcBuffer();
    ASSERT_EQ((int)buf[0], 39);
    ASSERT_EQ((int)buf[1], 1);
    ASSERT_EQ((int)buf[2], 3);
    ASSERT_EQ((int)buf[511], 0xA5);
    // The data register reads the same bytes from the CPU pointer
    ASSERT_EQ((int)rd(fdc, 0xD087), 39);
    ASSERT_EQ((int)rd(fdc, 0xD087), 1);
    unlink(path.c_str());
}

TEST_CASE(f011_record_not_found_without_disk_or_bad_sector) {
    SdCardDevice sd;
    sd.reset();
    F011Fdc fdc(&sd);
    readSector(fdc, 0, 0, 1);
    ASSERT_EQ((int)(rd(fdc, 0xD082) & 0x10), 0x10);       // RNF: no disk

    std::string path = makeD81("rnf");
    ASSERT(fdc.mountDisk(8, path));
    readSector(fdc, 0, 0, 0);                              // sectors start at 1
    ASSERT_EQ((int)(rd(fdc, 0xD082) & 0x10), 0x10);
    readSector(fdc, 0, 0, 1);
    ASSERT_EQ((int)(rd(fdc, 0xD082) & 0x10), 0);
    unlink(path.c_str());
}

TEST_CASE(f011_write_second_half_with_swap) {
    std::string path = makeD81("wr");
    SdCardDevice sd;
    sd.reset();
    F011Fdc fdc(&sd);
    ASSERT(fdc.mountDisk(8, path));
    // Read-modify-write of the second 256-byte half, as WriteSector does
    readSector(fdc, 10, 0, 4);
    wr(fdc, 0xD080, 0x10);                                 // SWAP: CPU side addresses the 2nd half
    for (int i = 0; i < 256; i++) wr(fdc, 0xD087, (uint8_t)(0x40 + (i & 0x3F)));
    wr(fdc, 0xD080, 0x00);
    wr(fdc, 0xD081, 0x01);                                 // reset pointers
    wr(fdc, 0xD081, 0x80);                                 // write sector
    ASSERT_EQ((int)(rd(fdc, 0xD082) & 0x10), 0);
    auto b = readFile(path, 10 * 10240 + 3 * 512, 512);
    ASSERT_EQ((int)b[0], 10);                              // first half kept
    ASSERT_EQ((int)b[2], 4);
    ASSERT_EQ((int)b[256], 0x40);                          // second half written
    ASSERT_EQ((int)b[511], 0x40 + 63);
    unlink(path.c_str());
}

TEST_CASE(f011_disk_change_on_swap_and_eject) {
    std::string a = makeD81("dca"), b = makeD81("dcb");
    SdCardDevice sd;
    sd.reset();
    F011Fdc fdc(&sd);
    ASSERT(fdc.mountDisk(8, a));
    ASSERT_EQ((int)(rd(fdc, 0xD083) & 0x01), 0x01);       // DSKCHG
    wr(fdc, 0xD080, 0x01);                                 // select drive 1 ...
    wr(fdc, 0xD080, 0x00);                                 // ... and back: clears drive 0's DSKCHG
    ASSERT_EQ((int)(rd(fdc, 0xD083) & 0x01), 0);
    ASSERT(fdc.mountDisk(8, b));                           // another writable disk: still a change
    ASSERT_EQ((int)(rd(fdc, 0xD083) & 0x01), 0x01);
    fdc.ejectDisk(8);
    ASSERT_EQ((int)(rd(fdc, 0xD083) & 0x08), 0);
    ASSERT(!fdc.mountDisk(10, a));                         // only units 8 and 9
    unlink(a.c_str());
    unlink(b.c_str());
}

// The D81 HYPPO mounts inside the SD card image: $D68B flags, start sector at
// $D68C; bit 2 means writable (sdcardio.vhdl reads "not write protected").
TEST_CASE(f011_reads_d81_inside_sd_image) {
    std::string d81 = makeD81("sdsrc");
    std::string img = std::string("/tmp/mmemu_f011_sdimg_") + std::to_string(getpid()) + ".img";
    const uint32_t startSector = 100;
    {
        std::vector<uint8_t> pre(startSector * 512, 0xEE);
        auto disk = readFile(d81, 0, F011Fdc::D81_SIZE);
        FILE* f = fopen(img.c_str(), "wb");
        fwrite(pre.data(), 1, pre.size(), f);
        fwrite(disk.data(), 1, disk.size(), f);
        fclose(f);
    }
    SdCardDevice sd;
    sd.reset();
    bool hyper = true;
    sd.setHypervisorQuery([&hyper]() { return hyper; });
    ASSERT(sd.mountImage(img));
    F011Fdc fdc(&sd);
    sd.ioWrite(nullptr, 0xD68C, startSector & 0xFF);
    sd.ioWrite(nullptr, 0xD68D, startSector >> 8);
    sd.ioWrite(nullptr, 0xD68B, 0x03);                     // image enabled + present, not writable
    ASSERT_EQ((int)(rd(fdc, 0xD083) & 0x08), 0x08);
    ASSERT_EQ((int)(rd(fdc, 0xD082) & 0x02), 0x02);       // write protected
    readSector(fdc, 5, 1, 7);
    ASSERT_EQ((int)sd.fdcBuffer()[0], 5);
    ASSERT_EQ((int)sd.fdcBuffer()[1], 1);
    ASSERT_EQ((int)sd.fdcBuffer()[2], 7);
    wr(fdc, 0xD081, 0x80);                                 // write refused
    ASSERT_EQ((int)(rd(fdc, 0xD082) & 0x10), 0);          // (no command run: write protected)

    sd.ioWrite(nullptr, 0xD68B, 0x07);                     // + writable
    ASSERT_EQ((int)(rd(fdc, 0xD082) & 0x02), 0);

    // Outside the hypervisor the mount registers do not change
    hyper = false;
    sd.ioWrite(nullptr, 0xD68B, 0x00);
    uint8_t v = 0;
    sd.ioRead(nullptr, 0xD68B, &v);
    ASSERT_EQ((int)v, 0x07);
    ASSERT_EQ((int)(rd(fdc, 0xD083) & 0x08), 0x08);
    unlink(d81.c_str());
    unlink(img.c_str());
}

TEST_CASE(f011_de00_window_follows_bufsel_outside_hypervisor) {
    SdCardDevice sd;
    sd.reset();
    bool hyper = false;
    sd.setHypervisorQuery([&hyper]() { return hyper; });
    sd.fdcBuffer()[5] = 0x11;
    sd.sectorBuffer()[5] = 0x22;
    sd.ioWrite(nullptr, 0xD680, 0x81);                     // map the buffer at $DE00
    uint8_t v = 0;
    sd.ioWrite(nullptr, 0xD689, 0x00);                     // BUFSEL = F011
    sd.ioRead(nullptr, 0xDE05, &v);
    ASSERT_EQ((int)v, 0x11);
    sd.ioWrite(nullptr, 0xD689, 0x80);                     // BUFSEL = SD card
    sd.ioRead(nullptr, 0xDE05, &v);
    ASSERT_EQ((int)v, 0x22);
    sd.ioWrite(nullptr, 0xD689, 0x00);
    hyper = true;                                          // the hypervisor always sees the SD buffer
    sd.ioRead(nullptr, 0xDE05, &v);
    ASSERT_EQ((int)v, 0x22);
}

namespace {
struct WriteWatcher : ExecutionObserver {
    int writes = 0;
    void onMemoryWrite(IBus*, uint32_t, uint8_t, uint8_t) override { writes++; }
};
}

// With an observer (the debugger) attached, a write to an I/O register must
// not also read it: reading $D087 advances the F011 buffer pointer.
TEST_CASE(map_mmu_io_write_does_not_read_with_observer) {
    SparseMemoryBus phys("phys", 28);
    MapMmu mmu("mmu", &phys);
    int ioReads = 0, ioWrites = 0;
    mmu.setIoHooks(
        [&](IBus*, uint32_t a, uint8_t* v) { if (a != 0xD087) return false; ioReads++; *v = 0; return true; },
        [&](IBus*, uint32_t a, uint8_t)    { if (a != 0xD087) return false; ioWrites++; return true; });
    WriteWatcher w;
    mmu.setObserver(&w);
    mmu.write8(0xD087, 0x55);
    ASSERT_EQ(ioWrites, 1);
    ASSERT_EQ(ioReads, 0);
    ASSERT_EQ(w.writes, 1);
    mmu.write8(0x1234, 0x66);                              // RAM write still recorded
    ASSERT_EQ((int)phys.peek8(0x1234), 0x66);
    ASSERT_EQ(w.writes, 2);
    mmu.setObserver(nullptr);
}

// gs4510.vhdl: the CPU port is at $00/$01 only while block 0 is not MAP'd
// (the C65 DOS maps its RAM there and keeps a pointer at $00/$01).
TEST_CASE(bank_ctrl_port_hidden_when_block0_mapped) {
    SparseMemoryBus phys("phys", 28);
    MapMmu mmu("mmu", &phys);
    C64BankController bc(&phys);
    bc.setMapMmu(&mmu);
    bc.reset();
    uint8_t v = 0;
    ASSERT(bc.ioRead(nullptr, 0x0001, &v));
    MapState ms = {};
    ms.enables = 0x01;
    ms.offsets[0] = 0x10000;
    mmu.setMapState(ms);
    ASSERT(!bc.ioRead(nullptr, 0x0000, &v));
    ASSERT(!bc.ioRead(nullptr, 0x0001, &v));
    ASSERT(!bc.ioWrite(nullptr, 0x0001, 0x00));
    ASSERT(bc.ioRead(nullptr, 0x0002, &v) == false);
}
