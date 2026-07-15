#include "test_harness.h"
#include "libdebug/main/o45_symbol_parser.h"
#include <cstring>

// Helper: Build debug symbols data in the expected format
static std::vector<uint8_t> buildDebugSymbols(
    const std::vector<std::pair<std::string, std::vector<std::pair<std::string, int16_t>>>>& procedures) {
    std::vector<uint8_t> data;

    // Write procedure count (little-endian)
    uint16_t procCount = procedures.size();
    data.push_back(procCount & 0xFF);
    data.push_back((procCount >> 8) & 0xFF);

    // Write each procedure
    for (const auto& [funcName, params] : procedures) {
        // Write function name (null-terminated)
        for (char c : funcName) {
            data.push_back((uint8_t)c);
        }
        data.push_back(0x00);

        // Write parameter count
        data.push_back((uint8_t)params.size());

        // Write each parameter
        for (const auto& [paramName, offset] : params) {
            // Write parameter name (null-terminated)
            for (char c : paramName) {
                data.push_back((uint8_t)c);
            }
            data.push_back(0x00);

            // Write offset (little-endian int16)
            data.push_back(offset & 0xFF);
            data.push_back((offset >> 8) & 0xFF);
        }

        // Write terminator
        data.push_back(0xFF);
    }

    return data;
}

TEST_CASE(o45_symbol_parser_empty_data) {
    std::vector<uint8_t> data;
    std::map<std::string, std::vector<VariableSymbol>> vars;
    ASSERT(!O45SymbolParser::parse(data, vars));
}

TEST_CASE(o45_symbol_parser_single_procedure_no_params) {
    auto data = buildDebugSymbols({
        {"_main", {}}
    });

    std::map<std::string, std::vector<VariableSymbol>> vars;
    ASSERT(O45SymbolParser::parse(data, vars));
    ASSERT_EQ((int)vars.size(), 1);
    ASSERT(vars.count("_main") > 0);
    ASSERT_EQ((int)vars["_main"].size(), 0);
}

TEST_CASE(o45_symbol_parser_single_procedure_with_params) {
    auto data = buildDebugSymbols({
        {"_add", {
            {"@_p_a", 2},
            {"@_p_b", 4}
        }}
    });

    std::map<std::string, std::vector<VariableSymbol>> vars;
    ASSERT(O45SymbolParser::parse(data, vars));
    ASSERT_EQ((int)vars.size(), 1);
    ASSERT(vars.count("_add") > 0);
    ASSERT_EQ((int)vars["_add"].size(), 2);

    const auto& params = vars["_add"];
    ASSERT_EQ(params[0].name, "@_p_a");
    ASSERT_EQ((int)params[0].address, 2);
    ASSERT(params[0].isParameter);
    ASSERT(params[0].isFrameRelative);

    ASSERT_EQ(params[1].name, "@_p_b");
    ASSERT_EQ((int)params[1].address, 4);
    ASSERT(params[1].isParameter);
    ASSERT(params[1].isFrameRelative);
}

TEST_CASE(o45_symbol_parser_multiple_procedures) {
    auto data = buildDebugSymbols({
        {"_add", {
            {"@_p_a", 2},
            {"@_p_b", 4}
        }},
        {"_multiply", {
            {"@_p_x", 2},
            {"@_p_y", 4}
        }}
    });

    std::map<std::string, std::vector<VariableSymbol>> vars;
    ASSERT(O45SymbolParser::parse(data, vars));
    ASSERT_EQ((int)vars.size(), 2);

    ASSERT(vars.count("_add") > 0);
    ASSERT_EQ((int)vars["_add"].size(), 2);
    ASSERT_EQ(vars["_add"][0].name, "@_p_a");

    ASSERT(vars.count("_multiply") > 0);
    ASSERT_EQ((int)vars["_multiply"].size(), 2);
    ASSERT_EQ(vars["_multiply"][0].name, "@_p_x");
}

TEST_CASE(o45_symbol_parser_negative_offset) {
    auto data = buildDebugSymbols({
        {"_func", {
            {"param", -10}
        }}
    });

    std::map<std::string, std::vector<VariableSymbol>> vars;
    ASSERT(O45SymbolParser::parse(data, vars));
    ASSERT(vars.count("_func") > 0);
    ASSERT_EQ((int)vars["_func"].size(), 1);
    ASSERT_EQ((int)(int16_t)vars["_func"][0].address, -10);
}

TEST_CASE(o45_symbol_parser_populate_table) {
    auto data = buildDebugSymbols({
        {"_test", {
            {"x", 2},
            {"y", 4}
        }}
    });

    VariableSymbolTable table;
    ASSERT(O45SymbolParser::populateTable(data, table));

    const auto* var = table.findVariable("_test", "x");
    ASSERT(var != nullptr);
    ASSERT_EQ(var->name, "x");
    ASSERT_EQ((int)var->address, 2);
    ASSERT(var->isParameter);
    ASSERT(var->isFrameRelative);
}

TEST_CASE(o45_symbol_parser_parameter_names_preserved) {
    // Test that various parameter name formats are preserved
    auto data = buildDebugSymbols({
        {"_func", {
            {"@_p_param", 2},
            {"ARG1", 4},
            {"__vr0", 6}
        }}
    });

    std::map<std::string, std::vector<VariableSymbol>> vars;
    ASSERT(O45SymbolParser::parse(data, vars));
    ASSERT_EQ((int)vars["_func"].size(), 3);
    ASSERT_EQ(vars["_func"][0].name, "@_p_param");
    ASSERT_EQ(vars["_func"][1].name, "ARG1");
    ASSERT_EQ(vars["_func"][2].name, "__vr0");
}

TEST_CASE(o45_symbol_parser_large_offset) {
    auto data = buildDebugSymbols({
        {"_func", {
            {"big_offset", 32767}  // Max int16
        }}
    });

    std::map<std::string, std::vector<VariableSymbol>> vars;
    ASSERT(O45SymbolParser::parse(data, vars));
    ASSERT_EQ((int)vars["_func"][0].address, 32767);
}
