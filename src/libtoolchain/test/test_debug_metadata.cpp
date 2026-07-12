#include "test_harness.h"
#include "libtoolchain/main/debug_metadata.h"

TEST_CASE(debug_metadata_parser_variable) {
    std::string line = "; .debug_var: _main x offset=10 size=2 type=int scope=local";
    DebugVariable var;
    ASSERT(DebugMetadataParser::parseVariableLine(line, var));
    ASSERT_EQ(var.functionName, std::string("_main"));
    ASSERT_EQ(var.internalName, std::string("x"));
    ASSERT_EQ(var.offset, 10u);
    ASSERT_EQ(var.size, 2u);
    ASSERT_EQ(var.type, std::string("int"));
    ASSERT_EQ(var.scope, DebugScope::LOCAL);
}

TEST_CASE(debug_metadata_parser_parameter) {
    std::string line = "; .debug_var: _make_point @_p_x offset=10 size=2 type=int scope=parameter src_line=12 name=p.x";
    DebugVariable var;
    ASSERT(DebugMetadataParser::parseVariableLine(line, var));
    ASSERT_EQ(var.functionName, std::string("_make_point"));
    ASSERT_EQ(var.internalName, std::string("@_p_x"));
    ASSERT_EQ(var.offset, 10u);
    ASSERT_EQ(var.size, 2u);
    ASSERT_EQ(var.displayName, std::string("p.x"));
    ASSERT_EQ(var.scope, DebugScope::PARAMETER);
    ASSERT_EQ(var.srcLine, 12);
}

TEST_CASE(debug_metadata_parser_with_file) {
    std::string line = "; .debug_var: _test __vr0 offset=4 size=2 type=I16 scope=local name=temp src_file=main.c";
    DebugVariable var;
    ASSERT(DebugMetadataParser::parseVariableLine(line, var));
    ASSERT_EQ(var.srcFile, std::string("main.c"));
}

TEST_CASE(debug_metadata_parser_global) {
    std::string line = "; .debug_var: @global counter offset=0 size=1 type=byte scope=global";
    DebugVariable var;
    ASSERT(DebugMetadataParser::parseVariableLine(line, var));
    ASSERT_EQ(var.scope, DebugScope::GLOBAL);
}

TEST_CASE(debug_metadata_parser_reject_invalid) {
    std::string line = "; not a debug line";
    DebugVariable var;
    ASSERT(!DebugMetadataParser::parseVariableLine(line, var));
}

TEST_CASE(debug_metadata_is_metadata_line) {
    ASSERT(DebugMetadataParser::isDebugMetadataLine("; .debug_var: _main x"));
    ASSERT(DebugMetadataParser::isDebugMetadataLine("; .debug_struct: Point"));
    ASSERT(!DebugMetadataParser::isDebugMetadataLine("; regular comment"));
    ASSERT(!DebugMetadataParser::isDebugMetadataLine("not a comment"));
}

TEST_CASE(debug_metadata_registry_add_variable) {
    DebugMetadataRegistry registry;
    DebugVariable var1;
    var1.functionName = "_main";
    var1.internalName = "x";
    var1.displayName = "x";

    registry.addVariable(var1);
    auto vars = registry.getVariablesForFunction("_main");
    ASSERT_EQ(vars.size(), 1u);
    ASSERT_EQ(vars[0].internalName, std::string("x"));
}

TEST_CASE(debug_metadata_registry_struct_fields) {
    DebugMetadataRegistry registry;
    std::vector<DebugStructField> fields;
    DebugStructField f1;
    f1.structName = "Point";
    f1.fieldName = "x";
    f1.fieldOffset = 0;
    f1.fieldSize = 2;
    f1.fieldType = "int";
    fields.push_back(f1);

    registry.addStructFields(fields);
    auto retrieved = registry.getStructFields("Point");
    ASSERT_EQ(retrieved.size(), 1u);
    ASSERT_EQ(retrieved[0].fieldName, std::string("x"));
}

TEST_CASE(debug_metadata_registry_clear) {
    DebugMetadataRegistry registry;
    DebugVariable var;
    var.functionName = "_test";
    registry.addVariable(var);
    ASSERT_EQ(registry.getVariables().size(), 1u);
    registry.clear();
    ASSERT_EQ(registry.getVariables().size(), 0u);
}
