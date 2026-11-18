#include "test_harness.h"
#include <algorithm>
#include "libcore/main/rom_loader.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/memory_bus.h"
#include "libdevices/main/io_registry.h"
#include "libcore/main/machines/machine_registry.h"

TEST_CASE(smoke_test) {
    ASSERT(1 == 1);
}

TEST_CASE(rom_loader_success) {
    const char* path = "test_rom.bin";
    uint8_t data[] = { 0x01, 0x02, 0x03, 0x04 };
    FILE* f = fopen(path, "wb");
    fwrite(data, 1, sizeof(data), f);
    fclose(f);

    uint8_t buf[4];
    bool result = romLoad(path, buf, 4);
    ASSERT(result == true);
    ASSERT(buf[0] == 0x01);
    ASSERT(buf[3] == 0x04);

    remove(path);
}

TEST_CASE(rom_loader_size_mismatch) {
    const char* path = "test_rom_mismatch.bin";
    uint8_t data[] = { 0x01, 0x02, 0x03 };
    FILE* f = fopen(path, "wb");
    fwrite(data, 1, sizeof(data), f);
    fclose(f);

    uint8_t buf[4];
    bool result = romLoad(path, buf, 4);
    ASSERT(result == false);

    remove(path);
}

TEST_CASE(machine_descriptor_cleanup) {
    auto* desc = new MachineDescriptor();
    desc->machineId = "test";
    
    auto* bus = new FlatMemoryBus("test", 16);
    desc->buses.push_back({"system", bus});
    
    auto* io = new IORegistry();
    desc->ioRegistry = io;
    
    class MockHandler : public IOHandler {
    public:
        MockHandler(bool* deleted) : m_deleted(deleted) {}
        ~MockHandler() override { *m_deleted = true; }
        const char* name() const override { return "mock"; }
        uint32_t baseAddr() const override { return 0; }
        uint32_t addrMask() const override { return 0; }
        bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
        bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
        void reset() override {}
        void tick(uint64_t) override {}
    private:
        bool* m_deleted;
    };
    
    bool handlerDeleted = false;
    io->registerOwnedHandler(new MockHandler(&handlerDeleted));
    
    uint8_t* rom = new uint8_t[1024];
    desc->roms.push_back(rom);
    
    bool genericDeleted = false;
    desc->deleters.push_back([&genericDeleted]() { genericDeleted = true; });
    
    delete desc;
    
    ASSERT(handlerDeleted == true);
    ASSERT(genericDeleted == true);
    // Note: cpus and buses are also deleted, but they are pointers to ICore/IBus
    // which we'd need more mocks to verify. This is enough for now.
}

// A machine's vendor (for the GUI's grouped machine list) is kept with its
// registration, handed to the descriptor it creates, and "" when not given
TEST_CASE(machine_registry_vendor) {
    auto& reg = MachineRegistry::instance();
    reg.registerMachine("test-vendor-a", [] { auto* m = new MachineDescriptor(); m->machineId = "a"; return m; },
                        "Test machine A", "Acme");
    reg.registerMachine("test-vendor-b", [] { auto* m = new MachineDescriptor(); m->machineId = "b"; return m; },
                        "Test machine B");
    reg.registerMachine("test-vendor-c", [] {
        auto* m = new MachineDescriptor();
        m->machineId = "c";
        m->vendor = "Own";                          // the descriptor's own wins
        return m;
    }, "Test machine C", "Acme");
    EXPECT_EQ(reg.vendorOf("test-vendor-a"), std::string("Acme"));
    EXPECT_EQ(reg.vendorOf("test-vendor-b"), std::string(""));
    EXPECT_EQ(reg.vendorOf("no-such-machine"), std::string(""));
    MachineDescriptor* a = reg.createMachine("test-vendor-a");
    MachineDescriptor* b = reg.createMachine("test-vendor-b");
    MachineDescriptor* c = reg.createMachine("test-vendor-c");
    ASSERT(a && b && c);
    EXPECT_EQ(a->vendor, std::string("Acme"));
    EXPECT_EQ(b->vendor, std::string(""));
    EXPECT_EQ(c->vendor, std::string("Own"));
    delete a;
    delete b;
    delete c;
    // Registering again without a vendor forgets it
    reg.registerMachine("test-vendor-a", [] { return new MachineDescriptor(); }, "Test machine A", "");
    EXPECT_EQ(reg.vendorOf("test-vendor-a"), std::string(""));
    // The plugin registering a machine is kept with it
    reg.setSource("mmemu-plugin-test.so");
    reg.registerMachine("test-vendor-b", [] { return new MachineDescriptor(); }, "Test machine B", "Acme");
    reg.setSource("");
    EXPECT_EQ(reg.sourceOf("test-vendor-b"), std::string("mmemu-plugin-test.so"));
    EXPECT_EQ(reg.sourceOf("test-vendor-c"), std::string(""));
    for (const char* id : {"test-vendor-a", "test-vendor-b", "test-vendor-c"}) reg.unregisterMachine(id);
    EXPECT_EQ(reg.sourceOf("test-vendor-b"), std::string(""));
    std::vector<std::string> ids;
    reg.enumerate(ids);
    EXPECT_TRUE(std::find(ids.begin(), ids.end(), "test-vendor-b") == ids.end());
}
