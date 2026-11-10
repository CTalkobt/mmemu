#pragma once

// The ST018 in Hayazashi Nidan Morita Shougi 2 (#15).
//
// After Mesen2 (Core/SNES/Coprocessors/ST018/), GPL-3.0, Sour and the Mesen
// contributors. Seta's ST018 is an ARMv3 (an ARM60-class core) with 128 KB
// of program ROM, 32 KB of data ROM and 16 KB of work RAM, talking to the
// SNES through a byte each way and a status register at $3800-$3804
// ($00-$3F/$80-$BF:$3000-$3FFF, decoded by A1-A2 and A8-A15). The rest of
// the cartridge is the plain LoROM map with battery RAM.
//
// The ARM is run up to the master clock after every SNES step; as in
// Mesen, each of its bus accesses and internal cycles takes one master
// clock. The firmware is not in the cartridge dump: roms/snes/st018.rom
// (163840 bytes: the program, then the data ROM), as Mesen.

#include <cstdint>
#include <vector>

namespace snes {

class St018 {
public:
    static constexpr uint32_t PRG_SIZE = 0x20000, DATA_SIZE = 0x8000, RAM_SIZE = 0x4000;

    enum Mode : uint8_t {
        USR = 0x10, FIQ = 0x11, IRQ = 0x12, SVC = 0x13, ABT = 0x17, UND = 0x1B, SYS = 0x1F,
    };
    struct Psr {
        uint8_t mode = 0;
        bool f = false, i = false, v = false, c = false, z = false, n = false;
        uint32_t value() const {
            return (uint32_t)n << 31 | (uint32_t)z << 30 | (uint32_t)c << 29 | (uint32_t)v << 28 | (uint32_t)i << 7 | (uint32_t)f << 6 | mode;
        }
    };

    /** `firmware`: PRG_SIZE bytes of program then DATA_SIZE bytes of data. */
    explicit St018(const std::vector<uint8_t>& firmware);

    /** The ARM's power-on (as the cartridge's). */
    void powerOn();
    /** Run up to master clock `master`. */
    void runTo(uint64_t master);

    /** The SNES side, $3000-$3FFF: -1 for open bus. */
    int read(uint16_t a, bool peek);
    void write(uint16_t a, uint8_t v);

    // For tests and the debugger
    uint32_t reg(int i) const { return m_r[i & 15]; }
    const Psr& cpsr() const { return m_cpsr; }
    uint8_t workRam(uint32_t a) const { return m_ram[a & (RAM_SIZE - 1)]; }
    uint64_t cycles() const { return m_cycles; }

private:
    // Bus access kinds (Mesen's ArmV3AccessMode)
    enum : uint8_t { SEQ = 1, WORD = 2, BYTE = 4, NO_ROTATE = 8, PREFETCH = 16 };

    // The ST018's bus
    uint32_t busRead(uint8_t mode, uint32_t addr);
    void busWrite(uint8_t mode, uint32_t addr, uint32_t v);
    uint8_t readByte(uint32_t addr);
    void writeByte(uint32_t addr, uint8_t v);
    uint8_t status() const;

    // The ARM
    void armPowerOn(bool forReset);
    void exec();
    void processPipeline();
    void reloadPipeline();
    uint32_t readCode(uint8_t mode, uint32_t addr) { m_pipeMode |= SEQ; return busRead(mode, addr); }
    uint32_t read(uint8_t mode, uint32_t addr) { m_pipeMode &= (uint8_t)~SEQ; return busRead(mode, addr); }
    void write(uint8_t mode, uint32_t addr, uint32_t v) { m_pipeMode &= (uint8_t)~SEQ; busWrite(mode, addr, v); }
    void idle(int n = 1) { m_pipeMode &= (uint8_t)~SEQ; m_cycles += (uint64_t)n; }
    void setR(uint8_t r, uint32_t v) { m_r[r] = v; if (r == 15) m_reload = true; }
    void switchMode(uint8_t mode);
    Psr& spsr();
    void exception(uint8_t mode, uint32_t vector);
    bool condition(uint32_t cc) const;

    uint32_t add(uint32_t a, uint32_t b, bool carry, bool flags);
    uint32_t sub(uint32_t a, uint32_t b, bool carry, bool flags) { return add(a, ~b, carry, flags); }
    uint32_t logical(uint32_t result, bool carry, bool flags);
    uint32_t shift(uint8_t type, uint32_t value, uint8_t amount, bool& carry);

    void opBranch();
    void opMsr();
    void opMrs();
    void opDataProcessing();
    void opMultiply();
    void opMultiplyLong();
    void opSingleTransfer();
    void opBlockTransfer();
    void opSwap();
    void opSwi();
    void opInvalid();

    std::vector<uint8_t> m_prg, m_data;
    uint8_t m_ram[RAM_SIZE] = {};

    // The ARM's state (Mesen's ArmV3CpuState)
    uint32_t m_op = 0;
    uint32_t m_r[16] = {};
    Psr m_cpsr;
    uint32_t m_userRegs[7] = {}, m_fiqRegs[7] = {}, m_irqRegs[2] = {}, m_svcRegs[2] = {}, m_abtRegs[2] = {}, m_undRegs[2] = {};
    Psr m_fiqSpsr, m_irqSpsr, m_svcSpsr, m_abtSpsr, m_undSpsr;
    struct Slot { uint32_t addr = 0, op = 0; };
    Slot m_fetch, m_decode, m_execute;
    bool m_reload = false;
    uint8_t m_pipeMode = 0;
    uint64_t m_cycles = 0;

    // The SNES link (Mesen's St018State)
    bool m_hasDataForSnes = false, m_hasDataForArm = false, m_armReset = false, m_ack = false;
    uint8_t m_dataSnes = 0, m_dataArm = 0;
};

}  // namespace snes
