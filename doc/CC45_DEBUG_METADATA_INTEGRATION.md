# cc45 Compiler: Debug Metadata Emission Integration

## Overview

This document describes the implementation plan for adding debug metadata emission to the cc45 compiler. Debug metadata enables mmemu's debugging infrastructure to display variable names, types, and memory locations during program execution.

**Status:** Implementation Plan  
**Target Release:** cc45 v1.0.6  
**Complexity:** Medium (40-60 lines of code change)

## Requirement

cc45 must emit structured debug metadata as assembly comments during code generation. This metadata follows the format specification defined in mmemu's `DEBUG_METADATA.md`:

```
; .debug_var: function_name var_name offset=N size=N type=TYPE scope=SCOPE [src_line=N] [src_file=FILE] [name=DISPLAY_NAME]
; .debug_struct: struct_name field1:offset:size:type field2:offset:size:type ...
```

## Current State

### CodeGenerator Architecture

The `CodeGenerator` class in `src/main/CodeGenerator.cpp` maintains:

1. **Variable Type Tracking**:
   - `variableTypes` (local/parameter variables per function)
   - `globalVariableTypes` (global variables)
   - `VarInfo` struct containing: type, pointer level, signedness, volatility, array dimensions

2. **Frame Layout Information**:
   - `frameLocals_` map: variable name → frame offset
   - `frameSize_` current frame size
   - `localOffsets_` tracked per function

3. **Function Context**:
   - `functionStack_` tracking nested function scopes
   - `currentFunction` pointer
   - Per-function parameter information

### Strategic Emission Points

| Location | Purpose | Available Info |
|----------|---------|-----------------|
| After `.func_flags` (line ~1430, 1562) | Function entry marker | Function name, calling convention, leaf status |
| Parameter processing (line ~1257) | Parameter metadata | Name, type, offset, source location |
| Local variable allocation (line ~1327) | Local variable metadata | Name, type, size, frame offset |
| Global variable processing (line ~1776) | Global metadata | Name, type, absolute address, size |

## Implementation Plan

### Phase 1: Type String Formatter (10 lines)

Create a helper function to format type information:

```cpp
class CodeGenerator {
private:
    std::string formatTypeString(const std::string& type, int pointerLevel, 
                                  const std::vector<int>& arrayDims);
};
```

**Logic**:
- Simple types: `int` → `"I32"`, `char` → `"char"`, etc.
- Pointer types: Add `*` suffix for each pointer level
- Array types: Append `[N]` for each array dimension
- Struct types: Prefix with `struct_`

**Examples**:
- `int` (2 bytes) → `"int16"`
- `int*` (pointer to int) → `"int16*"`
- `int[10]` → `"int16[10]"`
- `Point` struct → `"struct_Point"`

### Phase 2: Variable Metadata Emitter (20-30 lines)

Add methods to emit variable metadata:

```cpp
class CodeGenerator {
private:
    void emitDebugVariable(
        const std::string& functionName,
        const std::string& varName,
        uint32_t offset,
        const std::string& type,
        const std::string& scope,
        int srcLine = -1,
        const std::string& srcFile = "",
        const std::string& displayName = ""
    );
    
    void emitDebugStruct(
        const std::string& structName,
        const std::vector<std::pair<std::string, uint32_t>>& fields
    );
};
```

**Implementation**:
```cpp
void CodeGenerator::emitDebugVariable(
    const std::string& functionName,
    const std::string& varName,
    uint32_t offset,
    const std::string& type,
    const std::string& scope,
    int srcLine,
    const std::string& srcFile,
    const std::string& displayName)
{
    std::stringstream ss;
    ss << "; .debug_var: " << functionName << " " << varName 
       << " offset=" << offset << " size=" << getTypeSize(type) 
       << " type=" << type << " scope=" << scope;
    
    if (srcLine >= 0) ss << " src_line=" << srcLine;
    if (!srcFile.empty()) ss << " src_file=" << srcFile;
    if (!displayName.empty() && displayName != varName) 
        ss << " name=" << displayName;
    
    emit(ss.str());
}
```

### Phase 3: Integration Points (15-20 lines)

**Point 1: Function Entry** (after line 1430, 1562)

```cpp
// After emitting .func_flags
std::string functionName = "_" + node.name;  // Mangled name
// Emit parameter metadata
for (const auto& param : node.parameters) {
    if (variableTypes.count(pName)) {
        VarInfo& vi = variableTypes.at(pName);
        emitDebugVariable(
            functionName,
            pName,
            frameOffsets[pName],
            formatTypeString(vi.type, vi.pointerLevel, {}),
            "parameter",
            param.srcLine,
            srcFile
        );
    }
}

// Emit local variable metadata
for (const auto& [name, offset] : frameLocals_) {
    if (variableTypes.count(name)) {
        VarInfo& vi = variableTypes.at(name);
        emitDebugVariable(
            functionName,
            name,
            offset,
            formatTypeString(vi.type, vi.pointerLevel, {}),
            "local"
        );
    }
}
```

**Point 2: Global Variables** (after line 1776)

```cpp
// In generateGlobalVariable
std::string globalName = "_" + node.name;
emitDebugVariable(
    "@global",
    globalName,
    computeGlobalAddress(node),
    formatTypeString(node.type, node.pointerLevel, node.arrayDims),
    "global",
    node.srcLine,
    node.srcFile,
    node.name
);
```

## Type Size Calculation

Add a helper for computing byte sizes:

```cpp
int CodeGenerator::getTypeSize(const std::string& type) {
    if (type == "int8" || type == "char" || type == "uint8") return 1;
    if (type == "int16" || type == "uint16") return 2;
    if (type == "int32" || type == "uint32" || type.back() == '*') return 4;
    return 1;  // Default
}
```

## Example Output

For this C code:

```c
int add(int x, int y) {
    int result = x + y;
    return result;
}

int global_counter = 0;
```

The compiler should emit:

```asm
_add:
    ; .debug_var: _add @_p_x offset=10 size=2 type=int16 scope=parameter name=x
    ; .debug_var: _add @_p_y offset=12 size=2 type=int16 scope=parameter name=y
    ; .debug_var: _add _l_result offset=8 size=2 type=int16 scope=local name=result
    .func_flags stack_call
    ; ... function body ...
    rts

global_counter:
    .byte 0
    ; .debug_var: @global _global_counter offset=2000 size=1 type=int32 scope=global name=global_counter
```

## Integration with mmemu

Once emitted, mmemu's `DebugMetadataParser` will:

1. **Parse** the comments during assembly load
2. **Store** in `DebugMetadataRegistry`
3. **Display** variable info via:
   - CLI `vars` command
   - GUI VariablePane widget
   - GDB protocol `qMmemuVariables` query
   - MCP `list_variables` tool

## Testing

### Unit Test Cases

```cpp
TEST_CASE("debug_metadata_function_parameters") {
    // Compile function with int parameters
    // Verify metadata emitted for each parameter
}

TEST_CASE("debug_metadata_local_variables") {
    // Compile function with local variables
    // Verify metadata includes proper offsets and types
}

TEST_CASE("debug_metadata_global_variables") {
    // Compile global variables
    // Verify metadata with global scope and absolute addresses
}

TEST_CASE("debug_metadata_array_types") {
    // Compile arrays and verify type strings (e.g., "int16[10]")
}

TEST_CASE("debug_metadata_struct_types") {
    // Compile structs and verify struct_NAME format
}

TEST_CASE("debug_metadata_pointers") {
    // Compile pointer variables and verify * suffix
}
```

### Integration Test

```bash
# Compile with cc45
./cc45 -c test.c -o test.o45

# Check assembly output contains metadata
grep "\.debug_var:" test.s45 | head -5
; .debug_var: _main x offset=10 size=2 type=int16 scope=local

# Load in mmemu
./mmemu-cli -c test.prg
> load-debug-metadata test.s45
> vars main
x = (int16 at offset 10)
y = (int16 at offset 12)
```

## Benefits

1. **Source-Level Debugging** — Variables appear by name in CLI, GUI, MCP
2. **Type-Aware Display** — Proper interpretation of memory as int/char/struct
3. **Frame Analysis** — Stack frame layout visible with variable positions
4. **IDE Integration** — VS Code/CLion can display variable types and values
5. **Zero Overhead** — Metadata is comments only, no runtime cost

## Backward Compatibility

- Metadata is **optional** (assembly comments, ignored if not present)
- Existing cc45 output remains unchanged
- Parser gracefully handles missing metadata
- mmemu works with or without debug metadata

## Effort Estimate

- **Implementation**: 1-2 hours (50-70 lines of code)
- **Testing**: 1 hour (unit + integration tests)
- **Documentation**: 30 minutes (this document + examples)
- **Total**: 2.5-3.5 hours

## Files to Modify

1. `src/main/CodeGenerator.h` — Add method declarations
2. `src/main/CodeGenerator.cpp` — Add implementations + call sites

## References

- mmemu DEBUG_METADATA.md format specification
- cc45 CodeGenerator.cpp variable tracking
- VarInfo struct definition (offset calculation)
- M65Emitter for understanding current output format

## Success Criteria

- [x] Design reviewed (this document)
- [ ] Implementation complete
- [ ] All unit tests passing
- [ ] Integration test with mmemu successful
- [ ] Assembly output contains `.debug_var:` comments
- [ ] mmemu can parse and display variable metadata
- [ ] No performance regression
- [ ] Documentation updated in CHANGELOG.md
