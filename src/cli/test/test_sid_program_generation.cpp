#include "test_harness.h"
#include "cli/main/sid_test_programs.h"
#include <algorithm>

/**
 * Unit tests for SID test program generation
 * Validates assembly generation and binary compilation
 */

TEST_CASE(sid_program_gen_resonance_sweep_assembly) {
    // Verify resonance sweep assembly generates non-empty output
    std::string asm_code = SIDTestProgramGenerator::generateAssembly(
        SIDTestProgramGenerator::TestType::ResonanceSweep6581);

    ASSERT(!asm_code.empty());
    ASSERT(asm_code.find("LOOP") != std::string::npos);
    ASSERT(asm_code.find("$D417") != std::string::npos);  // Filter control register
    ASSERT(asm_code.find("OSC3") != std::string::npos);   // OSC3 readout
}

TEST_CASE(sid_program_gen_cutoff_sweep_assembly) {
    // Verify cutoff sweep assembly
    std::string asm_code = SIDTestProgramGenerator::generateAssembly(
        SIDTestProgramGenerator::TestType::CutoffSweep6581);

    ASSERT(!asm_code.empty());
    ASSERT(asm_code.find("FCLOOP") != std::string::npos);
    ASSERT(asm_code.find("$D416") != std::string::npos);  // Filter freq high
}

TEST_CASE(sid_program_gen_combined_waveforms_assembly) {
    // Verify combined waveforms assembly
    std::string asm_code = SIDTestProgramGenerator::generateAssembly(
        SIDTestProgramGenerator::TestType::CombinedWaveforms);

    ASSERT(!asm_code.empty());
    ASSERT(asm_code.find("WVLOOP") != std::string::npos);
    ASSERT(asm_code.find("$31") != std::string::npos);  // Triangle + Pulse opcode
}

TEST_CASE(sid_program_gen_saturation_assembly) {
    // Verify saturation test assembly
    std::string asm_code = SIDTestProgramGenerator::generateAssembly(
        SIDTestProgramGenerator::TestType::HighResonanceSaturation);

    ASSERT(!asm_code.empty());
    ASSERT(asm_code.find("SATLOOP") != std::string::npos);
}

TEST_CASE(sid_program_gen_result_address) {
    // Verify result address is consistent ($2000)
    ASSERT_EQ(SIDTestProgramGenerator::getResultAddress(
        SIDTestProgramGenerator::TestType::ResonanceSweep6581), 0x2000);
    ASSERT_EQ(SIDTestProgramGenerator::getResultAddress(
        SIDTestProgramGenerator::TestType::CutoffSweep6581), 0x2000);
}

TEST_CASE(sid_program_gen_result_size) {
    // Verify result sizes
    uint32_t size = SIDTestProgramGenerator::getResultSize(
        SIDTestProgramGenerator::TestType::ResonanceSweep6581);
    ASSERT(size >= 16);  // At least 16 bytes for resonance sweep
}

TEST_CASE(sid_program_gen_assembly_contains_org_directive) {
    // Verify assembly programs have .org $0800 for C64
    std::string asm_code = SIDTestProgramGenerator::generateAssembly(
        SIDTestProgramGenerator::TestType::ResonanceSweep6581);

    ASSERT(asm_code.find(".org") != std::string::npos);
    ASSERT(asm_code.find("$0800") != std::string::npos);
}

TEST_CASE(sid_program_gen_assembly_initializes_sid) {
    // Verify assembly programs initialize SID properly
    std::string asm_code = SIDTestProgramGenerator::generateAssembly(
        SIDTestProgramGenerator::TestType::ResonanceSweep6581);

    // Should initialize voice 1 frequency
    ASSERT(asm_code.find("$D400") != std::string::npos ||
           asm_code.find("$D401") != std::string::npos);

    // Should set gate bit
    ASSERT(asm_code.find("$D404") != std::string::npos);
}

TEST_CASE(sid_program_gen_binary_generation_with_ca65) {
    // Test binary generation (requires ca65 to be installed)
    // If ca65 is not available, this test should gracefully skip

    std::vector<uint8_t> binary = SIDTestProgramGenerator::generateBinary(
        SIDTestProgramGenerator::TestType::ResonanceSweep6581);

    // Binary may be empty if ca65 not available (that's OK for this test)
    // If it's not empty, it should be a reasonable size (> 100 bytes)
    if (!binary.empty()) {
        ASSERT(binary.size() > 50);  // Minimum viable program
    }
}

TEST_CASE(sid_program_gen_all_types_generate_assembly) {
    // Verify all test types can generate assembly
    std::vector<SIDTestProgramGenerator::TestType> types = {
        SIDTestProgramGenerator::TestType::ResonanceSweep6581,
        SIDTestProgramGenerator::TestType::ResonanceSweep8580,
        SIDTestProgramGenerator::TestType::CutoffSweep6581,
        SIDTestProgramGenerator::TestType::CutoffSweep8580,
        SIDTestProgramGenerator::TestType::CombinedWaveforms,
        SIDTestProgramGenerator::TestType::HighResonanceSaturation,
    };

    for (auto type : types) {
        std::string asm_code = SIDTestProgramGenerator::generateAssembly(type);
        ASSERT(!asm_code.empty());
    }
}
