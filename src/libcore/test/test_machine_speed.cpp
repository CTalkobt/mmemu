#include "test_harness.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/machine_speed.h"
#include <chrono>

using machine_speed::RealTimePacer;
using namespace std::chrono_literals;

// Real-time pacing arithmetic (#169)
TEST_CASE(realtime_pacer_paces_device_cycles) {
    RealTimePacer p;
    auto t0 = RealTimePacer::Clock::time_point{} + 10s;
    ASSERT(!p.ahead(1000, 1000000.0, t0));               // first call only syncs
    ASSERT(!p.ahead(1000 + 10000, 1000000.0, t0 + 10ms)); // 10 ms emulated at 10 ms: on time
    ASSERT(p.ahead(1000 + 20000, 1000000.0, t0 + 10ms));  // 20 ms emulated at 10 ms: ahead
    auto w = p.waitTime(1000 + 20000, t0 + 10ms);
    ASSERT(w > 9ms && w < 11ms);
    ASSERT(!p.ahead(1000 + 20000, 1000000.0, t0 + 25ms)); // wall caught up
    ASSERT(p.waitTime(1000 + 20000, t0 + 25ms) == RealTimePacer::Clock::duration::zero());
    ASSERT(!p.ahead(1000, 0.0, t0));                      // no clock: never paced
}

TEST_CASE(realtime_pacer_resyncs_when_far_behind) {
    RealTimePacer p;
    auto t0 = RealTimePacer::Clock::time_point{} + 10s;
    p.ahead(0, 1000000.0, t0);
    // Paused for a second: 1 s of wall time, 0 emulated. No catch-up burst:
    ASSERT(!p.ahead(0, 1000000.0, t0 + 1s));              // resyncs here
    ASSERT(p.ahead(5000, 1000000.0, t0 + 1s + 1ms));      // 5 ms emulated in 1 ms: ahead again
    // A reset that lowers the cycle count resyncs too
    ASSERT(!p.ahead(10, 1000000.0, t0 + 1s + 2ms));
    ASSERT(p.ahead(10 + 3000, 1000000.0, t0 + 1s + 3ms));
}

TEST_CASE(machine_speed_real_time_and_max) {
    MachineDescriptor m;
    ASSERT(!machine_speed::controllable(m));
    std::string err;
    ASSERT(!machine_speed::apply(m, "max", err));
    ASSERT(machine_speed::applyStartSpeed(m, "real", err));   // no speed setting: ignored

    uint64_t cycles = 0;
    m.clockHz = 985248;
    m.deviceCycles = [&]() { return cycles; };
    ASSERT(machine_speed::controllable(m));
    ASSERT(!machine_speed::supported(m));
    ASSERT_EQ(machine_speed::choices(m), std::string("real|max"));
    // Created at max (tests, MCP); front ends select real time on startup
    ASSERT(machine_speed::atMax(m));
    ASSERT(machine_speed::applyStartSpeed(m, "real", err));
    ASSERT_EQ(machine_speed::describe(m), std::string("Speed: real time (985248 Hz)"));
    ASSERT(!machine_speed::applyStartSpeed(m, "fast", err));
    ASSERT(machine_speed::applyStartSpeed(m, "", err));    // "" leaves it alone
    ASSERT(!machine_speed::atMax(m));

    ASSERT(machine_speed::apply(m, "max", err));
    ASSERT(machine_speed::atMax(m));
    ASSERT_EQ(machine_speed::describe(m), std::string("Speed: max (unpaced)"));
    ASSERT(!machine_speed::apply(m, "1", err));           // no CPU clock choices
    ASSERT_EQ(err, std::string("Syntax: speed [real|max]"));
    ASSERT(machine_speed::apply(m, "Real", err));
    ASSERT(!machine_speed::atMax(m));

    // The machine form of ahead(): paced at real time, never at max
    RealTimePacer p;
    auto t0 = RealTimePacer::Clock::time_point{} + 10s;
    p.ahead(m, t0);
    cycles = 985248 / 10;                                 // 100 ms emulated
    ASSERT(p.ahead(m, t0 + 50ms));
    m.maxSpeed = true;
    ASSERT(!p.ahead(m, t0 + 50ms));
}
