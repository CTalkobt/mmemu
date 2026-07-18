#include "sid_test_programs.h"
#include <sstream>
#include <iomanip>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>

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

std::vector<uint8_t> SIDTestProgramGenerator::generateBinary(TestType type, const std::string& ca65Path) {
    std::vector<uint8_t> binary;

    // Generate assembly source
    std::string asmSource = generateAssembly(type);
    if (asmSource.empty()) {
        return binary;  // Failed to generate assembly
    }

    // Use static counter for unique temp file names
    static int counter = 0;
    std::ostringstream baseName;
    baseName << "/tmp/sid_test_" << getpid() << "_" << (counter++);

    std::string asmFile = baseName.str() + ".s";
    std::string objPath = baseName.str() + ".o";
    std::string binPath = baseName.str() + ".bin";

    // Write assembly to temp file
    FILE* f = fopen(asmFile.c_str(), "w");
    if (!f) {
        return binary;
    }

    fprintf(f, "%s", asmSource.c_str());
    fclose(f);

    // Invoke ca65 assembler
    std::ostringstream cmd;
    cmd << ca65Path << " " << asmFile << " -o " << objPath << " 2>/dev/null";

    int ret = system(cmd.str().c_str());
    if (ret != 0) {
        // Assembly failed
        unlink(asmFile.c_str());
        return binary;
    }

    // Link with ld65 to create binary
    // ld65 config for C64 program at $0800
    std::ostringstream ld_cmd;
    ld_cmd << "ld65 " << objPath << " -t c64 -o " << binPath << " 2>/dev/null";

    ret = system(ld_cmd.str().c_str());
    if (ret != 0) {
        // Try without linking (use object file directly)
        // For simple programs, ca65 output object can be read directly
        FILE* obj = fopen(objPath.c_str(), "rb");
        if (obj) {
            fseek(obj, 0, SEEK_END);
            long size = ftell(obj);
            fseek(obj, 0, SEEK_SET);

            binary.resize(size);
            fread(binary.data(), 1, size, obj);
            fclose(obj);
        }
    } else {
        // Read binary from linked output
        FILE* bin = fopen(binPath.c_str(), "rb");
        if (bin) {
            fseek(bin, 0, SEEK_END);
            long size = ftell(bin);
            fseek(bin, 0, SEEK_SET);

            // Skip C64 two-byte header (load address)
            uint16_t loadAddr = 0;
            if (fread(&loadAddr, 2, 1, bin) == 1) {
                size -= 2;
                binary.resize(size);
                fread(binary.data(), 1, size, bin);
            }
            fclose(bin);
        }
    }

    // Clean up temp files
    unlink(asmFile.c_str());
    unlink(objPath.c_str());
    unlink(binPath.c_str());

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
    asm_code << "; Results: 16 bytes at $2000 (one per resonance level 0-15)\n\n";

    asm_code << ".org $0800\n\n";

    // Initialize SID voice 1: sawtooth wave at 1 kHz
    asm_code << "; Setup voice 1 (sawtooth at ~1 kHz)\n";
    asm_code << genSIDWrite(0x00, 0xE8);  // Freq low ($D400)
    asm_code << genSIDWrite(0x01, 0x03);  // Freq hi ($D401)
    asm_code << genSIDWrite(0x02, 0x00);  // Pulse width low ($D402)
    asm_code << genSIDWrite(0x03, 0x08);  // Pulse width hi ($D403)

    // ADSR: fast attack, medium decay, max sustain, fast release
    asm_code << genSIDWrite(0x05, 0x09);  // AD ($D405)
    asm_code << genSIDWrite(0x06, 0xF0);  // SR ($D406)

    // Voice 1 gate on, sawtooth waveform
    asm_code << genSIDWrite(0x04, 0x21);  // Ctrl ($D404): gate=1, saw=0x20

    // Filter setup: LP mode, cutoff=$0500, voice 1 routed
    asm_code << genSIDWrite(0x15, 0x00);  // Filter Freq Lo ($D415)\n";
    asm_code << genSIDWrite(0x16, 0x20);  // Filter Freq Hi ($D416)\n";
    asm_code << genSIDWrite(0x18, 0x10);  // Filter Mode/Vol ($D418): LP=0x10\n";

    asm_code << "\n; Main resonance sweep loop\n";
    asm_code << "LDX #0                  ; Resonance level counter (0-15)\n";
    asm_code << "RLOOP:\n";

    // Set resonance and voice 1 filter enable
    asm_code << "  TXA                 ; Load resonance value\n";
    asm_code << "  ASL                 ; Shift left 4 times\n";
    asm_code << "  ASL                 ; to bits 4-7\n";
    asm_code << "  ASL\n";
    asm_code << "  ASL\n";
    asm_code << "  ORA #$01            ; OR in voice 1 filter bit\n";
    asm_code << "  STA $D417           ; Set filter control ($D417)\n";

    // Wait for filter to settle (~256 cycles)
    asm_code << "  LDY #0\n";
    asm_code << "WAIT_OUTER:\n";
    asm_code << "    LDA #$FF\n";
    asm_code << "  WAIT:\n";
    asm_code << "    DEA\n";
    asm_code << "    BNE WAIT\n";
    asm_code << "    DEY\n";
    asm_code << "    BNE WAIT_OUTER\n";

    // Read OSC3 output and store at result address
    asm_code << "  LDA $D41B           ; Read OSC3 output\n";
    asm_code << "  STA $2000,X         ; Store at $2000+X\n";

    // Next resonance level
    asm_code << "  INX\n";
    asm_code << "  CPX #16             ; Loop while X < 16\n";
    asm_code << "  BNE RLOOP\n";

    // Done - halt
    asm_code << "  JMP $               ; Infinite loop at end\n";

    return asm_code.str();
}

std::string SIDTestProgramGenerator::generateCutoffSweepAssembly(bool is8580) {
    std::ostringstream asm_code;

    asm_code << "; SID6581 Cutoff Frequency Sweep Test\n";
    asm_code << "; Tests filter frequency tracking with fixed resonance\n";
    asm_code << "; Results: 16 bytes at $2000 (one per cutoff step)\n\n";

    asm_code << ".org $0800\n\n";

    // Initialize SID voice 1: sawtooth wave at 1 kHz
    asm_code << "; Setup voice 1 (sawtooth at ~1 kHz)\n";
    asm_code << genSIDWrite(0x00, 0xE8);  // Freq low
    asm_code << genSIDWrite(0x01, 0x03);  // Freq hi
    asm_code << genSIDWrite(0x05, 0x09);  // AD
    asm_code << genSIDWrite(0x06, 0xF0);  // SR
    asm_code << genSIDWrite(0x04, 0x21);  // Gate + Sawtooth

    // Filter with fixed resonance (8) and LP mode
    asm_code << genSIDWrite(0x17, 0x80);  // Res=8, Voice 1 on ($D417)\n";
    asm_code << genSIDWrite(0x18, 0x10);  // Mode/Vol ($D418)\n";

    asm_code << "\n; Cutoff frequency sweep loop\n";
    asm_code << "LDX #0                  ; Frequency step counter (0-15)\n";
    asm_code << "FCLOOP:\n";

    // Set cutoff frequency
    // For sweep: FC = $100 + X*$200 gives range ~$100 to $1E00
    asm_code << "  TXA\n";
    asm_code << "  ASL                 ; X * 2\n";
    asm_code << "  STA $D416           ; High byte of filter freq\n";
    asm_code << "  LDA #$00\n";
    asm_code << "  STA $D415           ; Low byte = 0\n";

    // Wait for filter to settle
    asm_code << "  LDY #0\n";
    asm_code << "FC_WAIT:\n";
    asm_code << "    LDA #$FF\n";
    asm_code << "  FC_WAIT_IN:\n";
    asm_code << "    DEA\n";
    asm_code << "    BNE FC_WAIT_IN\n";
    asm_code << "    DEY\n";
    asm_code << "    BNE FC_WAIT\n";

    // Read OSC3 and store
    asm_code << "  LDA $D41B           ; Read OSC3\n";
    asm_code << "  STA $2000,X         ; Store result\n";

    // Next step
    asm_code << "  INX\n";
    asm_code << "  CPX #16\n";
    asm_code << "  BNE FCLOOP\n";

    asm_code << "  JMP $               ; Halt\n";

    return asm_code.str();
}

std::string SIDTestProgramGenerator::generateCombinedWaveformsAssembly() {
    std::ostringstream asm_code;

    asm_code << "; SID6581 Combined Waveforms Test\n";
    asm_code << "; Tests hardwired waveform mixing (tri+pulse, saw+pulse)\n";
    asm_code << "; Results: OSC3 at different waveform combinations\n\n";

    asm_code << ".org $0800\n\n";

    asm_code << "; Setup voice 1 at 1 kHz\n";
    asm_code << genSIDWrite(0x00, 0xE8);  // Freq low
    asm_code << genSIDWrite(0x01, 0x03);  // Freq hi
    asm_code << genSIDWrite(0x02, 0x80);  // Pulse width (middle)
    asm_code << genSIDWrite(0x03, 0x08);  // Pulse width hi
    asm_code << genSIDWrite(0x05, 0x09);  // AD
    asm_code << genSIDWrite(0x06, 0xF0);  // SR

    // Filter with resonance 12
    asm_code << genSIDWrite(0x17, 0xC1);  // Res=12, Voice 1 on
    asm_code << genSIDWrite(0x18, 0x10);  // LP mode

    asm_code << "; Test waveform combinations\n";
    asm_code << "LDX #0                  ; Waveform counter\n";
    asm_code << "WVLOOP:\n";

    // Waveform patterns: 0=saw, 1=tri+pulse, 2=saw+pulse, 3=tri+pulse+saw
    asm_code << "  CPX #0\n";
    asm_code << "  BNE WV1\n";
    asm_code << "  LDA #$21            ; Sawtooth\n";
    asm_code << "  JMP WV_SET\n";
    asm_code << "WV1:\n";
    asm_code << "  CPX #1\n";
    asm_code << "  BNE WV2\n";
    asm_code << "  LDA #$31            ; Triangle + Pulse\n";
    asm_code << "  JMP WV_SET\n";
    asm_code << "WV2:\n";
    asm_code << "  CPX #2\n";
    asm_code << "  BNE WV3\n";
    asm_code << "  LDA #$61            ; Sawtooth + Pulse\n";
    asm_code << "  JMP WV_SET\n";
    asm_code << "WV3:\n";
    asm_code << "  LDA #$71            ; All waveforms\n";
    asm_code << "WV_SET:\n";
    asm_code << "  STA $D404           ; Set control register\n";

    // Wait and sample
    asm_code << "  LDY #$FF\n";
    asm_code << "WV_WAIT:\n";
    asm_code << "    DEY\n";
    asm_code << "    BNE WV_WAIT\n";

    asm_code << "  LDA $D41B           ; Read OSC3\n";
    asm_code << "  STA $2000,X         ; Store at $2000+X\n";

    asm_code << "  INX\n";
    asm_code << "  CPX #4\n";
    asm_code << "  BNE WVLOOP\n";

    asm_code << "  JMP $               ; Halt\n";

    return asm_code.str();
}

std::string SIDTestProgramGenerator::generateHighResonanceSaturationAssembly() {
    std::ostringstream asm_code;

    asm_code << "; SID6581 High-Resonance Saturation Test\n";
    asm_code << "; Tests soft-clipping behavior at max resonance\n";
    asm_code << "; Measures OSC3 output with resonance=15 at different volumes\n\n";

    asm_code << ".org $0800\n\n";

    // Voice 1: sawtooth at 1 kHz
    asm_code << genSIDWrite(0x00, 0xE8);  // Freq low
    asm_code << genSIDWrite(0x01, 0x03);  // Freq hi
    asm_code << genSIDWrite(0x05, 0x09);  // AD
    asm_code << genSIDWrite(0x06, 0xF0);  // SR
    asm_code << genSIDWrite(0x04, 0x21);  // Sawtooth + gate

    // Filter: LP mode, max resonance (15), voice 1 on
    asm_code << genSIDWrite(0x15, 0x00);  // Filter freq low
    asm_code << genSIDWrite(0x16, 0x10);  // Filter freq hi (low cutoff)
    asm_code << genSIDWrite(0x17, 0xF1);  // Res=15, Voice 1 on

    asm_code << "\n; Test with different filter modes\n";
    asm_code << "LDX #0                  ; Mode counter\n";
    asm_code << "SATLOOP:\n";

    // Test different filter modes with max resonance
    asm_code << "  TXA\n";
    asm_code << "  ASL                 ; A = mode * 2 (bits 4-5)\n";
    asm_code << "  ASL\n";
    asm_code << "  ASL\n";
    asm_code << "  ASL\n";
    asm_code << "  ORA #$F1            ; OR in resonance=15, voice 1\n";
    asm_code << "  STA $D417           ; Set filter control\n";

    // Wait
    asm_code << "  LDY #$FF\n";
    asm_code << "SAT_WAIT:\n";
    asm_code << "    DEY\n";
    asm_code << "    BNE SAT_WAIT\n";

    asm_code << "  LDA $D41B           ; Read OSC3\n";
    asm_code << "  STA $2000,X         ; Store result\n";

    asm_code << "  INX\n";
    asm_code << "  CPX #4\n";
    asm_code << "  BNE SATLOOP\n";

    asm_code << "  JMP $               ; Halt\n";

    return asm_code.str();
}

std::string SIDTestProgramGenerator::genSIDWrite(uint8_t reg, uint8_t value) {
    std::ostringstream ss;
    ss << "  LDA #$" << std::hex << std::setfill('0') << std::setw(2) << (int)value << "\n";
    ss << "  STA $D4" << std::hex << std::setfill('0') << std::setw(2) << (int)reg << "\n";
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
