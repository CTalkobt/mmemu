#include "test_harness.h"
#include "libdevices/main/io_registry.h"
#include "libmem/main/memory_bus.h"
#include "libdebug/main/execution_observer.h"
#include "plugins/devices/cia6526/main/cia6526.h"
#include "plugins/devices/via6522/main/via6522.h"
#include "plugins/devices/pia6520/main/pia6520.h"
#include "plugins/devices/vic2/main/vic2.h"
#include "plugins/devices/mega65_math/main/mega65_math.h"
#include "plugins/devices/sdcard/main/f011.h"
#include "plugins/devices/sdcard/main/sdcard.h"
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

// Peeks (debugger views, memory dumps, a write's "before" value) must not
// change a device: the same value as a read, and no flag, latch, pointer or
// generator advanced.

namespace {
uint8_t peek(IOHandler& h, uint32_t a) { uint8_t v = 0; ASSERT(h.ioPeek(nullptr, a, &v)); return v; }
uint8_t read(IOHandler& h, uint32_t a) { uint8_t v = 0; ASSERT(h.ioRead(nullptr, a, &v)); return v; }
}

TEST_CASE(io_peek_cia_icr_and_tod_unchanged) {
    CIA6526 cia{"CIA1", 0xDC00};
    cia.reset();
    cia.ioWrite(nullptr, 0xDC0D, 0x81);        // enable timer A interrupt
    cia.ioWrite(nullptr, 0xDC04, 0x02);
    cia.ioWrite(nullptr, 0xDC05, 0x00);
    cia.ioWrite(nullptr, 0xDC0E, 0x11);        // force load + start
    cia.tick(10);
    uint8_t icr = peek(cia, 0xDC0D);
    ASSERT_EQ((int)(icr & 0x81), 0x81);
    ASSERT_EQ((int)peek(cia, 0xDC0D), (int)icr);   // still pending
    ASSERT_EQ((int)read(cia, 0xDC0D), (int)icr);   // a read returns it ...
    ASSERT_EQ((int)(peek(cia, 0xDC0D) & 0x01), 0); // ... and clears it

    // A peek of TODHR does not latch the clock (TODTEN would read the latch)
    uint8_t hr = peek(cia, 0xDC0B);
    ASSERT_EQ((int)peek(cia, 0xDC0B), (int)hr);
}

TEST_CASE(io_peek_via_timer_flag_unchanged) {
    VIA6522 via{"VIA1", 0x9110};
    via.reset();
    via.ioWrite(nullptr, 0x911E, 0xC0);        // enable T1 interrupt
    via.ioWrite(nullptr, 0x9114, 0x02);
    via.ioWrite(nullptr, 0x9115, 0x00);        // start T1
    via.tick(10);
    ASSERT_EQ((int)(peek(via, 0x911D) & 0x40), 0x40);
    uint8_t t1 = peek(via, 0x9114);            // peek T1CL: IFR bit 6 stays
    ASSERT_EQ((int)(peek(via, 0x911D) & 0x40), 0x40);
    ASSERT_EQ((int)read(via, 0x9114), (int)t1);
    ASSERT_EQ((int)(peek(via, 0x911D) & 0x40), 0);   // the read clears it
}

TEST_CASE(io_peek_pia_ca1_flag_unchanged) {
    PIA6520 pia{"PIA", 0xD300};
    pia.reset();
    pia.ioWrite(nullptr, 0xD301, 0x04);        // select ORA, CA1 on a falling edge
    pia.setCA1(true);
    pia.setCA1(false);
    ASSERT_EQ((int)(peek(pia, 0xD301) & 0x80), 0x80);
    peek(pia, 0xD300);                         // peek ORA: CRA bit 7 stays
    ASSERT_EQ((int)(peek(pia, 0xD301) & 0x80), 0x80);
    read(pia, 0xD300);
    ASSERT_EQ((int)(peek(pia, 0xD301) & 0x80), 0);
}

namespace {
struct TestVic : VIC2 {
    void setCollisions(uint8_t ss, uint8_t sb) { m_sprSprColl = ss; m_sprBgColl = sb; }
};
}

TEST_CASE(io_peek_vic2_collisions_unchanged) {
    TestVic vic;
    vic.setCollisions(0x03, 0x05);
    ASSERT_EQ((int)peek(vic, 0xD01E), 0x03);
    ASSERT_EQ((int)peek(vic, 0xD01F), 0x05);
    ASSERT_EQ((int)read(vic, 0xD01E), 0x03);
    ASSERT_EQ((int)peek(vic, 0xD01E), 0x00);   // cleared by the read
    ASSERT_EQ((int)peek(vic, 0xD01F), 0x05);
}

TEST_CASE(io_peek_mega65_rng_not_advanced) {
    Mega65MathDevice math;
    uint8_t r = peek(math, 0xD7EF);
    ASSERT_EQ((int)peek(math, 0xD7EF), (int)r);
    ASSERT_EQ((int)read(math, 0xD7EF), (int)r);
}

TEST_CASE(io_peek_f011_data_pointer_unchanged) {
    std::string path = "/tmp/mmemu_peek_" + std::to_string(getpid()) + ".d81";
    {
        std::vector<uint8_t> img(F011Fdc::D81_SIZE, 0);
        for (int i = 0; i < 512; i++) img[i] = (uint8_t)i;   // track 0, side 0, sector 1
        FILE* f = fopen(path.c_str(), "wb");
        fwrite(img.data(), 1, img.size(), f);
        fclose(f);
    }
    SdCardDevice sd;
    sd.reset();
    F011Fdc fdc(&sd);
    ASSERT(fdc.mountDisk(8, path));
    fdc.ioWrite(nullptr, 0xD081, 0x01);
    fdc.ioWrite(nullptr, 0xD085, 1);
    fdc.ioWrite(nullptr, 0xD081, 0x40);        // read sector
    uint8_t statB = peek(fdc, 0xD083);
    ASSERT_EQ((int)peek(fdc, 0xD087), 0);
    ASSERT_EQ((int)peek(fdc, 0xD087), 0);      // the CPU pointer did not move
    ASSERT_EQ((int)peek(fdc, 0xD083), (int)statB);
    ASSERT_EQ((int)read(fdc, 0xD087), 0);
    ASSERT_EQ((int)read(fdc, 0xD087), 1);      // reads advance it
    ASSERT_EQ((int)peek(fdc, 0xD087), 2);
    unlink(path.c_str());
}

namespace {
struct NullObserver : ExecutionObserver {};
}

// The bus: peek8 goes through ioPeek, and a write's "before" value (taken when
// an observer such as the debugger is attached) does not read the device.
TEST_CASE(io_peek_bus_peek8_and_write_before_value) {
    FlatMemoryBus bus("system", 16);
    IORegistry io;
    CIA6526 cia{"CIA1", 0xDC00};
    cia.reset();
    io.registerHandler(&cia);
    bus.setIoLowBase(0xD000);
    bus.setIoHooks([&io](IBus* b, uint32_t a, uint8_t* v) { return io.dispatchRead(b, a, v); },
                   [&io](IBus* b, uint32_t a, uint8_t v)  { return io.dispatchWrite(b, a, v); },
                   [&io](IBus* b, uint32_t a, uint8_t* v) { return io.dispatchPeek(b, a, v); });
    NullObserver obs;
    bus.setObserver(&obs);
    bus.write8(0xDC0D, 0x81);
    bus.write8(0xDC04, 0x02);
    bus.write8(0xDC05, 0x00);
    bus.write8(0xDC0E, 0x11);
    cia.tick(10);
    uint8_t icr = bus.peek8(0xDC0D);
    ASSERT_EQ((int)(icr & 0x01), 0x01);
    bus.write8(0xDC0D, 0x01);                  // "before" = a peek: ICR stays pending
    ASSERT_EQ((int)(bus.peek8(0xDC0D) & 0x01), 0x01);
    ASSERT_EQ((int)(bus.read8(0xDC0D) & 0x01), 0x01);
    ASSERT_EQ((int)(bus.peek8(0xDC0D) & 0x01), 0x00);
    bus.setObserver(nullptr);
}
