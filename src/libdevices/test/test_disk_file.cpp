// Disk images inside .gz and .zip files (#233)

#include "test_harness.h"
#include "libdevices/main/disk_file.h"
#include "plugins/cbm-loader/main/d64_parser.h"
#include "plugins/devices/ieee488/main/cbm_image_dos.h"
#include "plugins/machines/apple2/main/disk2.h"
#include "plugins/machines/bbc/main/bbc_disc.h"
#include "plugins/devices/vera/main/sdcard_spi.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::string tempDir() {
    char tmpl[] = "/tmp/mmemu-diskfile-XXXXXX";
    const char* d = mkdtemp(tmpl);
    return d ? std::string(d) : std::string("/tmp");
}

void writeBytes(const std::string& path, const std::vector<uint8_t>& b) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write((const char*)b.data(), (std::streamsize)b.size());
}

std::vector<uint8_t> readBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> pattern(size_t n, unsigned seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)((i * 7 + seed + (i >> 9)) & 0xFF);
    return v;
}

// A zip with two stored entries: a text file, then the disk image
std::vector<uint8_t> zipTwo(const std::string& name1, const std::vector<uint8_t>& d1,
                            const std::string& name2, const std::vector<uint8_t>& d2) {
    std::vector<uint8_t> z, cd;
    auto w16 = [](std::vector<uint8_t>& b, uint16_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); };
    auto w32 = [&](std::vector<uint8_t>& b, uint32_t v) { w16(b, (uint16_t)v); w16(b, (uint16_t)(v >> 16)); };
    uint16_t count = 0;
    for (auto e : {std::make_pair(name1, &d1), std::make_pair(name2, &d2)}) {
        const uint32_t at = (uint32_t)z.size();
        uint32_t crc = 0;
        {   // CRC-32 of the entry
            std::vector<uint8_t> tmp = *e.second;
            crc = 0xFFFFFFFFu;
            for (uint8_t b : tmp) {
                crc ^= b;
                for (int k = 0; k < 8; ++k) crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
            }
            crc ^= 0xFFFFFFFFu;
        }
        const uint32_t n = (uint32_t)e.second->size();
        w32(z, 0x04034b50); w16(z, 10); w16(z, 0); w16(z, 0); w16(z, 0); w16(z, 0);
        w32(z, crc); w32(z, n); w32(z, n); w16(z, (uint16_t)e.first.size()); w16(z, 0);
        z.insert(z.end(), e.first.begin(), e.first.end());
        z.insert(z.end(), e.second->begin(), e.second->end());
        w32(cd, 0x02014b50); w16(cd, 20); w16(cd, 10); w16(cd, 0); w16(cd, 0); w16(cd, 0); w16(cd, 0);
        w32(cd, crc); w32(cd, n); w32(cd, n); w16(cd, (uint16_t)e.first.size());
        w16(cd, 0); w16(cd, 0); w16(cd, 0); w16(cd, 0); w32(cd, 0); w32(cd, at);
        cd.insert(cd.end(), e.first.begin(), e.first.end());
        ++count;
    }
    const uint32_t cdAt = (uint32_t)z.size();
    z.insert(z.end(), cd.begin(), cd.end());
    w32(z, 0x06054b50); w16(z, 0); w16(z, 0); w16(z, count); w16(z, count);
    w32(z, (uint32_t)cd.size()); w32(z, cdAt); w16(z, 0);
    return z;
}

}  // namespace

TEST_CASE(disk_file_gzip_zip_round_trips) {
    const auto img = pattern(174848, 3);
    std::vector<uint8_t> out;
    ASSERT(diskfile::gunzip(diskfile::gzip(img, "x.d64"), out));
    ASSERT(out == img);

    const std::string dir = tempDir();
    writeBytes(dir + "/a.zip", diskfile::zipOne("GAME.D64", img));
    ASSERT(diskfile::kindOf(dir + "/a.zip") == diskfile::Kind::Zip);
    ASSERT_EQ(diskfile::imageName(dir + "/a.zip"), std::string("GAME.D64"));
    ASSERT_EQ(diskfile::imageExt(dir + "/a.zip"), std::string("d64"));
    out.clear();
    ASSERT(diskfile::read(dir + "/a.zip", out));
    ASSERT(out == img);
    ASSERT(diskfile::writable(dir + "/a.zip"));

    // .gz takes the format from the inner name
    writeBytes(dir + "/b.dsk.gz", diskfile::gzip(img));
    ASSERT_EQ(diskfile::imageExt(dir + "/b.dsk.gz"), std::string("dsk"));
    ASSERT_EQ(diskfile::imageExt(dir + "/c.D81"), std::string("d81"));
    ASSERT(!diskfile::isArchive(dir + "/c.d81"));
    std::system(("rm -rf " + dir).c_str());
}

TEST_CASE(disk_file_multi_entry_zip_read_only) {
    const std::string dir = tempDir();
    const std::string readme = "Read me first\n";
    const auto img = pattern(143360, 9);
    writeBytes(dir + "/two.zip", zipTwo("README.TXT", std::vector<uint8_t>(readme.begin(), readme.end()),
                                        "disks/Game.DSK", img));
    ASSERT_EQ(diskfile::imageExt(dir + "/two.zip"), std::string("dsk"));
    std::vector<uint8_t> out;
    ASSERT(diskfile::read(dir + "/two.zip", out));
    ASSERT(out == img);
    ASSERT(!diskfile::writable(dir + "/two.zip"));
    ASSERT(!diskfile::save(dir + "/two.zip", out));          // left as it was
    ASSERT(diskfile::read(dir + "/two.zip", out));
    ASSERT(out == img);

    // An Apple II disk from it opens write-protected
    auto disk = apple2::Disk2Image::open(dir + "/two.zip");
    ASSERT(disk != nullptr);
    ASSERT(disk->writeProtected());
    std::system(("rm -rf " + dir).c_str());
}

TEST_CASE(disk_file_read_only_archive) {
    const std::string dir = tempDir();
    writeBytes(dir + "/r.d64.gz", diskfile::gzip(pattern(174848, 1)));
    chmod((dir + "/r.d64.gz").c_str(), 0444);
    if (access((dir + "/r.d64.gz").c_str(), W_OK) != 0) {   // not when running as root
        ASSERT(!diskfile::writable(dir + "/r.d64.gz"));
        D64Parser d64;
        ASSERT(d64.open(dir + "/r.d64.gz"));
        uint8_t sec[256] = {1, 2, 3};
        ASSERT(!d64.writeSector(18, 1, sec));
    }
    std::system(("rm -rf " + dir).c_str());
}

TEST_CASE(disk_file_d64_gz_zip_write) {
    const std::string dir = tempDir();
    const auto img = pattern(174848, 5);
    writeBytes(dir + "/g.d64.gz", diskfile::gzip(img, "g.d64"));
    writeBytes(dir + "/z.zip", diskfile::zipOne("z.d64", img));
    uint8_t sec[256];
    for (int i = 0; i < 256; ++i) sec[i] = (uint8_t)(0xA5 ^ i);
    for (const std::string name : {"g.d64.gz", "z.zip"}) {
        const std::string path = dir + "/" + name;
        {
            CbmImageDos dos;                                   // the IEC / IEEE drives' path
            ASSERT(dos.open(path));
        }
        D64Parser d64;
        ASSERT(d64.open(path));
        ASSERT(d64.writable());
        ASSERT(d64.writeSector(17, 3, sec));
        D64Parser again;
        ASSERT(again.open(path));                             // remounted
        const uint32_t at = again.sectorOffset(17, 3);
        ASSERT(std::equal(sec, sec + 256, again.data().begin() + at));
        ASSERT(again.data()[at + 256] == img[at + 256]);      // the rest as it was
        ASSERT(diskfile::imageExt(path) == "d64");            // still the same kind of archive
    }
    ASSERT(readBytes(dir + "/g.d64.gz")[0] == 0x1F);
    ASSERT(readBytes(dir + "/z.zip")[0] == 'P');
    std::system(("rm -rf " + dir).c_str());
}

TEST_CASE(disk_file_apple2_bbc_write) {
    const std::string dir = tempDir();
    // Apple II: a nibble track written back
    writeBytes(dir + "/a.zip", diskfile::zipOne("a.nib", pattern(35 * apple2::Disk2Image::NIB_TRACK, 2)));
    {
        auto d = apple2::Disk2Image::open(dir + "/a.zip");
        ASSERT(d != nullptr);
        ASSERT(!d->writeProtected());
        std::vector<uint8_t> nib(apple2::Disk2Image::NIB_TRACK, 0x96);
        d->writeTrack(4, nib.data(), (int)nib.size());
    }
    {
        auto d = apple2::Disk2Image::open(dir + "/a.zip");
        ASSERT(d != nullptr);
        ASSERT_EQ((int)d->data()[4 * apple2::Disk2Image::NIB_TRACK + 100], 0x96);
        ASSERT_EQ((int)d->data()[3 * apple2::Disk2Image::NIB_TRACK + 100], (int)pattern(35 * apple2::Disk2Image::NIB_TRACK, 2)[3 * apple2::Disk2Image::NIB_TRACK + 100]);
    }
    // BBC: a byte changed and flushed
    writeBytes(dir + "/b.ssd.gz", diskfile::gzip(pattern(200 * 1024, 4)));
    {
        auto d = bbc::DfsImage::open(dir + "/b.ssd.gz");
        ASSERT(d != nullptr);
        d->write(0x1234, 0x5A);
        d->flush();
    }
    {
        auto d = bbc::DfsImage::open(dir + "/b.ssd.gz");
        ASSERT(d != nullptr);
        ASSERT_EQ((int)d->read(0x1234), 0x5A);
    }
    std::system(("rm -rf " + dir).c_str());
}

TEST_CASE(disk_file_sdcard_zip_detach) {
    const std::string dir = tempDir();
    writeBytes(dir + "/sd.zip", diskfile::zipOne("sd.img", std::vector<uint8_t>(1 << 20, 0)));
    SdCardSpi sd;
    ASSERT(sd.attach(dir + "/sd.zip"));
    ASSERT(!sd.readOnly());
    sd.select(true);
    auto cmd = [&](uint8_t c, uint32_t arg) {
        const uint8_t b[6] = {(uint8_t)(0x40 | c), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16),
                              (uint8_t)(arg >> 8), (uint8_t)arg, 0x95};
        for (uint8_t x : b) sd.handle(x);
        for (int i = 0; i < 4; ++i) sd.handle(0xFF);          // the response
    };
    cmd(24, 5);                                               // WRITE_BLOCK at LBA 5
    sd.handle(0xFE);
    for (int i = 0; i < 512; ++i) sd.handle((uint8_t)(i ^ 0x3C));
    sd.handle(0xFF);
    sd.handle(0xFF);
    sd.select(false);
    sd.detach();
    std::vector<uint8_t> out;
    ASSERT(diskfile::read(dir + "/sd.zip", out));
    ASSERT_EQ((int)out[5 * 512 + 0], 0x3C);
    ASSERT_EQ((int)out[5 * 512 + 511], (511 ^ 0x3C) & 0xFF);
    ASSERT_EQ((int)out[4 * 512 + 7], 0);
    std::system(("rm -rf " + dir).c_str());
}
