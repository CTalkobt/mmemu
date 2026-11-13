#include "test_harness.h"
#include "plugins/devices/wd17xx/main/floppy_disk.h"
#include "plugins/devices/wd17xx/main/wd17xx.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unistd.h>

// Disk images: each format built by hand, loaded, read, written, saved and reloaded

namespace {

uint8_t pat(int cyl, int head, int sec, int i) { return (uint8_t)(cyl * 5 + head * 11 + sec * 17 + i); }

std::string tmpPath(const std::string& name) {
    return (std::filesystem::temp_directory_path() /
            ("mmemu_wd17xx_" + std::to_string(::getpid()) + "_" + name)).string();
}

void writeFile(const std::string& path, const std::vector<uint8_t>& b) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write((const char*)b.data(), (std::streamsize)b.size());
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/** Raw sector bytes in file order: cyl, head, sector. */
std::vector<uint8_t> sectorBytes(int cyls, int heads, int spt, int first, int size = 256) {
    std::vector<uint8_t> b;
    for (int c = 0; c < cyls; ++c)
        for (int h = 0; h < heads; ++h)
            for (int s = 0; s < spt; ++s)
                for (int i = 0; i < size; ++i) b.push_back(pat(c, h, first + s, i));
    return b;
}

std::vector<uint8_t> filled(uint8_t v, int n = 256) { return std::vector<uint8_t>(n, v); }

} // namespace

// CRC-16/CCITT: the $A1 $A1 $A1 $FE ID of track 0 sector 1 as on any MFM disk
TEST_CASE(floppy_crc) {
    uint16_t c = floppyCrcAfterA1();
    ASSERT_EQ(c, 0xCDB4);
    const uint8_t id[] = {0xFE, 0, 0, 1, 1};
    for (uint8_t b : id) c = floppyCrc(c, b);
    ASSERT_EQ(c, 0xFA0C);
}

// JVC: headerless 35 x 18 x 256, single-sided
TEST_CASE(floppy_jvc_roundtrip) {
    auto raw = sectorBytes(35, 1, 18, 1);
    ASSERT(FloppyDisk::detect(raw, "disk.dsk") == FloppyFormat::JVC);
    std::string err;
    auto d = FloppyDisk::fromBytes(raw, FloppyFormat::Auto, &err, "disk.dsk");
    ASSERT(d);
    ASSERT_EQ(d->cylinders(), 35);
    ASSERT_EQ(d->heads(), 1);
    std::vector<uint8_t> s;
    ASSERT(d->readSector(17, 0, 3, s));
    ASSERT_EQ(s[10], pat(17, 0, 3, 10));
    for (auto& sec : d->scanTrack(0, 0)) ASSERT(sec.dd && sec.idCrcOk && sec.dataCrcOk);
    ASSERT(d->toBytes(FloppyFormat::JVC) == raw);
    ASSERT(d->writeSector(17, 0, 3, filled(0xAA)));
    auto out = d->toBytes(FloppyFormat::JVC);
    size_t off = (17 * 18 + 2) * 256;
    for (int i = 0; i < 256; ++i) raw[off + i] = 0xAA;
    ASSERT(out == raw);
    // Through a file
    std::string path = tmpPath("a.dsk");
    writeFile(path, sectorBytes(35, 1, 18, 1));
    auto f = FloppyDisk::load(path, &err);
    ASSERT(f);
    ASSERT(f->format() == FloppyFormat::JVC);
    ASSERT(f->writeSector(1, 0, 1, filled(0x55)));
    ASSERT(f->save(&err));
    auto g = FloppyDisk::load(path, &err);
    ASSERT(g->readSector(1, 0, 1, s));
    ASSERT(s == filled(0x55));
    ASSERT(g->readSector(1, 0, 2, s));
    ASSERT_EQ(s[0], pat(1, 0, 2, 0));
    std::remove(path.c_str());
}

// JVC with a header: 2 sides, 10 sectors from 0, kept on save
TEST_CASE(floppy_jvc_header) {
    std::vector<uint8_t> raw = {10, 2, 1, 0};
    auto body = sectorBytes(40, 2, 10, 0);
    raw.insert(raw.end(), body.begin(), body.end());
    std::string err;
    auto d = FloppyDisk::fromBytes(raw, FloppyFormat::JVC, &err);
    ASSERT(d);
    ASSERT_EQ(d->heads(), 2);
    ASSERT_EQ(d->cylinders(), 40);
    std::vector<uint8_t> s;
    ASSERT(d->readSector(39, 1, 9, s));
    ASSERT_EQ(s[200], pat(39, 1, 9, 200));
    ASSERT(!d->readSector(0, 0, 10, s));
    ASSERT(d->toBytes(FloppyFormat::JVC) == raw);
    // A headerless single-sided image of 80 tracks stays one side; 90 tracks is two
    auto big = sectorBytes(90, 1, 18, 1);
    auto e = FloppyDisk::fromBytes(big, FloppyFormat::JVC, &err);
    ASSERT_EQ(e->heads(), 2);
    ASSERT_EQ(e->cylinders(), 45);
}

// VDK: the "dk" header, cylinders, heads, write protect
TEST_CASE(floppy_vdk_roundtrip) {
    std::vector<uint8_t> raw = {'d', 'k', 12, 0, 0x10, 0x10, 'X', 0, 40, 2, 1, 0};
    auto body = sectorBytes(40, 2, 18, 1);
    raw.insert(raw.end(), body.begin(), body.end());
    ASSERT(FloppyDisk::detect(raw, "game.dsk") == FloppyFormat::VDK);
    std::string err;
    auto d = FloppyDisk::fromBytes(raw, FloppyFormat::Auto, &err, "game.vdk");
    ASSERT(d);
    ASSERT(d->writeProtected());
    ASSERT_EQ(d->cylinders(), 40);
    ASSERT_EQ(d->heads(), 2);
    std::vector<uint8_t> s;
    ASSERT(d->readSector(20, 1, 18, s));
    ASSERT_EQ(s[255], pat(20, 1, 18, 255));
    ASSERT(d->toBytes(FloppyFormat::VDK) == raw);
    d->setWriteProtected(false);
    d->writeSector(0, 1, 1, filled(0x77));
    auto out = d->toBytes(FloppyFormat::VDK);
    ASSERT_EQ(out[10] & 1, 0);
    auto e = FloppyDisk::fromBytes(out, FloppyFormat::VDK, &err);
    ASSERT(e->readSector(0, 1, 1, s));
    ASSERT(s == filled(0x77));
}

// DMK: raw tracks with mixed density and their IDAM tables
TEST_CASE(floppy_dmk_roundtrip) {
    FloppyDisk d(3, 2);
    std::vector<FloppySectorSpec> fm, mfm;
    for (int s = 0; s < 10; ++s) {
        FloppySectorSpec sp;
        sp.track = 0; sp.sector = (uint8_t)s; sp.dd = false;
        sp.data = filled((uint8_t)s);
        if (s == 3) sp.dam = 0xFA;
        fm.push_back(sp);
    }
    for (int s = 1; s <= 18; ++s) {
        FloppySectorSpec sp;
        sp.track = 1; sp.sector = (uint8_t)s; sp.side = 1; sp.dd = true;
        sp.data = filled((uint8_t)(0x80 + s));
        if (s == 5) sp.dam = 0xF8;
        mfm.push_back(sp);
    }
    ASSERT(d.formatTrack(0, 0, fm));
    ASSERT(d.formatTrack(1, 1, mfm));
    std::string err;
    std::string path = tmpPath("b.dmk");
    ASSERT(d.saveAs(path, FloppyFormat::Auto, &err));
    auto bytes = readFile(path);
    ASSERT_EQ(bytes.size(), 16u + 3 * 2 * 0x1900);
    ASSERT(FloppyDisk::detect(bytes, "b.dsk") == FloppyFormat::DMK);
    auto e = FloppyDisk::load(path, &err);
    ASSERT(e);
    ASSERT(e->format() == FloppyFormat::DMK);
    auto a = e->scanTrack(0, 0);
    ASSERT_EQ(a.size(), 10u);
    ASSERT(!a[0].dd);
    ASSERT_EQ(a[3].dam, 0xFA);
    auto b = e->scanTrack(1, 1);
    ASSERT_EQ(b.size(), 18u);
    ASSERT(b[0].dd);
    ASSERT_EQ(b[4].dam, 0xF8);
    std::vector<uint8_t> s;
    ASSERT(e->readSector(1, 1, 7, s));
    ASSERT(s == filled(0x87));
    ASSERT(e->writeSector(0, 0, 9, filled(0x99)));
    ASSERT(e->save(&err));
    auto f = FloppyDisk::load(path, &err);
    ASSERT(f->readSector(0, 0, 9, s));
    ASSERT(s == filled(0x99));
    ASSERT(f->toBytes(FloppyFormat::DMK) == readFile(path));
    std::remove(path.c_str());
}

// DMK flag $40: single density, each byte stored once
TEST_CASE(floppy_dmk_single_density_flag) {
    FloppyDisk d(2, 1);
    std::vector<FloppySectorSpec> fm;
    for (int s = 0; s < 10; ++s) {
        FloppySectorSpec sp;
        sp.track = 1; sp.sector = (uint8_t)s; sp.dd = false; sp.data = filled((uint8_t)(s + 1));
        fm.push_back(sp);
    }
    ASSERT(d.formatTrack(1, 0, fm));
    fm.clear();
    for (int s = 0; s < 10; ++s) {
        FloppySectorSpec sp;
        sp.track = 0; sp.sector = (uint8_t)s; sp.dd = false; sp.data = filled((uint8_t)(s + 1));
        fm.push_back(sp);
    }
    ASSERT(d.formatTrack(0, 0, fm));
    auto full = d.toBytes(FloppyFormat::DMK);
    // Halve it by hand
    int tlen = full[2] | (full[3] << 8), half = (tlen - 128) / 2 + 128;
    std::vector<uint8_t> sd(full.begin(), full.begin() + 16);
    sd[2] = (uint8_t)half; sd[3] = (uint8_t)(half >> 8); sd[4] |= 0x40;
    for (int t = 0; t < 2; ++t) {
        size_t base = 16 + (size_t)t * tlen;
        for (int k = 0; k < 64; ++k) {
            int p = full[base + 2 * k] | (full[base + 2 * k + 1] << 8);
            if (p) p = ((p & 0x3FFF) - 128) / 2 + 128;
            sd.push_back((uint8_t)p);
            sd.push_back((uint8_t)(p >> 8));
        }
        for (int i = 0; i < (tlen - 128) / 2; ++i) sd.push_back(full[base + 128 + 2 * i]);
    }
    std::string err;
    auto e = FloppyDisk::fromBytes(sd, FloppyFormat::DMK, &err);
    ASSERT(e);
    std::vector<uint8_t> s;
    ASSERT(e->readSector(1, 0, 4, s));
    ASSERT(s == filled(5));
    ASSERT(e->toBytes(FloppyFormat::DMK) == sd);     // saved halved again
}

// JV1: TRS-80 Model I, 10 x 256 FM sectors from 0, the directory on track 17 marked $FA
TEST_CASE(floppy_jv1_roundtrip) {
    auto raw = sectorBytes(35, 1, 10, 0);
    ASSERT(FloppyDisk::detect(raw, "ldos.dsk") == FloppyFormat::JV1);
    std::string err;
    auto d = FloppyDisk::fromBytes(raw, FloppyFormat::Auto, &err, "ldos.dsk");
    ASSERT(d);
    ASSERT(d->format() == FloppyFormat::JV1);
    ASSERT_EQ(d->cylinders(), 35);
    auto t17 = d->scanTrack(17, 0);
    ASSERT_EQ(t17.size(), 10u);
    ASSERT_EQ(t17[0].dam, 0xFA);
    ASSERT(!t17[0].dd);
    ASSERT_EQ(d->scanTrack(16, 0)[0].dam, 0xFB);
    ASSERT(d->toBytes(FloppyFormat::JV1) == raw);
    ASSERT(d->writeSector(34, 0, 9, filled(0x3C)));
    auto out = d->toBytes(FloppyFormat::JV1);
    ASSERT(std::equal(out.end() - 256, out.end(), filled(0x3C).begin()));
}

// JV3: per-sector density, data mark, side, CRC error, size; headers kept on save
TEST_CASE(floppy_jv3_roundtrip) {
    std::vector<uint8_t> raw(2901 * 3 + 1, 0xFF);
    std::vector<uint8_t> data;
    int n = 0;
    auto add = [&](int t, int s, uint8_t flags, int size) {
        raw[n * 3] = (uint8_t)t; raw[n * 3 + 1] = (uint8_t)s; raw[n * 3 + 2] = flags;
        for (int i = 0; i < size; ++i) data.push_back(pat(t, (flags & 0x10) ? 1 : 0, s, i));
        ++n;
    };
    for (int s = 0; s < 10; ++s) add(0, s, s == 2 ? 0x20 : 0x00, 256);          // FM, sector 2 $FA
    for (int s = 0; s < 18; ++s) add(1, s, 0x80 | (s == 4 ? 0x20 : 0), 256);      // MFM, sector 4 $F8
    for (int s = 0; s < 18; ++s) add(1, s, 0x80 | 0x10, 256);                     // MFM side 1
    add(2, 1, 0x80 | 0x08 | 0x01, 128);                                           // a 128-byte sector, CRC error
    raw.insert(raw.end(), data.begin(), data.end());
    ASSERT(FloppyDisk::detect(raw, "nd80.dsk") == FloppyFormat::JV3);
    std::string err;
    auto d = FloppyDisk::fromBytes(raw, FloppyFormat::Auto, &err, "nd80.dsk");
    ASSERT(d);
    ASSERT(!d->writeProtected());
    ASSERT_EQ(d->cylinders(), 3);
    ASSERT_EQ(d->heads(), 2);
    auto t0 = d->scanTrack(0, 0);
    ASSERT_EQ(t0.size(), 10u);
    ASSERT(!t0[0].dd);
    ASSERT_EQ(t0[2].dam, 0xFA);
    auto t1 = d->scanTrack(1, 0);
    ASSERT(t1[0].dd);
    ASSERT_EQ(t1[4].dam, 0xF8);
    ASSERT_EQ(d->scanTrack(1, 1).size(), 18u);
    auto t2 = d->scanTrack(2, 0);
    ASSERT_EQ(t2.size(), 1u);
    ASSERT_EQ(t2[0].data.size(), 128u);
    ASSERT(!t2[0].dataCrcOk);
    std::vector<uint8_t> s;
    ASSERT(d->readSector(1, 1, 17, s));
    ASSERT_EQ(s[3], pat(1, 1, 17, 3));
    ASSERT(d->toBytes(FloppyFormat::JV3) == raw);
    // Written and saved through a file
    std::string path = tmpPath("c.jv3");
    writeFile(path, raw);
    auto f = FloppyDisk::load(path, &err);
    ASSERT(f->writeSector(0, 0, 7, filled(0x42)));
    ASSERT(f->save(&err));
    auto g = FloppyDisk::load(path, &err);
    ASSERT(g->readSector(0, 0, 7, s));
    ASSERT(s == filled(0x42));
    std::remove(path.c_str());
}

// The controller writes through to the image file
TEST_CASE(floppy_controller_writeback) {
    std::string path = tmpPath("d.dsk");
    writeFile(path, sectorBytes(35, 1, 18, 1));
    Wd17xx f(Wd17xx::Variant::WD2793);
    f.setClockHz(1000000);
    ASSERT(f.mountDisk(0, path));
    ASSERT_EQ(f.getMountedDiskPath(0), path);
    f.selectDrive(0);
    f.writeRegister(1, 0);
    f.writeRegister(2, 2);
    f.writeRegister(0, 0xA0);
    std::vector<uint8_t> data = filled(0xC3);
    size_t idx = 0;
    for (int t = 0; t < 1000000 && !f.intrq(); ++t) {
        if (f.drq() && idx < data.size()) f.writeRegister(3, data[idx++]);
        f.tick(1);
    }
    ASSERT(f.intrq());
    ASSERT(f.disk(0)->dirty());
    for (int t = 0; t < 600000; t += 100) f.tick(100);       // half a second idle: saved
    ASSERT(!f.disk(0)->dirty());
    auto raw = readFile(path);
    ASSERT_EQ(raw[256], 0xC3);
    ASSERT_EQ(raw[0], pat(0, 0, 1, 0));
    f.ejectDisk(0);
    ASSERT(f.disk(0) == nullptr);
    std::remove(path.c_str());
}

// Unformatted disks and the geometry limits
TEST_CASE(floppy_blank_and_extend) {
    FloppyDisk d(40, 1);
    ASSERT(d.scanTrack(0, 0).empty());
    ASSERT(d.track(40, 0) == nullptr);
    ASSERT(d.extend(41, 1) != nullptr);
    ASSERT_EQ(d.cylinders(), 42);
    ASSERT_EQ(d.heads(), 2);
    ASSERT(d.extend(200, 0) == nullptr);
    // 26 FM sectors of 128 don't fit 6272 slots with the full gaps: the track grows to take them
    std::vector<FloppySectorSpec> many;
    for (int s = 1; s <= 26; ++s) {
        FloppySectorSpec sp;
        sp.sector = (uint8_t)s; sp.sizeCode = 0; sp.dd = false;
        many.push_back(sp);
    }
    ASSERT(d.formatTrack(0, 0, many));
    ASSERT_EQ(d.scanTrack(0, 0).size(), 26u);
}
