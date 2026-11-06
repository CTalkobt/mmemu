#include "test_harness.h"
#include "libcore/main/core_registry.h"
#include "libcore/main/machines/machine_registry.h"
#include "libcore/main/image_loader.h"
#include <memory>
#include <string>

TEST_CASE(core_registry_basic) {
    auto& reg = CoreRegistry::instance();
    
    class MockCore : public ICore {
    public:
        const char* isaName() const override { return "mock"; }
        const char* variantName() const override { return "mock"; }
        uint32_t isaCaps() const override { return 0; }
        int regCount() const override { return 0; }
        const RegDescriptor* regDescriptor(int) const override { return nullptr; }
        uint32_t regRead(int) const override { return 0; }
        void regWrite(int, uint32_t) override {}
        uint32_t pc() const override { return 0; }
        void setPc(uint32_t) override {}
        uint32_t sp() const override { return 0; }
        int step() override { return 1; }
        void reset() override {}
        void triggerIrq() override {}
        void triggerNmi() override {}
        void triggerReset() override {}
        int disassembleOne(IBus*, uint32_t, char*, int) override { return 1; }
        int disassembleEntry(IBus*, uint32_t, void*) override { return 1; }
        void saveState(uint8_t*) const override {}
        void loadState(const uint8_t*) override {}
        size_t stateSize() const override { return 0; }
        int isCallAt(IBus*, uint32_t) override { return 0; }
        bool isReturnAt(IBus*, uint32_t) override { return false; }
        bool isProgramEnd(IBus*) override { return false; }
        int writeCallHarness(IBus*, uint32_t, uint32_t) override { return 0; }
        void setDataBus(IBus*) override {}
        void setCodeBus(IBus*) override {}
        void setIoBus(IBus*) override {}
        IBus* getDataBus() const override { return nullptr; }
        IBus* getCodeBus() const override { return nullptr; }
        IBus* getIoBus()   const override { return nullptr; }
        uint64_t cycles() const override { return 0; }
    };
    
    reg.registerCore("mock", "default", "GPL", []() { return new MockCore(); });
    
    ICore* core = reg.createCore("mock");
    ASSERT(core != nullptr);
    ASSERT(std::string(core->isaName()) == "mock");
    delete core;
}

TEST_CASE(machine_registry_basic) {
    auto& reg = MachineRegistry::instance();
    
    reg.registerMachine("test_machine", []() {
        auto* md = new MachineDescriptor();
        md->machineId = "test_machine";
        md->displayName = "Test Machine";
        return md;
    });
    
    MachineDescriptor* md = reg.createMachine("test_machine");
    ASSERT(md != nullptr);
    ASSERT(md->machineId == "test_machine");
    ASSERT(md->displayName == "Test Machine");
    delete md;
    
    std::vector<std::string> ids;
    reg.enumerate(ids);
    bool found = false;
    for (const auto& id : ids) if (id == "test_machine") found = true;
    ASSERT(found);
}

// Two cartridge factories on one extension (.crt for the C64 and the
// Plus/4): the first that accepts the file wins, nullptr passes it on
TEST_CASE(cartridge_factories_share_extension) {
    struct Cart : ICartridgeHandler {
        explicit Cart(std::string t) : type(std::move(t)) {}
        bool attach(IBus*, MachineDescriptor*) override { return true; }
        void eject(IBus*) override {}
        CartridgeMetadata metadata() const override { CartridgeMetadata md; md.type = type; return md; }
        std::string type;
    };
    auto& reg = ImageLoaderRegistry::instance();
    reg.registerCartridgeHandler("tstcrt", [](const std::string& p) -> std::unique_ptr<ICartridgeHandler> {
        if (p.find("first") == std::string::npos) return nullptr;
        return std::make_unique<Cart>("first");
    });
    reg.registerCartridgeHandler("TSTCRT", [](const std::string& p) -> std::unique_ptr<ICartridgeHandler> {
        if (p.find("second") == std::string::npos) return nullptr;
        return std::make_unique<Cart>("second");
    });
    auto a = reg.createCartridgeHandler("x/first.tstcrt");
    auto b = reg.createCartridgeHandler("x/second.TstCrt");
    ASSERT(a && a->metadata().type == "first");
    ASSERT(b && b->metadata().type == "second");
    ASSERT(!reg.createCartridgeHandler("x/neither.tstcrt"));
}
