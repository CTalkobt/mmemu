#include "test_harness.h"
#include "plugins/devices/xt_hdc/main/xt_hdc.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

// The XT hard disk controller driven through its four ports as a host's
// driver does: select, a six-byte command block, data, the completion byte.

namespace {

struct Image {
    std::string path;
    explicit Image(size_t bytes) {
        char t[] = "/tmp/xt_hdc_XXXXXX";
        int fd = mkstemp(t);
        close(fd);
        path = t;
        std::vector<uint8_t> img(bytes);
        for (size_t i = 0; i < bytes; ++i) img[i] = (uint8_t)(i / 512 + i * 7);
        std::ofstream f(path, std::ios::binary);
        f.write((const char*)img.data(), (std::streamsize)img.size());
    }
    ~Image() { std::remove(path.c_str()); }
};

uint8_t expected(size_t lba, size_t i) { return (uint8_t)(lba + (lba * 512 + i) * 7); }

void command(XtHdc& h, std::initializer_list<int> dcb) {
    h.write(2, 0);                                     // select
    ASSERT_EQ(h.read(1) & 0x0F, XtHdc::ST_BUSY | XtHdc::ST_CD | XtHdc::ST_REQ);
    for (int b : dcb) h.write(0, (uint8_t)b);
}

uint8_t completion(XtHdc& h) {
    ASSERT_EQ(h.read(1) & 0x0F, XtHdc::ST_BUSY | XtHdc::ST_CD | XtHdc::ST_IO | XtHdc::ST_REQ);
    const uint8_t c = h.read(0);
    ASSERT_EQ(h.read(1) & 0x0F, 0);
    return c;
}

}  // namespace

// Read two sectors across a track boundary; the default geometry is 4 heads, 17 sectors
TEST_CASE(xt_hdc_read_sectors) {
    Image img(4 * 17 * 512 * 3);          // 3 cylinders
    XtHdc h;
    ASSERT(h.mount(0, img.path));
    ASSERT_EQ(h.cylinders(0), 3);
    ASSERT_EQ(h.heads(0), 4);
    h.write(1, 0);                         // reset
    ASSERT_EQ(h.read(1), 0);
    command(h, {0x00, 0x00, 0, 0, 0, 0});  // test drive ready
    ASSERT_EQ(completion(h), 0x00);
    // Read cylinder 1, head 2, sector 16 (0-based) and the next (head 3, sector 0)
    command(h, {0x08, 0x02, 16, 1, 2, 0});
    for (int s = 0; s < 2; ++s) {
        const size_t lba = (1 * 4 + 2) * 17 + 16 + s;
        for (int i = 0; i < 512; ++i) {
            ASSERT_EQ(h.read(1) & 0x0F, XtHdc::ST_BUSY | XtHdc::ST_IO | XtHdc::ST_REQ);
            ASSERT_EQ(h.read(0), expected(lba, i));
        }
    }
    ASSERT_EQ(completion(h), 0x00);
}

// Write a sector and read it back, from the file too
TEST_CASE(xt_hdc_write_sector) {
    Image img(4 * 17 * 512 * 2);
    XtHdc h;
    ASSERT(h.mount(0, img.path));
    command(h, {0x0A, 0x01, 3, 0, 1, 0});   // head 1, sector 3
    for (int i = 0; i < 512; ++i) {
        ASSERT_EQ(h.read(1) & 0x0F, XtHdc::ST_BUSY | XtHdc::ST_REQ);
        h.write(0, (uint8_t)(i ^ 0xA5));
    }
    ASSERT_EQ(completion(h), 0x00);
    std::ifstream f(img.path, std::ios::binary);
    f.seekg((17 + 3) * 512);
    std::vector<uint8_t> got(512);
    f.read((char*)got.data(), 512);
    for (int i = 0; i < 512; ++i) ASSERT_EQ(got[i], (uint8_t)(i ^ 0xA5));
}

// Initialize drive characteristics sets the geometry; a bad address fails and request sense says why
TEST_CASE(xt_hdc_geometry_and_sense) {
    Image img(2 * 17 * 512 * 10);
    XtHdc h;
    ASSERT(h.mount(0, img.path));
    command(h, {0x0C, 0x00, 0, 0, 0, 0});
    const uint8_t params[8] = {0x00, 10, 2, 0, 10, 0, 10, 11};   // 10 cylinders, 2 heads
    for (uint8_t p : params) h.write(0, p);
    ASSERT_EQ(completion(h), 0x00);
    ASSERT_EQ(h.cylinders(0), 10);
    ASSERT_EQ(h.heads(0), 2);
    // Cylinder 3 head 1 sector 0 is LBA (3*2+1)*17
    command(h, {0x08, 0x01, 0, 3, 1, 0});
    const size_t lba = (3 * 2 + 1) * 17;
    for (int i = 0; i < 512; ++i) ASSERT_EQ(h.read(0), expected(lba, i));
    ASSERT_EQ(completion(h), 0x00);
    // Head 2 doesn't exist now
    command(h, {0x08, 0x02, 0, 0, 1, 0});
    ASSERT_EQ(completion(h), 0x02);
    command(h, {0x03, 0x00, 0, 0, 0, 0});
    uint8_t sense[4];
    for (auto& s : sense) s = h.read(0);
    ASSERT_EQ(sense[0], 0x80 | XtHdc::ERR_ILLEGAL_ADDRESS);
    ASSERT_EQ(sense[1], 0x02);
    ASSERT_EQ(completion(h), 0x00);
    // Drive 1 has no image: not ready, completion carries the drive
    command(h, {0x00, 0x20, 0, 0, 0, 0});
    ASSERT_EQ(completion(h), 0x22);
    command(h, {0x03, 0x20, 0, 0, 0, 0});
    ASSERT_EQ(h.read(0), 0x80 | XtHdc::ERR_NOT_READY);
    // An unknown command
    h.write(1, 0);
    command(h, {0x55, 0x00, 0, 0, 0, 0});
    ASSERT_EQ(completion(h), 0x02);
}

// Interrupts at completion once enabled in the mask; the sector buffer commands
TEST_CASE(xt_hdc_interrupt_and_buffer) {
    Image img(4 * 17 * 512);
    XtHdc h;
    ASSERT(h.mount(0, img.path));
    h.write(3, 0x02);
    command(h, {0x0F, 0x00, 0, 0, 0, 0});
    for (int i = 0; i < 512; ++i) h.write(0, (uint8_t)i);
    ASSERT(h.irq());
    ASSERT(h.read(1) & XtHdc::ST_INT);
    ASSERT_EQ(h.read(0), 0x00);
    ASSERT(!h.irq());
    command(h, {0x0E, 0x00, 0, 0, 0, 0});
    for (int i = 0; i < 512; ++i) ASSERT_EQ(h.read(0), (uint8_t)i);
    ASSERT(h.irq());
    ASSERT_EQ(completion(h), 0x00);
}
