#include "test_harness.h"
#include "libdebug/main/debug_context.h"
#include "libdebug/main/frame_analyzer.h"
#include "libcore/main/machines/machine_registry.h"
#include "libtoolchain/main/variable_symbol.h"

TEST_CASE(frame_analyzer_empty) {
    auto md = MachineRegistry::instance().createMachine("c64");
    ASSERT(md != nullptr);
    ASSERT(!md->cpus.empty());
    ASSERT(!md->buses.empty());

    ICore* cpu = md->cpus[0].cpu;
    IBus* bus = md->buses[0].bus;

    DebugContext dbg(cpu, bus);

    // Test empty frame
    auto layout = FrameLayoutAnalyzer::analyzeCurrentFrame(&dbg, bus, 0x100, 256);
    ASSERT(layout.empty());  // No variables = no frame entries

    delete md;
}

TEST_CASE(frame_analyzer_detect_gaps) {
    std::vector<FrameLayoutEntry> entries;

    // Create entries with a gap between them
    FrameLayoutEntry e1;
    e1.type = FrameLayoutEntry::Type::VARIABLE;
    e1.offset = 0;
    e1.size = 4;
    entries.push_back(e1);

    FrameLayoutEntry e2;
    e2.type = FrameLayoutEntry::Type::VARIABLE;
    e2.offset = 10;  // Gap of 6 bytes
    e2.size = 2;
    entries.push_back(e2);

    auto gaps = FrameLayoutAnalyzer::detectGaps(entries);
    ASSERT(gaps.size() == 1);
    ASSERT(gaps[0].offset == 4);
    ASSERT(gaps[0].size == 6);
    ASSERT(gaps[0].type == FrameLayoutEntry::Type::GAP);
}

TEST_CASE(frame_analyzer_no_gaps) {
    std::vector<FrameLayoutEntry> entries;

    // Create contiguous entries
    FrameLayoutEntry e1;
    e1.type = FrameLayoutEntry::Type::VARIABLE;
    e1.offset = 0;
    e1.size = 4;
    entries.push_back(e1);

    FrameLayoutEntry e2;
    e2.type = FrameLayoutEntry::Type::VARIABLE;
    e2.offset = 4;  // No gap
    e2.size = 2;
    entries.push_back(e2);

    auto gaps = FrameLayoutAnalyzer::detectGaps(entries);
    ASSERT(gaps.empty());
}

TEST_CASE(frame_analyzer_initialization_status_zero) {
    std::vector<FrameLayoutEntry> entries;

    FrameLayoutEntry e;
    e.type = FrameLayoutEntry::Type::VARIABLE;
    e.value = {0, 0, 0, 0};  // All zeros
    entries.push_back(e);

    FrameLayoutAnalyzer::checkInitializationStatus(entries, nullptr);

    ASSERT(entries[0].status == FrameLayoutEntry::Status::UNINITIALIZED);
}

TEST_CASE(frame_analyzer_initialization_status_nonzero) {
    std::vector<FrameLayoutEntry> entries;

    FrameLayoutEntry e;
    e.type = FrameLayoutEntry::Type::VARIABLE;
    e.value = {0x42, 0, 0, 0};  // Non-zero
    entries.push_back(e);

    FrameLayoutAnalyzer::checkInitializationStatus(entries, nullptr);

    ASSERT(entries[0].status == FrameLayoutEntry::Status::INITIALIZED);
}

TEST_CASE(frame_analyzer_format_frame_layout) {
    std::vector<FrameLayoutEntry> entries;

    FrameLayoutEntry e;
    e.type = FrameLayoutEntry::Type::VARIABLE;
    e.displayName = "myvar";
    e.typeStr = "int16";
    e.offset = 0;
    e.size = 2;
    e.value = {0x42, 0x43};
    e.status = FrameLayoutEntry::Status::INITIALIZED;
    entries.push_back(e);

    auto formatted = FrameLayoutAnalyzer::formatFrameLayout(entries, 0x100, 256);

    ASSERT(formatted.find("Stack Frame at FP=$100") != std::string::npos);
    ASSERT(formatted.find("myvar") != std::string::npos);
    ASSERT(formatted.find("int16") != std::string::npos);
    ASSERT(formatted.find("OK") != std::string::npos);
}

TEST_CASE(frame_analyzer_format_struct_definition) {
    std::vector<FrameLayoutEntry> entries;

    FrameLayoutEntry e1;
    e1.type = FrameLayoutEntry::Type::VARIABLE;
    e1.displayName = "x";
    e1.typeStr = "int8";
    e1.offset = 0;
    e1.size = 1;
    e1.value = {42};
    e1.status = FrameLayoutEntry::Status::INITIALIZED;
    entries.push_back(e1);

    FrameLayoutEntry e2;
    e2.type = FrameLayoutEntry::Type::GAP;
    e2.displayName = "[gap]";
    e2.typeStr = "-";
    e2.offset = 1;
    e2.size = 3;
    e2.status = FrameLayoutEntry::Status::UNKNOWN;
    entries.push_back(e2);

    FrameLayoutEntry e3;
    e3.type = FrameLayoutEntry::Type::VARIABLE;
    e3.displayName = "y";
    e3.typeStr = "int32";
    e3.offset = 4;
    e3.size = 4;
    e3.value = {1, 2, 3, 4};
    e3.status = FrameLayoutEntry::Status::INITIALIZED;
    entries.push_back(e3);

    auto formatted = FrameLayoutAnalyzer::formatAsStructDefinition(entries);

    ASSERT(formatted.find("struct _frame") != std::string::npos);
    ASSERT(formatted.find("int8") != std::string::npos);
    ASSERT(formatted.find("x") != std::string::npos);
    ASSERT(formatted.find("gap") != std::string::npos);
    ASSERT(formatted.find("y") != std::string::npos);
    ASSERT(formatted.find("}") != std::string::npos);
}
