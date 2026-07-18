#include "sid_test_programs.h"
#include <sstream>

std::string SIDTestProgramGenerator::generateAssembly(TestType type) {
    switch (type) {
        case TestType::ResonanceSweep6581:
            return generateResonanceSweepAssembly(false);
        case TestType::ResonanceSweep8580:
            return generateResonanceSweepAssembly(true);
        case TestType::CutoffSweep6581:
            return generateCutoffSweepAssembly(false);
        case TestType::CutoffSweep8580:
            return generateCutoffSweepAssembly(true);
        case TestType::CombinedWaveforms:
            return generateCombinedWaveformsAssembly();
        case TestType::HighResonanceSaturation:
            return generateHighResonanceSaturationAssembly();
        default:
            return "";
    }
}

std::vector<uint8_t> SIDTestProgramGenerator::generateBinary(TestType type) {
    // TODO: Implement binary generation via ca65 assembler
    std::vector<uint8_t> binary;
    return binary;
}

uint32_t SIDTestProgramGenerator::getResultAddress(TestType type) {
    // Results written to $2000 (8192)
    return 0x2000;
}

uint32_t SIDTestProgramGenerator::getResultSize(TestType type) {
    // Most tests capture 16 resonance levels × 16 bytes per level = 256 bytes
    return 256;
}

std::string SIDTestProgramGenerator::generateResonanceSweepAssembly(bool is8580) {
    std::ostringstream asm_code;

    asm_code << "; SID6581 Resonance Sweep Test\n";
    asm_code << "; Tests resonance effect on filter peak amplitude\n";
    asm_code << "; Results written to $2000\n\n";

    asm_code << ".org $0800\n\n";

    // Initialize SID voice 1: sawtooth wave, 1 kHz
    asm_code << "; Setup voice 1: sawtooth at 1 kHz\n";
    asm_code << genSIDWrite(0x00, 0xE8);  // Frequency low byte ($E8 for ~1 kHz)
    asm_code << genSIDWrite(0x01, 0x03);  // Frequency high byte
    asm_code << genSIDWrite(0x02, 0x00);  // Pulse width low
    asm_code << genSIDWrite(0x03, 0x08);  // Pulse width high

    // Attack/Decay (fast attack, medium decay)
    asm_code << genSIDWrite(0x05, 0x09);

    // Sustain/Release (sustain at max, fast release)
    asm_code << genSIDWrite(0x06, 0xF0);

    // Gate on + Sawtooth waveform
    asm_code << genSIDWrite(0x04, 0x21);  // Gate=1, Wave=Sawtooth

    // Setup filter: LP, FC=$500 (mid-range), voice 1 routed
    asm_code << genSIDWrite(0x15, 0x00);  // FC_LO
    asm_code << genSIDWrite(0x16, 0x20);  // FC_HI
    asm_code << genSIDWrite(0x18, 0x10);  // Mode=LP, Volume=max

    asm_code << "; Sweep resonance 0→15\n";
    asm_code << "LDX #0              ; Resonance counter\n";
    asm_code << "LOOP:\n";

    // Set resonance (bits 4-7 of $D417)
    asm_code << "  TXA\n";
    asm_code << "  ASL\n";
    asm_code << "  ASL\n";
    asm_code << "  ASL\n";
    asm_code << "  ASL\n";
    asm_code << "  ORA #$01           ; Voice 1 filter on\n";
    asm_code << "  STA $D417\n";

    // Wait a bit for stabilization
    asm_code << "  LDY #$FF\n";
    asm_code << "WAIT:\n";
    asm_code << "  DEY\n";
    asm_code << "  BNE WAIT\n";

    // Read OSC3 and store at $2000 + X*16
    asm_code << "  LDA $D41B          ; OSC3\n";
    asm_code << "  STA $2000,X\n";

    asm_code << "  INX\n";
    asm_code << "  CPX #16\n";
    asm_code << "  BNE LOOP\n";

    // Halt
    asm_code << "  JMP $\n";

    return asm_code.str();
}

std::string SIDTestProgramGenerator::generateCutoffSweepAssembly(bool is8580) {
    std::ostringstream asm_code;

    asm_code << "; SID6581 Cutoff Frequency Sweep Test\n";
    asm_code << "; Tests frequency tracking of filter\n";
    asm_code << "; Results written to $2000\n\n";

    asm_code << ".org $0800\n\n";

    // Similar setup to resonance sweep but sweeps cutoff instead

    return asm_code.str();
}

std::string SIDTestProgramGenerator::generateCombinedWaveformsAssembly() {
    std::ostringstream asm_code;

    asm_code << "; SID6581 Combined Waveforms Test\n";
    asm_code << "; Tests triangle+pulse, saw+pulse combinations\n\n";

    asm_code << ".org $0800\n\n";

    return asm_code.str();
}

std::string SIDTestProgramGenerator::generateHighResonanceSaturationAssembly() {
    std::ostringstream asm_code;

    asm_code << "; SID6581 High-Resonance Saturation Test\n";
    asm_code << "; Tests soft-clipping behavior at max resonance\n\n";

    asm_code << ".org $0800\n\n";

    return asm_code.str();
}

std::string SIDTestProgramGenerator::genSIDWrite(uint8_t reg, uint8_t value) {
    std::ostringstream ss;
    ss << "  LDA #$" << std::hex << (int)value << "\n";
    ss << "  STA $D4" << std::hex << (int)reg << "\n";
    return ss.str();
}

std::string SIDTestProgramGenerator::genFrequencySweep(
    uint16_t startHz, uint16_t endHz, uint16_t steps) {
    // TODO: Generate frequency sweep loop
    return "";
}

std::string SIDTestProgramGenerator::genCaptureOutput(
    uint32_t addr, uint16_t sampleCount) {
    // TODO: Generate output capture code
    return "";
}
