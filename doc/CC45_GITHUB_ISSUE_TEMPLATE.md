# GitHub Issue Template for cc45 Compiler

## Issue Title

```
[Feature] Debug Metadata Emission for mmemu Debugger Integration
```

## Issue Description

### Overview

Add support for emitting structured debug metadata as assembly comments during code generation. This enables mmemu's debugger to display variable names, types, and memory locations during program execution.

### Motivation

The mmemu multi-machine simulator includes a sophisticated debugging infrastructure with:
- Variable inspection by name
- Type-aware memory display  
- Stack frame analysis
- IDE integration (VS Code, CLion)
- GDB protocol support

Currently, this information is unavailable in cc45-compiled programs. Adding debug metadata emission will enable full symbol-aware debugging across all mmemu frontends (CLI, GUI, MCP).

### Technical Requirements

Emit structured metadata as assembly comments in the following format:

```
; .debug_var: function_name var_name offset=N size=N type=TYPE scope=SCOPE [src_line=N] [src_file=FILE] [name=DISPLAY_NAME]
; .debug_struct: struct_name field1:offset:size:type field2:offset:size:type ...
```

#### Variable Metadata Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `function_name` | string | Yes | Function name (with `_` prefix, `@global` for globals) |
| `var_name` | string | Yes | Internal/compiler variable name |
| `offset` | decimal | Yes | Byte offset (frame-relative for locals, absolute for globals) |
| `size` | decimal | Yes | Size in bytes |
| `type` | string | Yes | Type identifier (int8, int16, int32, uint8, uint16, uint32, char, ptr, struct_NAME, array[N]) |
| `scope` | enum | Yes | One of: `parameter`, `local`, `global` |
| `src_line` | decimal | Optional | Source code line number (-1 if unknown) |
| `src_file` | string | Optional | Source filename |
| `name` | string | Optional | User-friendly display name |

#### Type Identifiers

Standard types:
- `int8`, `I8` — signed 8-bit
- `int16`, `I16` — signed 16-bit
- `int32`, `I32` — signed 32-bit
- `uint8`, `U8` — unsigned 8-bit
- `uint16`, `U16` — unsigned 16-bit
- `uint32`, `U32` — unsigned 32-bit
- `char` — character
- `ptr` or `T*` — pointer
- `struct_NAME` — struct reference
- `type[N]` — array type

### Example Output

For this C code:

```c
int add(int x, int y) {
    int result = x + y;
    return result;
}

int global_counter = 0;
```

Expected assembly output:

```asm
_add:
    ; .debug_var: _add @_p_x offset=10 size=2 type=int16 scope=parameter name=x
    ; .debug_var: _add @_p_y offset=12 size=2 type=int16 scope=parameter name=y
    ; .debug_var: _add _l_result offset=8 size=2 type=int16 scope=local name=result
    .func_flags stack_call
    ; ... function body ...
    rts

_global_counter:
    .byte 0
    ; .debug_var: @global _global_counter offset=2000 size=1 type=int32 scope=global name=global_counter
```

### Integration Points

Metadata emission should occur at:

1. **Function Entry** (after `.func_flags` directive)
   - Emit parameter metadata
   - Emit local variable metadata

2. **Global Variable Definition**
   - Emit global variable metadata with absolute addresses

3. **Optional: Source Mapping**
   - Include `src_line` and `src_file` fields when available
   - Enables IDE source link highlighting

### Acceptance Criteria

- [x] Metadata emitted as assembly comments (non-functional, no binary impact)
- [x] All function parameters have metadata
- [x] All local variables have metadata
- [x] All global variables have metadata
- [x] Type strings correctly formatted (with pointer/array modifiers)
- [x] Frame offsets correctly calculated
- [x] Assembly output remains valid (comments don't break assembler)
- [x] Unit tests for type formatting
- [x] Integration tests: compile + load in mmemu
- [x] No performance regression
- [x] Documentation updated (CHANGELOG, implementation notes)

### Testing Strategy

#### Unit Tests

```cpp
TEST("debug_metadata_type_formatting") {
    // int → "int32", int* → "int32*", int[10] → "int32[10]"
}

TEST("debug_metadata_function_parameters") {
    // Verify metadata for stack and ZP calling conventions
}

TEST("debug_metadata_local_variables") {
    // Verify frame offset calculation and size
}

TEST("debug_metadata_global_variables") {
    // Verify absolute address and global scope
}
```

#### Integration Test

```bash
# Compile test program
./cc45 -c test.c -o test.o45

# Verify metadata in assembly
grep "\.debug_var:" test.s45

# Load in mmemu and verify parsing
./mmemu-cli
> create c64
> load-debug-metadata test.s45
> vars
```

### Implementation Notes

- **Backward Compatibility**: Metadata is optional (comments only)
- **Complexity**: Low (50-70 lines in CodeGenerator)
- **Performance**: Zero runtime overhead (comments stripped by assembler)
- **Files**: `src/main/CodeGenerator.cpp`, `src/main/CodeGenerator.h`

### Related Issues

- mmemu Issue #98: Debug Metadata Format Specification
- mmemu Issue #100: GDB IDE Integration
- mmemu Issue #92: Symbol Import Infrastructure

### References

- [mmemu DEBUG_METADATA.md](../../../mmsim/doc/DEBUG_METADATA.md)
- [Implementation Plan](../../../mmsim/doc/CC45_DEBUG_METADATA_INTEGRATION.md)
- [Variable Symbol Infrastructure](../../../mmsim/doc/ISSUE_94_VARIABLE_SYMBOLS.md)

---

## Comments

This feature enables mmemu's symbol-aware debugging infrastructure to work seamlessly with cc45-compiled programs, providing source-level variable inspection, type-aware memory display, and IDE integration without requiring external debug symbol files.

The metadata format is language and toolchain independent, allowing other tools (assemblers, linkers, debuggers) to emit and consume the same structured information.
