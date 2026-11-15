#include "test_harness.h"
#include "plugins/devices/mc14412/main/mc14412.h"
#include <memory>

namespace {
constexpr double HZ = 2457600.0;
void runSec(Mc14412& m, double s) {
    const uint64_t n = (uint64_t)(s * HZ);
    for (uint64_t t = 0; t < n; t += 1000) m.tick(1000);
}
}  // namespace

// Carrier: off hook with a far end, for CARRIER_DELAY; bytes pass only with it and the chip enabled
TEST_CASE(mc14412_carrier_and_data) {
    Mc14412 m(HZ);
    auto line = std::make_shared<LoopbackSerialEndpoint>();
    m.setLine(line);
    m.send(0x41);
    ASSERT(!m.carrier());
    ASSERT(m.sentToLine.empty());          // no carrier: lost
    runSec(m, 0.5);
    ASSERT(!m.carrier());                  // on hook
    m.setOffHook(true);
    runSec(m, 0.05);
    ASSERT(!m.carrier());                  // not yet
    runSec(m, 0.1);
    ASSERT(m.carrier());
    m.send(0x43);
    ASSERT(m.sentToLine.empty());          // not enabled: not sent
    m.setEnable(true);
    ASSERT(m.connected());
    m.send(0x42);
    uint8_t b = 0;
    ASSERT(m.receive(b));
    ASSERT_EQ(b, 0x42);
    ASSERT_EQ(m.sentToLine.size(), 1u);
    m.setOffHook(false);                   // hang up: carrier goes at once
    ASSERT(!m.carrier());
    ASSERT(!m.connected());
}

// The line signal: the far modem's mark tone (2225 Hz heard when originating, 1270 Hz when answering)
TEST_CASE(mc14412_line_tone) {
    for (bool orig : {true, false}) {
        Mc14412 m(HZ);
        m.setLine(std::make_shared<LoopbackSerialEndpoint>());
        m.setOriginate(orig);
        ASSERT(!m.lineSignal());
        m.setOffHook(true);
        runSec(m, 0.2);
        // Count rising edges over 0.1 s in 10-clock steps
        int edges = 0;
        bool last = m.lineSignal();
        for (int i = 0; i < (int)(HZ * 0.1 / 10); ++i) {
            m.tick(10);
            const bool v = m.lineSignal();
            if (v && !last) ++edges;
            last = v;
        }
        const int want = orig ? 222 : 127;
        ASSERT(edges >= want - 1 && edges <= want + 1);
    }
}

// No far end (`none`): never a carrier
TEST_CASE(mc14412_no_far_end) {
    Mc14412 m(HZ);
    ASSERT(m.attachLine("none"));
    m.setEnable(true);
    m.setOffHook(true);
    runSec(m, 0.5);
    ASSERT(!m.carrier());
    ASSERT(!m.lineSignal());
    ASSERT(!m.attachLine("nosuchscheme:x"));
}

// Pulse dialling: breaks of 60 ms, makes of 40 ms, 600 ms between digits -> "305"
TEST_CASE(mc14412_pulse_dial) {
    Mc14412 m(HZ);
    m.setLine(std::make_shared<LoopbackSerialEndpoint>());
    m.setEnable(true);
    runSec(m, 1.0);
    m.setOffHook(true);
    runSec(m, 0.5);
    for (int digit : {3, 10, 5}) {
        for (int p = 0; p < digit; ++p) {
            m.setOffHook(false);
            runSec(m, 0.06);
            m.setOffHook(true);
            runSec(m, 0.04);
        }
        runSec(m, 0.6);
    }
    ASSERT_EQ(m.dialled(), std::string("305"));
    ASSERT(m.carrier());                   // the far end answers once dialling stops
    ASSERT(m.connected());
    // Hanging up for a while and picking up starts a new call
    m.setOffHook(false);
    runSec(m, 1.0);
    m.setOffHook(true);
    runSec(m, 0.5);
    ASSERT(m.dialled().empty());
}

// Ringing: a far end while on hook rings 2 s on, 4 s off
TEST_CASE(mc14412_ring) {
    Mc14412 m(HZ);
    m.setLine(std::make_shared<LoopbackSerialEndpoint>());
    m.tick(1);
    ASSERT(m.ring());
    runSec(m, 2.5);
    ASSERT(!m.ring());
    runSec(m, 4.0);
    ASSERT(m.ring());
    m.setOffHook(true);
    ASSERT(!m.ring());
    ASSERT(m.describe().find("modem:loopback") == 0);
}
