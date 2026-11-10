// The uPD7725 (DSP-1..4) with a small hand-assembled program, run on its own
// and through a LoROM cartridge: no Nintendo firmware is needed.

#include "test_harness.h"
#include "plugins/machines/snes/main/machine_snes.h"
#include "plugins/machines/snes/main/snes_necdsp.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/ibus.h"
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// uPD7725 instructions
uint32_t op(int src, int dst, int alu = 0, int psel = 0, int accB = 0) {
    return (uint32_t)(psel << 20 | alu << 16 | accB << 15 | src << 4 | dst);
}
uint32_t ld(uint16_t imm, int dst) { return 0xC00000u | (uint32_t)imm << 6 | (uint32_t)dst; }
uint32_t jp(int type, int addr) { return 0x800000u | (uint32_t)type << 13 | (uint32_t)addr << 2; }
constexpr int JRQM = 0xBE, JMP = 0x100;
constexpr int S_A = 1, S_RP = 5, S_ROM = 6, S_DR = 8, S_DRNF = 9;
constexpr int D_A = 1, D_RP = 5, D_DR = 6, D_K = 0xA, D_L = 0xD;

/**
 * Takes K then L from the SNES (16 bits each), answers K*L (the M output,
 * 1.15 fixed point), then data ROM word 5, and starts again.
 */
std::vector<uint8_t> firmware() {
    const uint32_t prog[] = {
        op(S_DR, 0),            // 0: RQM: ready for input
        jp(JRQM, 1),            // 1: wait for two bytes
        op(S_DRNF, D_K),        // 2
        op(S_DR, 0),            // 3
        jp(JRQM, 4),            // 4
        op(S_DRNF, D_L),        // 5
        ld(0, D_A),             // 6
        op(0, 0, 5, 2),         // 7: A = A + M
        op(S_A, D_DR),          // 8: answer
        jp(JRQM, 9),            // 9: wait for the SNES to read it
        ld(5, D_RP),            // 10
        op(S_ROM, D_DR),        // 11
        jp(JRQM, 12),           // 12
        jp(JMP, 0),             // 13
    };
    std::vector<uint8_t> fw(snes::NecDsp::PROGRAM_SIZE + snes::NecDsp::DATA_SIZE, 0);
    for (size_t i = 0; i < sizeof prog / sizeof prog[0]; ++i) {
        fw[i * 3] = (uint8_t)prog[i]; fw[i * 3 + 1] = (uint8_t)(prog[i] >> 8); fw[i * 3 + 2] = (uint8_t)(prog[i] >> 16);
    }
    fw[snes::NecDsp::PROGRAM_SIZE + 10] = 0xEF;
    fw[snes::NecDsp::PROGRAM_SIZE + 11] = 0xBE;
    return fw;
}

}  // namespace

TEST_CASE(snes_necdsp_core) {
    snes::NecDsp d;
    std::string err;
    ASSERT(d.load(firmware(), err));
    d.step(10);
    EXPECT_TRUE(d.read(true) & 0x80);           // RQM
    d.write(false, 0x00); d.write(false, 0x40);  // K = 0.5
    d.step(10);
    EXPECT_EQ(d.state().k, 0x4000);
    d.write(false, 0x00); d.write(false, 0x20);  // L = 0.25
    d.step(10);
    EXPECT_TRUE(d.read(true) & 0x80);
    EXPECT_EQ(d.read(false), 0x00);
    EXPECT_EQ(d.read(false), 0x10);              // 0.125
    EXPECT_TRUE(!(d.read(true) & 0x80));
    d.step(10);
    EXPECT_EQ(d.read(false), 0xEF);
    EXPECT_EQ(d.read(false), 0xBE);
    d.step(10);
    EXPECT_EQ(d.state().pc, 1);                  // back waiting for K
}

// The ALU's flags: subtract with borrow, overflow
TEST_CASE(snes_necdsp_alu) {
    std::vector<uint8_t> fw(snes::NecDsp::PROGRAM_SIZE + snes::NecDsp::DATA_SIZE, 0);
    const uint32_t prog[] = {
        ld(0x7FFF, D_A),
        ld(0x0001, 3),                           // TR = 1
        op(3, 0, 5, 1),                          // A = A + TR: $8000, overflow
        ld(0x0000, 2),                           // B = 0
        op(3, 0, 4, 1, 1),                       // B = B - TR: $FFFF, carry (borrow)
    };
    for (size_t i = 0; i < 5; ++i) { fw[i * 3] = (uint8_t)prog[i]; fw[i * 3 + 1] = (uint8_t)(prog[i] >> 8); fw[i * 3 + 2] = (uint8_t)(prog[i] >> 16); }
    snes::NecDsp d;
    std::string err;
    ASSERT(d.load(fw, err));
    d.step(5);
    const auto& s = d.state();
    EXPECT_EQ(s.a, 0x8000);
    EXPECT_TRUE(s.fa.ov0);
    EXPECT_TRUE(s.fa.s0);
    EXPECT_TRUE(!(s.fa.c));
    EXPECT_EQ(s.b, 0xFFFF);
    EXPECT_TRUE(s.fb.c);
    EXPECT_TRUE(!(s.fb.z));
}

namespace {
/** A LoROM cartridge with a DSP chip (chipset $03) and the firmware appended. */
std::vector<uint8_t> dspCart(bool withFirmware) {
    std::vector<uint8_t> img(0x10000, 0);
    const char* title = "MMSIM DSP            ";
    for (int i = 0; i < 21; ++i) img[0x7FC0 + i] = (uint8_t)title[i];
    img[0x7FD5] = 0x20; img[0x7FD6] = 0x03; img[0x7FD7] = 0x06;
    img[0x7FDC] = 0xFF; img[0x7FDD] = 0xFF;
    img[0x7FFC] = 0x00; img[0x7FFD] = 0x80;
    const uint8_t code[] = {
        0x78, 0x18, 0xFB, 0xE2, 0x30,                        // SEI CLC XCE SEP #$30
        0xA9, 0x00, 0x8F, 0x00, 0x80, 0x30,                  // K = $4000
        0xA9, 0x40, 0x8F, 0x00, 0x80, 0x30,
        0xAF, 0x00, 0xC0, 0x30, 0x10, 0xFA,                  // wait for RQM
        0xA9, 0x00, 0x8F, 0x00, 0x80, 0x30,                  // L = $2000
        0xA9, 0x20, 0x8F, 0x00, 0x80, 0x30,
        0xAF, 0x00, 0xC0, 0x30, 0x10, 0xFA,
        0xAF, 0x00, 0x80, 0x30, 0x8D, 0x00, 0x00,            // the answer to $0000/$0001
        0xAF, 0x00, 0x80, 0x30, 0x8D, 0x01, 0x00,
        0x80, 0xFE,
    };
    for (size_t i = 0; i < sizeof code; ++i) img[i] = code[i];
    if (withFirmware) { const auto fw = firmware(); img.insert(img.end(), fw.begin(), fw.end()); }
    return img;
}
}  // namespace

TEST_CASE(snes_necdsp_cartridge) {
    std::string err;
    MachineDescriptor* m = snes::create(false, &err);
    ASSERT(snes::insertCartridge(m, dspCart(true), "d.sfc", err));
    EXPECT_EQ(snes::cartridgeInfo(m), std::string("d.sfc (MMSIM DSP, LoROM, 64K ROM, DSP1B)"));
    uint64_t t = 0;
    while (t < 200000) t += (uint64_t)m->schedulerStep(*m);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0x000000), 0x00);
    EXPECT_EQ(bus->peek8(0x000001), 0x10);
    delete m;
}

TEST_CASE(snes_necdsp_missing_firmware) {
    setenv("MMEMU_SNES_FIRMWARE", "/nonexistent", 1);
    std::string err;
    MachineDescriptor* m = snes::create(false, &err);
    EXPECT_TRUE(!(snes::insertCartridge(m, dspCart(false), "d.sfc", err)));
    EXPECT_TRUE(err.find("/nonexistent/dsp1b.rom") != std::string::npos);
    unsetenv("MMEMU_SNES_FIRMWARE");
    delete m;
}
