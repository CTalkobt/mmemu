#include "test_harness.h"
#include "plugins/devices/serial_endpoint/main/serial_endpoint.h"
#include <cstdio>
#include <filesystem>
#include <unistd.h>

TEST_CASE(serial_endpoint_specs) {
    auto none = makeSerialEndpoint("none");
    ASSERT(none);
    uint8_t b = 0;
    none->send(1);
    ASSERT(!none->receive(b));
    ASSERT(!none->dcd());
    auto loop = makeSerialEndpoint("loopback");
    loop->send(0x42);
    ASSERT(loop->receive(b));
    ASSERT_EQ(b, 0x42);
    ASSERT(!loop->receive(b));
    ASSERT(!makeSerialEndpoint("nonsense:1"));
    ASSERT(!makeSerialEndpoint("tcp:0"));
    // file: captures what is sent
    const auto path = std::filesystem::temp_directory_path() / ("mmsim_serial_" + std::to_string(::getpid()));
    {
        auto f = makeSerialEndpoint("file:" + path.string());
        ASSERT(f);
        f->send('h');
        f->send('i');
        ASSERT_EQ(f->describe(), "file:" + path.string());
    }
    ASSERT_EQ(std::filesystem::file_size(path), 2u);
    std::filesystem::remove(path);
    // A registered scheme
    registerSerialScheme("echo-test", [](const std::string& arg) -> std::unique_ptr<SerialEndpoint> {
        auto l = std::make_unique<LoopbackSerialEndpoint>();
        for (char c : arg) l->send((uint8_t)c);
        return l;
    });
    auto e = makeSerialEndpoint("echo-test:ok");
    ASSERT(e);
    ASSERT(e->receive(b));
    ASSERT_EQ(b, 'o');
}
