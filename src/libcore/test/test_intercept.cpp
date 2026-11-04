#include "test_harness.h"
#include "libcore/main/intercept.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/memory_bus.h"
#include "plugins/6502/main/cpu6502.h"
#include "plugins/devices/mega65_hypervisor/main/hdos_handler.h"
#include "plugins/devices/mega65_hypervisor/main/hypervisor_regs.h"
#include "plugins/45gs02/main/cpu45gs02.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// Counts calls; handles them (simulating RTS to $1234) when `handle` is set
struct CountingPc : IPcIntercept {
    uint32_t at; int calls = 0; bool handle = true;
    explicit CountingPc(const json& e) : at(interceptAddr(e.value("addr", json("$FFD5")))) {}
    std::vector<uint32_t> addresses() const override { return { at }; }
    bool onExecute(MachineDescriptor&, ICore& cpu, IBus&, uint32_t) override {
        calls++;
        if (handle) cpu.setPc(0x1234);
        return handle;
    }
};

struct CountingCall : ICallIntercept {
    int calls = 0;
    bool onCall(MachineDescriptor&, ICore&, uint32_t, uint32_t) override { calls++; return true; }
};

void registerTestTypes() {
    InterceptRegistry::instance().registerType("test.pc", [](const json& e) -> IIntercept* { return new CountingPc(e); });
    InterceptRegistry::instance().registerType("test.call", [](const json&) -> IIntercept* { return new CountingCall(); });
}

// A 6502 machine with RAM and a "ROM" image the test can map in and out
struct Rig {
    MachineDescriptor m;
    FlatMemoryBus* bus = new FlatMemoryBus("system", 16);
    MOS6502* cpu = new MOS6502();
    uint8_t rom[0x2000];
    Rig() {
        cpu->setDataBus(bus);
        cpu->setCodeBus(bus);
        m.machineId = "rig";
        m.cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});
        m.buses.push_back({"system", bus});
        for (int i = 0; i < 0x2000; i++) rom[i] = (uint8_t)(i * 7 + 3);
        mapRom(true);
    }
    void mapRom(bool on) {
        for (int i = 0; i < 0x2000; i++) bus->write8(0xE000 + i, on ? rom[i] : 0xEA);
    }
};

} // namespace

TEST_CASE(intercept_rom_entry_fires_only_while_rom_visible) {
    registerTestTypes();
    Rig r;
    json spec = json::parse(R"({ "roms": [ { "label": "kernal", "size": 8192, "cpuAddr": "0xE000",
        "intercepts": [ { "type": "test.pc", "name": "t", "addr": "$FFD5" } ] } ] })");
    installIntercepts(r.m, spec, [&r](const std::string& l) -> const uint8_t* { return l == "kernal" ? r.rom : nullptr; });
    ASSERT_EQ((int)r.m.intercepts.size(), 1);
    ASSERT(r.m.warnings.empty());
    auto* t = static_cast<CountingPc*>(r.m.intercepts[0]);

    r.cpu->setPc(0xFFD5);
    r.cpu->step();                       // ROM visible: intercepted, instruction skipped
    ASSERT_EQ(t->calls, 1);
    ASSERT_EQ((int)r.cpu->pc(), 0x1234);

    r.mapRom(false);                     // banked out: RAM at $FFD5, runs normally
    r.cpu->setPc(0xFFD5);
    r.cpu->step();
    ASSERT_EQ(t->calls, 1);
    ASSERT_EQ((int)r.cpu->pc(), 0xFFD6); // NOP executed

    r.mapRom(true);
    t->setEnabled(false);                // `intercept off`
    r.cpu->setPc(0xFFD5);
    r.cpu->step();
    ASSERT_EQ(t->calls, 1);
}

TEST_CASE(intercept_not_handled_runs_instruction) {
    registerTestTypes();
    Rig r;
    installIntercepts(r.m, json::parse(R"({ "intercepts": [ { "type": "test.pc", "addr": "$0300" } ] })"));
    auto* t = static_cast<CountingPc*>(r.m.intercepts[0]);
    t->handle = false;
    r.bus->write8(0x0300, 0xEA);
    r.cpu->setPc(0x0300);
    r.cpu->step();
    ASSERT_EQ(t->calls, 1);
    ASSERT_EQ((int)r.cpu->pc(), 0x0301);
}

TEST_CASE(intercept_unknown_type_and_missing_rom_address_warn) {
    registerTestTypes();
    Rig r;
    json spec = json::parse(R"({ "intercepts": [ { "type": "no.such.type" }, { "name": "x" } ],
        "roms": [ { "label": "kernal", "intercepts": [ { "type": "test.pc" } ] } ] })");
    installIntercepts(r.m, spec, [&r](const std::string&) -> const uint8_t* { return r.rom; });
    ASSERT(r.m.intercepts.empty());
    ASSERT_EQ((int)r.m.warnings.size(), 3);
}

TEST_CASE(intercept_call_selection_by_trap_and_function) {
    registerTestTypes();
    Rig r;
    installIntercepts(r.m, json::parse(R"({ "intercepts": [
        { "type": "test.call", "name": "dos", "trap": "$00", "functions": ["$02", "$34"] } ] })"));
    auto* c = static_cast<CountingCall*>(findIntercept(r.m, "dos"));
    ASSERT(c != nullptr);
    ASSERT(dispatchCallIntercepts(r.m, *r.cpu, 0x00, 0x34));
    ASSERT(!dispatchCallIntercepts(r.m, *r.cpu, 0x00, 0x36));   // not listed
    ASSERT(!dispatchCallIntercepts(r.m, *r.cpu, 0x01, 0x02));   // other trap
    c->setEnabled(false);
    ASSERT(!dispatchCallIntercepts(r.m, *r.cpu, 0x00, 0x02));
    ASSERT_EQ(c->calls, 1);
}

// HYPPO setname ($2E) is honoured: the name at user page Y is noted (and the
// call still goes to HYPPO); findfile ($34) then finds that host file. The
// `filename` setting overrides it.
TEST_CASE(hdos_setname_reaches_findfile_and_can_be_overridden) {
    std::string dir = "/tmp/mmemu_hdos_" + std::to_string(getpid());
    fs::create_directories(dir);
    std::ofstream(dir + "/game.prg") << "x";
    std::ofstream(dir + "/other.prg") << "y";

    registerHdosIntercept();
    MachineDescriptor m;
    m.machineId = "m65rig";
    auto* bus = new FlatMemoryBus("system", 16);
    auto* cpu = new MOS45GS02();
    cpu->setDataBus(bus);
    cpu->setCodeBus(bus);
    cpu->reset();
    m.cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});
    m.buses.push_back({"system", bus});
    json e = defaultHdosInterceptSpec();
    e["paths"] = json::array({dir});
    installIntercepts(m, json{{"intercepts", json::array({e})}});
    ASSERT_EQ((int)m.intercepts.size(), 1);

    HypervisorRegs regs(cpu);
    regs.setCallIntercept([&m, cpu](uint8_t trap, uint8_t f) { return dispatchCallIntercepts(m, *cpu, trap, f); });
    const char* name = "GAME.PRG";
    for (int i = 0; name[i]; i++) bus->write8(0x1200 + i, (uint8_t)name[i]);
    bus->write8(0x1200 + 8, 0);                         // name at user page $12
    bus->write8(0x2000, 0xEA);                          // NOP after the trap write
    auto trap = [&](uint8_t a, uint8_t y) {
        cpu->setPc(0x2000);
        cpu->regWrite(0, a);
        cpu->regWrite(2, y);
        regs.ioWrite(nullptr, 0xD640, 0);
    };

    trap(0x2E, 0x12);                                    // setname: noted, HYPPO handles it
    ASSERT(cpu->isHypervisor());
    cpu->exitHypervisor();
    trap(0x34, 0x00);                                    // findfile: served from the host folder
    ASSERT(!cpu->isHypervisor());
    ASSERT_EQ((int)(cpu->regRead(7) & 1), 1);            // carry set: found

    IIntercept* h = findIntercept(m, "hdos");
    ASSERT(h->setOption("filename", "MISSING.PRG"));
    trap(0x34, 0x00);
    ASSERT_EQ((int)(cpu->regRead(7) & 1), 0);            // the override wins
    ASSERT(h->setOption("filename", "OTHER.PRG"));
    trap(0x34, 0x00);
    ASSERT_EQ((int)(cpu->regRead(7) & 1), 1);
    fs::remove_all(dir);
}
