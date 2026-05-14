/**
 * Reproduction test for GitHub Issue #1:
 * I/O space instruction execution bug at $FFD0+
 *
 * Root cause: MCP server used desc->buses[0] (physical SparseMemoryBus)
 * for memory I/O, while the CPU reads/writes through the MapMmu virtual
 * bus. The MapMmu's C64 banking translates $E000-$FFFF to $002E000+,
 * so MCP writes landed at a different physical address than where the
 * CPU reads from.
 */
#include "test_harness.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/machines/machine_registry.h"
#include "libcore/main/icore.h"
#include "libmem/main/ibus.h"
#include "plugin_loader/main/plugin_loader.h"
#include <cstdio>

static void ensurePluginsLoaded() {
    static bool loaded = false;
    if (!loaded) {
        PluginLoader::instance().loadFromDir("./lib");
        loaded = true;
    }
}

struct RawMega65Machine {
    MachineDescriptor* desc;
    ICore* cpu;
    IBus*  bus;

    RawMega65Machine() {
        ensurePluginsLoaded();
        desc = MachineRegistry::instance().createMachine("rawMega65");
        if (!desc || desc->cpus.empty()) {
            fprintf(stderr, "FATAL: Could not create rawMega65 machine\n");
            exit(1);
        }
        cpu = desc->cpus[0].cpu;
        bus = desc->cpus[0].dataBus;
        if (!cpu || !bus) {
            fprintf(stderr, "FATAL: rawMega65 has no CPU or bus\n");
            exit(1);
        }
    }

    ~RawMega65Machine() {
        delete desc;
    }

    void writeBytes(uint32_t addr, const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; i++)
            bus->write8(addr + i, data[i]);
    }

    void writeResetVector(uint16_t startAddr) {
        bus->write8(0xFFFC, startAddr & 0xFF);
        bus->write8(0xFFFD, (startAddr >> 8) & 0xFF);
    }
};

TEST_CASE(ffd0_real_machine_read_write_consistency) {
    RawMega65Machine m;
    for (uint32_t a = 0xFFD0; a <= 0xFFDF; a++)
        m.bus->write8(a, (uint8_t)(a & 0xFF));
    for (uint32_t a = 0xFFD0; a <= 0xFFDF; a++) {
        uint8_t val = m.bus->read8(a);
        if (val != (uint8_t)(a & 0xFF)) {
            fprintf(stderr, "  FAIL: bus read8($%04X) = $%02X, expected $%02X\n",
                    a, val, (uint8_t)(a & 0xFF));
        }
        ASSERT_EQ((int)val, (int)(uint8_t)(a & 0xFF));
    }
}

TEST_CASE(ffd0_real_machine_7byte_stub) {
    RawMega65Machine m;
    uint8_t stub[] = { 0xA9, 0xDE, 0xA2, 0xAD, 0xA0, 0xBE, 0x60 };
    m.writeBytes(0xFFD2, stub, sizeof(stub));
    uint8_t caller[] = { 0x20, 0xD2, 0xFF, 0x00 };
    m.writeBytes(0x0800, caller, sizeof(caller));
    m.writeResetVector(0x0800);
    m.cpu->reset();

    ASSERT_EQ((int)m.cpu->pc(), 0x0800);
    m.cpu->step(); // JSR
    ASSERT_EQ((int)m.cpu->pc(), 0xFFD2);
    m.cpu->step(); // LDA #$DE
    ASSERT_EQ((int)m.cpu->pc(), 0xFFD4);
    m.cpu->step(); // LDX #$AD
    ASSERT_EQ((int)m.cpu->pc(), 0xFFD6);
    m.cpu->step(); // LDY #$BE
    ASSERT_EQ((int)m.cpu->pc(), 0xFFD8);

    uint8_t opAtFFD8 = m.bus->read8(0xFFD8);
    ASSERT_EQ((int)opAtFFD8, 0x60);

    m.cpu->step(); // RTS
    fprintf(stderr, "  7-byte real: After RTS, PC=$%04X (expect $0803)\n", m.cpu->pc());
    ASSERT_EQ((int)m.cpu->pc(), 0x0803);
}

TEST_CASE(ffd0_real_machine_9byte_stub) {
    RawMega65Machine m;
    uint8_t stub[] = { 0xA9, 0xDE, 0xA2, 0xAD, 0xA0, 0xBE, 0xA3, 0xEF, 0x60 };
    m.writeBytes(0xFFD2, stub, sizeof(stub));
    uint8_t caller[] = { 0x20, 0xD2, 0xFF, 0x00 };
    m.writeBytes(0x0800, caller, sizeof(caller));
    m.writeResetVector(0x0800);
    m.cpu->reset();

    m.cpu->step(); // JSR
    ASSERT_EQ((int)m.cpu->pc(), 0xFFD2);
    m.cpu->step(); // LDA
    m.cpu->step(); // LDX
    m.cpu->step(); // LDY
    m.cpu->step(); // LDZ #$EF
    ASSERT_EQ((int)m.cpu->pc(), 0xFFDA);
    ASSERT_EQ((int)m.cpu->regRead(3), 0xEF);

    m.cpu->step(); // RTS
    fprintf(stderr, "  9-byte real: After RTS, PC=$%04X (expect $0803)\n", m.cpu->pc());
    ASSERT_EQ((int)m.cpu->pc(), 0x0803);
}

// Regression: proves the physical bus and CPU bus see different addresses
// at $FFD0+ when MapMmu C64 banking is active.
TEST_CASE(ffd0_bus_mismatch_regression) {
    RawMega65Machine m;

    IBus* physBus = m.desc->buses[0].bus;
    IBus* cpuBus  = m.cpu->getDataBus();

    // Write different values through each bus
    physBus->write8(0xFFD2, 0xAA);
    cpuBus->write8(0xFFD2, 0xBB);

    // CPU should see 0xBB (written via its own bus)
    uint8_t cpuSees = cpuBus->read8(0xFFD2);
    ASSERT_EQ((int)cpuSees, 0xBB);

    // If translation is active, the physical bus still has 0xAA at raw $FFD2
    if (physBus != cpuBus) {
        uint8_t physSees = physBus->read8(0xFFD2);
        fprintf(stderr, "  regression: phys=$%02X cpu=$%02X (different = translation active)\n",
                physSees, cpuSees);
        ASSERT_NE((int)physSees, (int)cpuSees);
    }
}
